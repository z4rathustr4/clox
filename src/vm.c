/**
 * @file vm.c
 * @brief Bytecode virtual machine: value stack, call frames, upvalues and the
 *        interpreter loop.
 *
 *  Still work in progress.
 */

#include "vm.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "chunk.h"
#include "common.h"
#include "compiler.h"
#include "debug.h"
#include "hash.h"
#include "memory.h"
#include "object.h"
#include "value.h"

/**
 * @brief The single global VM instance.
 *
 * Fields used in this file:
 *  - stack / stackTop:   value stack (stackTop points one PAST the top value)
 *  - frames / frameCount: call-frame stack, one CallFrame per active call
 *  - globals:            global variables (name -> value)
 *  - strings:            string-intern set (every ObjString is unique)
 *  - openUpvalues:       list of upvalues still pointing into the stack,
 *                        sorted by stack address, descending
 *  - objects:            list of every heap object (for the GC / shutdown)
 *  - grayStack, grayCount, grayCapacity: GC worklist
 */
VM vm;

/* ======================================================================
 * Native functions
 * ====================================================================== */

/**
 * @brief Native `clock()`: CPU time used by the process, in seconds.
 * @param argCount Number of arguments passed (unused; takes none).
 * @param args     Pointer to the first argument on the VM stack (unused).
 * @return Elapsed CPU time as a number Value.
 */
static Value clockNative(int argCount, Value* args) {
  return NUMBER_VAL((double)clock() / CLOCKS_PER_SEC);
}

/* ======================================================================
 * Stack and error handling
 * ====================================================================== */

/**
 * @brief Resets execution state: empty value stack, no call frames, no open
 *        upvalues. Called at startup and after every runtime error.
 */
static void resetStack() {
  vm.stackTop = vm.stack;
  vm.frameCount = 0;
  vm.openUpvalues = NULL;
}

/**
 * @brief Reports a runtime error with a stack trace, then resets the VM.
 *
 * Prints the printf-style message, then one "[line N] in fn()" entry per
 * active frame, innermost first. Each frame's `ip` has already advanced past
 * the failing instruction, hence the `- 1` when mapping it to a source line
 * through the chunk's parallel `lines` array.
 *
 * @param format printf-style format string.
 * @param ...    Format arguments.
 */
static void runtimeError(const char* format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputs("\n", stderr);

  for (int i = vm.frameCount - 1; i >= 0; i--) {
    CallFrame* frame = &vm.frames[i];
    ObjFunction* function = frame->closure->function;
    size_t instruction = frame->ip - function->chunk.code - 1;
    fprintf(stderr, "[line %d] in ", function->chunk.lines[instruction]);
    if (function->name == NULL) {
      fprintf(stderr, "script\n");
    } else {
      fprintf(stderr, "%s()\n", function->name->chars);
    }
  }

  resetStack();
}

/**
 * @brief Registers a C function as a global Lox function.
 *
 * The name string and the ObjNative are pushed on the stack while the table
 * is modified so they stay reachable if an allocation triggers a GC. Using
 * vm.stack[0] and vm.stack[1] is valid because the stack is empty when this
 * runs from initVM().
 *
 * @param name     Global name Lox code will use.
 * @param function C implementation.
 */
static void defineNative(const char* name, NativeFn function) {
  push(OBJ_VAL(copyString(name, (int)strlen(name))));
  push(OBJ_VAL(newNative(function)));
  tableSet(&vm.globals, AS_STRING(vm.stack[0]), vm.stack[1]);
  pop();
  pop();
}

/* ======================================================================
 * VM lifecycle
 * ====================================================================== */

/**
 * @brief Initializes the VM: empty stack, no objects, empty GC worklist and
 *        tables, and the built-in natives.
 */
void initVM() {
  resetStack();
  vm.objects = NULL;

  // The gray worklist starts empty; it is grown lazily by markObject() and
  // released in memory.c:freeObjects().
  vm.grayCount = 0;
  vm.grayCapacity = 0;
  vm.grayStack = NULL;

  // Hash tables: globals and the string-intern set.
  initTable(&vm.globals);
  initTable(&vm.strings);

  defineNative("clock", clockNative);
}

