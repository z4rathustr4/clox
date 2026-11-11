/**
 * @file memory.c
 * @brief Dynamic memory management and (partial) tracing garbage collector.
 *
 * All VM allocation funnels through reallocate(), giving one place to hook
 * the collector. The GC is tri-color mark-sweep: white = unmarked, gray =
 * marked but references not yet traced (held on `vm.grayStack`), black =
 * marked and fully traced.
 *
 * @note Only the root-marking phase exists so far. There is no
 *       trace-references (blacken) step, no sweep, and no weak-reference
 *       cleanup for `vm.strings`, so collectGarbage() currently frees nothing.
 *
 * Still work in progress.
 */

#include "memory.h"

#include <stdlib.h>

#include "common.h"
#include "compiler.h"
#include "object.h"
#include "stdbool.h"
#include "value.h"
#include "vm.h"

#ifdef DEBUG_LOG_GC
#include <stdio.h>

#include "debug.h"
#endif /* ifdef DEBUG_LOG_GC */

/**
 * @brief Single entry point for all heap allocation, growth, shrinkage and
 *        freeing.
 *
 * Behavior by arguments:
 *  - oldSize == 0:            allocate @p newSize bytes
 *  - newSize == 0:            free @p pointer, return NULL
 *  - both nonzero:            resize (contents preserved up to the smaller)
 *
 * Under DEBUG_STRESS_GC a collection runs on every GROWING request, which
 * flushes out missing-root bugs quickly.
 *
 * @param pointer Existing block (NULL for a fresh allocation).
 * @param oldSize Current size in bytes (0 if none).
 * @param newSize Requested size in bytes (0 to free).
 * @return The (possibly moved) block, or NULL after a free. Exits the
 *         process if the system is out of memory.
 */
void* reallocate(void* pointer, size_t oldSize, size_t newSize) {
  if (newSize > oldSize) {
#ifdef DEBUG_STRESS_GC
    collectGarbage();
#endif /* ifdef DEBUG_STRESS_GC */
  }

  if (newSize == 0) {
    free(pointer);
    return NULL;
  }

  void* result = realloc(pointer, newSize);
  if (result == NULL) {
    exit(1);
  }
  return result;
}

/**
 * @brief Marks @p object reachable and queues it for tracing (turns it gray).
 *
 * Appends the object to the gray worklist, growing it with plain realloc()
 * rather than reallocate(): allocating through reallocate() could re-enter
 * the collector in the middle of a collection.
 *
 * @param object Object to mark; NULL is ignored.
 *
 * @bug There is no early return for already-marked objects. Once tracing is
 *      implemented, a reference cycle will push the same object forever.
 *      Add `if (object->isMarked) return;` before marking.
 */
void markObject(Obj* object) {
  if (object == NULL) return;

#ifdef DEBUG_LOG_GC
  printf("%p mark ", (void*)object);
  printValue(OBJ_VAL(object));
  printf("\n");
#endif /* ifdef DEBUG_LOG_GC */

  object->isMarked = true;

  // Add the object to the gray worklist; its outgoing references are
  // visited later during the trace phase.
  if (vm.grayCapacity < vm.grayCount + 1) {
    vm.grayCapacity = GROW_CAPACITY(vm.grayCapacity);
    vm.grayStack = (Obj**)realloc(vm.grayStack, sizeof(Obj*) * vm.grayCapacity);
    if (vm.grayStack == NULL) exit(1);
  }

  vm.grayStack[vm.grayCount++] = object;
}

/** @brief Marks @p value if (and only if) it holds a heap object; numbers,
 *         booleans and null need no marking. */
void markValue(Value value) {
  if (IS_OBJ(value)) markObject(AS_OBJ(value));
}

/**
 * @brief Frees one object and its owned out-of-line storage.
 *
 * Each type releases what it owns: closures free their upvalue pointer array
 * (not the upvalues themselves, which may be shared), functions free their
 * chunk, strings free their character buffer. Natives and upvalues own
 * nothing extra.
 */
static void freeObject(Obj* object) {
#ifdef DEBUG_LOG_GC
  printf("%p free type %d\n", (void*)object, object->type);
#endif /* ifdef DEBUG_LOG_GC */

  switch (object->type) {
    case OBJ_CLOSURE: {
      ObjClosure* closure = (ObjClosure*)object;
      FREE_ARRAY(ObjUpvalue*, closure->upvalues, closure->upvalueCount);
      FREE(ObjClosure, object);
      break;
    }
    case OBJ_FUNCTION: {
      ObjFunction* function = (ObjFunction*)object;
      freeChunk(&function->chunk);
      FREE(ObjFunction, object);
      break;
    }
    case OBJ_NATIVE:
      FREE(ObjNative, object);
      break;
    case OBJ_STRING: {
      ObjString* string = (ObjString*)object;
      FREE_ARRAY(char, string->chars, string->length + 1);
      FREE(ObjString, object);
      break;
    }
    case OBJ_UPVALUE:
      FREE(ObjUpvalue, object);
      break;
  }
}

/**
 * @brief Marks every GC root: everything the running program can reach
 *        directly.
 *
 * Roots: all live stack values; every active call frame's closure; every
 * open upvalue (which are not necessarily reachable from a closure while
 * a variable is still on the stack); global variables; and functions
 * currently being compiled.
 */
static void markRoots() {
  for (Value* slot = vm.stack; slot < vm.stackTop; slot++) {
    markValue(*slot);
  }

  for (int i = 0; i < vm.frameCount; i++) {
    markObject((Obj*)vm.frames[i].closure);
  }

  for (ObjUpvalue* upvalue = vm.openUpvalues; upvalue != NULL;
       upvalue = upvalue->next) {
    markObject((Obj*)upvalue);
  }

  markTable(&vm.globals);
  markCompilerRoots();
}

/**
 * @brief Runs a garbage collection.
 *
 * Currently performs only the root-marking phase (see the @note in the file
 * header). Remaining phases to add: trace references until the gray stack
 * is empty, tableRemoveWhite(&vm.strings), then sweep unmarked objects.
 */
void collectGarbage() {
#ifdef DEBUG_LOG_GC
  printf("-- GC BEGIN\n");
#endif /* ifdef DEBUG_LOG_GC */

  markRoots();

#ifdef DEBUG_LOG_GC
  printf("-- GC END\n");
#endif /* ifdef DEBUG_LOG_GC */
}

/**
 * @brief Frees every object in the VM's all-objects list, plus the gray
 *        worklist. Called once at shutdown.
 *
 * Saves each `next` pointer before freeing the node that holds it.
 */
void freeObjects() {
  Obj* object = vm.objects;

  while (object != NULL) {
    Obj* next = object->next;
    freeObject(object);
    object = next;
  }

  free(vm.grayStack);
}