/** @brief Frees the VM's tables and every object it still owns. */
void freeVM() {
  freeTable(&vm.globals);
  freeTable(&vm.strings);
  freeObjects();
}

/* ======================================================================
 * Value stack primitives
 * ====================================================================== */

/**
 * @brief Pushes @p value onto the stack.
 * @note No overflow check: depth is bounded by FRAMES_MAX * UINT8_COUNT,
 *       which the stack array is sized for.
 */
void push(Value value) {
  *vm.stackTop = value;
  vm.stackTop++;
}

/**
 * @brief Pops and returns the top value.
 * @note The vacated slot is not cleared, merely considered free.
 */
Value pop() {
  vm.stackTop--;
  return *vm.stackTop;
}

/**
 * @brief Returns a stack value without popping it.
 * @param distance 0 = top of stack, 1 = one below the top, and so on.
 */
static Value peek(int distance) { return vm.stackTop[-1 - distance]; }

/* ======================================================================
 * Calls
 * ====================================================================== */

/**
 * @brief Begins a call to a Lox closure by pushing a new CallFrame.
 *
 * Expected stack on entry:
 * @code
 *   [ ... | closure | arg0 | ... | argN-1 ]  <- stackTop
 * @endcode
 * The frame's `slots` window starts at the callee slot, so inside the
 * function slot 0 is the closure, slots 1..N are the parameters, and its
 * locals follow.
 *
 * @param closure  Closure being called.
 * @param argCount Arguments supplied by the caller.
 * @return true on success; false (after reporting) on an arity mismatch or
 *         when the frame stack is full (FRAMES_MAX).
 */
static bool call(ObjClosure* closure, int argCount) {
  // wrong argc?
  if (argCount != closure->function->arity) {
    runtimeError("Expected %d arguments but got %d.", closure->function->arity,
                 argCount);
    return false;
  }

  // The frame array is fixed-size (FRAMES_MAX); exceeding it is a stack
  // overflow.
  if (vm.frameCount == FRAMES_MAX) {
    runtimeError("Stack overflow.");
    return false;
  }

  CallFrame* frame = &vm.frames[vm.frameCount++];
  frame->closure = closure;
  frame->ip = closure->function->chunk.code;
  frame->slots = vm.stackTop - argCount - 1;  // window starts at the callee
  return true;
}

/**
 * @brief Call dispatcher: invokes @p callee according to its object type.
 *
 * The callee sits on the stack just below its @p argCount arguments.
 * Closures get a new frame (the interpreter loop then runs it). Natives run
 * immediately here: the arguments and callee are popped and replaced by the
 * native's return value.
 *
 * @param callee   Value being called.
 * @param argCount Number of arguments on the stack above it.
 * @return true if the call was set up or completed; false on error (already
 *         reported), which makes run() abort.
 *
 * @warning A declaration directly after a `case` label is only valid in C23
 *          (or as a compiler extension). Wrap the OBJ_NATIVE case body in
 *          `{ }` for portability.
 */
static bool callValue(Value callee, int argCount) {
  // Only heap objects can be callable.
  if (IS_OBJ(callee)) {
    switch (OBJ_TYPE(callee)) {
      case OBJ_CLOSURE:
        return call(AS_CLOSURE(callee), argCount);
      case OBJ_NATIVE:
        NativeFn native = AS_NATIVE(callee);  // extract the function pointer
        Value result =
            native(argCount, vm.stackTop - argCount);  // args start here
        vm.stackTop -= argCount + 1;  // discard args AND the callee slot
        push(result);                 // replace them with the result
        return true;
      default:
        break;  // non-callable object type (e.g. a string)
    }
  }
  runtimeError("Can only call functions and classes");
  return false;
}

/* ======================================================================
 * Upvalues
 * ====================================================================== */

/**
 * @brief Captures the stack local at @p local as an upvalue.
 *
 * Reuses an existing open upvalue for that slot if there is one, so every
 * closure capturing the same variable shares one ObjUpvalue (and sees each
 * other's writes). Otherwise creates one and splices it into
 * vm.openUpvalues.
 *
 * @invariant vm.openUpvalues is sorted by stack address, DESCENDING (head =
 *            slot nearest the stack top). This lets the search stop early
 *            and lets closeUpvalues() stop at the first slot below its
 *            threshold.
 *
 * @param local Pointer to the captured stack slot.
 * @return The shared (existing or new) open upvalue for @p local.
 */
static ObjUpvalue* captureUpvalue(Value* local) {
  // Trailing pointer, needed to splice a new node in.
  ObjUpvalue* prevUpvalue = NULL;
  // Cursor, starting at the head.
  ObjUpvalue* upvalue = vm.openUpvalues;

  // Walk past upvalues whose slot is ABOVE `local`; stop at the first one at
  // or below it (or the end of the list).
  while (upvalue != NULL && upvalue->location > local) {
    prevUpvalue = upvalue;
    upvalue = upvalue->next;
  }

  // Exact match: share the existing upvalue.
  if (upvalue != NULL && upvalue->location == local) {
    return upvalue;
  }

  // None exists: create an open upvalue pointing at the live stack slot.
  ObjUpvalue* createdUpvalue = newUpvalue(local);
  createdUpvalue->next = upvalue;  // in front of the first lower-or-NULL node

  if (prevUpvalue == NULL) {
    // `local` is above every open upvalue (or the list is empty): new head.
    vm.openUpvalues = createdUpvalue;
  } else {
    // Insert between prevUpvalue and upvalue, preserving descending order.
    prevUpvalue->next = createdUpvalue;
  }

  return createdUpvalue;
}

/**
 * @brief Closes every open upvalue pointing at stack slot @p last or above.
 *
 * "Closing" hoists the variable off the stack: its value is copied into the
 * upvalue's own `closed` field and `location` is repointed at that field, so
 * closures keep working after the stack slot is gone. Because the list is
 * sorted descending, it pops from the head and stops at the first upvalue
 * below @p last.
 *
 * @param last Lowest stack slot that is going away.
 */
static void closeUpvalues(Value* last) {
  while (vm.openUpvalues != NULL && vm.openUpvalues->location >= last) {
    ObjUpvalue* upvalue = vm.openUpvalues;

    upvalue->closed = *upvalue->location;  // hoist value off the stack
    upvalue->location = &upvalue->closed;  // now points at itself
    vm.openUpvalues = upvalue->next;       // unlink from the open list
  }
}

/* ======================================================================
 * Interpreter helpers
 * ====================================================================== */

/** @brief Lox truthiness: only `null` and `false` are falsey (0 and "" are
 *         truthy). */
static bool isFalsey(Value value) {
  return IS_NULL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

/**
 * @brief Pops two strings (b on top, a below) and pushes a + b.
 *
 * Builds the result in a fresh buffer and hands it to takeString(), which
 * interns it (freeing the buffer if an identical string already exists).
 *
 * @warning Both operands are popped BEFORE ALLOCATE, so they are unreachable
 *          if that allocation triggers a collection. Harmless while the GC
 *          frees nothing, but once sweep exists, peek() them and pop only
 *          after the result is built.
 */
static void concatenate() {
  ObjString* b = AS_STRING(pop());
  ObjString* a = AS_STRING(pop());

  int length = a->length + b->length;
  char* chars = ALLOCATE(char, length + 1);
  memcpy(chars, a->chars, a->length);
  memcpy(chars + a->length, b->chars, b->length);
  chars[length] = '\0';

  ObjString* result = takeString(chars, length);
  push(OBJ_VAL(result));
}

/* ======================================================================
 * Interpreter loop
 * ====================================================================== */

/**
 * @brief Main fetch-decode-execute loop.
 *
 * Runs the topmost call frame until the outermost function returns or a
 * runtime error occurs. The local `frame` caches the current CallFrame and
 * MUST be reloaded after anything that changes the frame stack (OP_CALL,
 * OP_RETURN).
 *
 * @return INTERPRET_OK on normal completion, INTERPRET_RUNTIME_ERR on error.
 *
 * @bug The compiler emits OP_LOOP for `while` and `for` (backward jumps), but
 *      no `case OP_LOOP` exists below, so loops cannot execute correctly: the
 *      operand bytes would be decoded as opcodes. Add:
 *      @code
 *      case OP_LOOP: {
 *        uint16_t offset = READ_SHORT();
 *        frame->ip -= offset;
 *        break;
 *      }
 *      @endcode
 * @bug OP_CONST prints every constant as it loads it (leftover debug output).
 */
static InterpretResult run() {
  CallFrame* frame = &vm.frames[vm.frameCount - 1];

// Reads one byte at ip and advances ip.
#define READ_BYTE() (*frame->ip++)

// Reads a 16-bit big-endian operand (used by jumps).
#define READ_SHORT() \
  (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))

// Reads a 1-byte constant index and returns that constant of the current
// function.
#define READ_CONSTANT() \
  (frame->closure->function->chunk.constants.values[READ_BYTE()])

// Reads a constant known to be an ObjString (e.g. a variable name).
#define READ_STRING() AS_STRING(READ_CONSTANT())

// Shared body of the binary numeric operators: type-check both operands (left
// on the stack for peek), pop them (b first, it is on top), apply `op`, push
// the result wrapped by `valueType` (NUMBER_VAL for arithmetic, BOOL_VAL for
// comparisons). The `return` exits run() on a type error.
#define BINARY_OP(valueType, op)                      \
  do {                                                \
    if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) { \
      runtimeError("Operands must be numbers.");      \
      return INTERPRET_RUNTIME_ERR;                   \
    }                                                 \
    double b = AS_NUMBER(pop());                      \
    double a = AS_NUMBER(pop());                      \
    push(valueType(a op b));                          \
  } while (false)

  for (;;) {
#ifdef DEBUG_TRACE_EXECUTION
    // Trace: dump the value stack, then disassemble the instruction about
    // to run (offset = ip - start of bytecode).
    printf("          ");
    for (Value* slot = vm.stack; slot < vm.stackTop; slot++) {
      printf("[ ");
      printValue(*slot);
      printf(" ]");
    }
    printf("\n");

    disassembleInstruction(
        &frame->closure->function->chunk,
        (int)(frame->ip - frame->closure->function->chunk.code));
#endif
    uint8_t instruction;
    // Fetch the opcode and dispatch on it.
    switch (instruction = READ_BYTE()) {
      // ---- Constants and literals --------------------------------------

      // Push a constant from the constant table.
      case OP_CONST: {
        Value constant = READ_CONSTANT();
        printValue(constant);
        printf("\n");
        push(constant);
        break;
      }
      case OP_NULL:
        push(NULL_VAL);
        break;
      case OP_TRUE:
        push(BOOL_VAL(true));
        break;
      case OP_FALSE:
        push(BOOL_VAL(false));
        break;
      // Discard the top of the stack (e.g. an expression statement's value).
      case OP_POP:
        pop();
        break;

      // ---- Local variables ---------------------------------------------

      // Locals live at fixed offsets from the frame's `slots` base, so the
      // operand is just that offset.
      // GET: copy the local onto the top of the stack.
      case OP_GET_LOCAL: {
        uint8_t slot = READ_BYTE();
        push(frame->slots[slot]);
        break;
      }
      // SET: store the top of the stack into the local WITHOUT popping,
      // because assignment is an expression whose result is the value.
      case OP_SET_LOCAL: {
        uint8_t slot = READ_BYTE();
        frame->slots[slot] = peek(0);
        break;
      }

      // ---- Global variables --------------------------------------------

      // Look up a global by name; error if never defined.
      case OP_GET_GLOBAL: {
        ObjString* name = READ_STRING();
        Value value;
        if (!tableGet(&vm.globals, name, &value)) {
          runtimeError("Undefined variable '%s'.", name->chars);
          return INTERPRET_RUNTIME_ERR;
        }
        push(value);
        break;
      }
      // Top-level `let`: store the initializer, THEN pop it (so the value
      // stays reachable for the GC while the table may be resizing).
      case OP_DEFINE_GLOBAL: {
        ObjString* name = READ_STRING();
        tableSet(&vm.globals, name, peek(0));
        pop();
        break;
      }
      // Assignment to a global. tableSet returns true when it created a NEW
      // key, meaning the variable was never declared: undo the accidental
      // insert and report an error. Does not pop (assignment is an
      // expression).
      case OP_SET_GLOBAL: {
        ObjString* name = READ_STRING();
        if (tableSet(&vm.globals, name, peek(0))) {
          tableDelete(&vm.globals, name);
          runtimeError("Undefined variable '%s'.", name->chars);
          return INTERPRET_RUNTIME_ERR;
        }
        break;
      }

      // ---- Upvalues ----------------------------------------------------

      // Operand indexes the CURRENT closure's upvalue array. `location`
      // points into the stack (open) or at the upvalue's own `closed` field
      // (closed), so one dereference handles both.
      case OP_GET_UPVALUE: {
        uint8_t slot = READ_BYTE();

        push(*frame->closure->upvalues[slot]->location);
        break;
      }
      // Writes through `location`; like SET_LOCAL, leaves the value on top.
      case OP_SET_UPVALUE: {
        uint8_t slot = READ_BYTE();

        *frame->closure->upvalues[slot]->location = peek(0);
        break;
      }

      // ---- Comparison and arithmetic -----------------------------------

      // Equality works on any two values (see valuesEqual).
      case OP_EQU: {
        Value b = pop();
        Value a = pop();
        push(BOOL_VAL(valuesEqual(a, b)));
        break;
      }
      // `>` and `<` yield booleans; numbers only.
      case OP_GREATER:
        BINARY_OP(BOOL_VAL, >);
        break;
      case OP_LESS:
        BINARY_OP(BOOL_VAL, <);
        break;
      // `+` is overloaded: string concatenation or numeric addition;
      // mixed types are an error.
      case OP_ADD: {
        if (IS_STRING(peek(0)) && IS_STRING(peek(1))) {
          concatenate();
        } else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
          double b = AS_NUMBER(pop());
          double a = AS_NUMBER(pop());
          push(NUMBER_VAL(a + b));
        } else {
          runtimeError("Must be either two numbers or two strings.");
          return INTERPRET_RUNTIME_ERR;
        }
        break;
      }
      case OP_SUBTRACT:
        BINARY_OP(NUMBER_VAL, -);
        break;
      case OP_MULTIPLY:
        BINARY_OP(NUMBER_VAL, *);
        break;
      case OP_DIVIDE:
        BINARY_OP(NUMBER_VAL, /);
        break;
      // Logical not: replaces the top value with the inverse of its
      // truthiness.
      case OP_NOT:
        push(BOOL_VAL(isFalsey(pop())));
        break;
      // Unary minus: operand must be a number.
      case OP_NEGATE:
        if (!IS_NUMBER(peek(0))) {
          runtimeError("Operand must be a number.");
          return INTERPRET_RUNTIME_ERR;
        }
        push(NUMBER_VAL(-AS_NUMBER(pop())));
        break;

      // ---- Statements --------------------------------------------------

      // `puts`: pop and print the value plus a newline.
      case OP_PUTS: {
        printValue(pop());
        printf("\n");
        break;
      }

      // ---- Control flow ------------------------------------------------

      // Unconditional forward jump: ip += 16-bit offset.
      case OP_JMP: {
        uint16_t offset = READ_SHORT();
        frame->ip += offset;
        break;
      }
      // Conditional forward jump when the top is falsey. The condition is
      // NOT popped; the compiler emits explicit OP_POPs on each path.
      case OP_JMP_IF_FALSE: {
        uint16_t offset = READ_SHORT();
        if (isFalsey(peek(0))) frame->ip += offset;
        break;
      }

      // ---- Functions and closures --------------------------------------

      // Call: operand = argument count; the callee is `argCount` slots below
      // the top. Natives finish immediately; closures push a frame, so
      // `frame` is reloaded to the new top frame.
      case OP_CALL: {
        int argCount = READ_BYTE();
        if (!callValue(peek(argCount), argCount)) {
          return INTERPRET_RUNTIME_ERR;
        }
        frame = &vm.frames[vm.frameCount - 1];
        break;
      }
      // Build a closure at runtime from a compile-time ObjFunction. After the
      // opcode and function constant, the bytecode has one (isLocal, index)
      // pair per upvalue:
      //   isLocal == 1: a local of the ENCLOSING function (frame->slots[idx])
      //                 -> capture it, possibly sharing an open upvalue
      //   isLocal == 0: an upvalue of the enclosing closure
      //                 -> reuse that closure's upvalue idx
      // The closure is pushed BEFORE the loop so the GC can see it while
      // captureUpvalue allocates.
      case OP_CLOSURE: {
        ObjFunction* function = AS_FUNCTION(READ_CONSTANT());
        ObjClosure* closure = newClosure(function);
        push(OBJ_VAL(closure));
        // fill closure
        for (int i = 0; i < closure->upvalueCount; i++) {
          uint8_t isLocal = READ_BYTE();
          uint8_t idx = READ_BYTE();
          if (isLocal) {
            closure->upvalues[i] = captureUpvalue(frame->slots + idx);
          } else {
            closure->upvalues[i] = frame->closure->upvalues[idx];
          }
        }
        break;
      }
      // A captured local is leaving scope while its function continues:
      // close the upvalue for the top-of-stack slot, then pop it.
      case OP_CLOSE_UPVALUE:
        closeUpvalues(vm.stackTop - 1);
        pop();
        break;
      // Return from the current function:
      //  1. pop the return value
      //  2. close every upvalue pointing into this frame's slots
      //  3. pop the call frame
      //  4. if it was the outermost frame (the script): pop the script
      //     closure and finish
      //  5. otherwise drop the whole frame window from the stack (reset
      //     stackTop to the frame base), push the return value for the
      //     caller, and resume the caller's frame
      // `frame` still points at the frame just popped during steps 4-5; that
      // is fine because frameCount-- only shrinks the array bound.
      case OP_RETURN: {
        Value result = pop();
        closeUpvalues(frame->slots);
        vm.frameCount--;
        if (vm.frameCount == 0) {
          pop();
          return INTERPRET_OK;
        }
        vm.stackTop = frame->slots;
        push(result);
        frame = &vm.frames[vm.frameCount - 1];
        break;
      }
    }
  }
#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
#undef BINARY_OP
}

/* ======================================================================
 * Entry point
 * ====================================================================== */

/**
 * @brief Compiles and runs a source string.
 *
 * Compiles to a top-level ObjFunction, wraps it in a closure and calls it
 * with zero arguments (pushing the first CallFrame), then enters run(). The
 * function is pushed while the closure is allocated so the GC cannot free it
 * mid-allocation; the closure then occupies slot 0 of the script's frame.
 *
 * @param source NUL-terminated Lox source text.
 * @return INTERPRET_OK, INTERPRET_COMPILE_ERR or INTERPRET_RUNTIME_ERR.
 */
InterpretResult interpret(const char* source) {
  ObjFunction* function = compile(source);
  if (function == NULL) return INTERPRET_COMPILE_ERR;

  push(OBJ_VAL(function));  // protect from GC during newClosure
  ObjClosure* closure = newClosure(function);
  pop();
  push(OBJ_VAL(closure));  // slot 0 of the script's frame
  call(closure, 0);

  return run();
}
