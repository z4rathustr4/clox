/**
 * @file object.c
 * @brief Heap object constructors and printing.
 *
 * Every heap object begins with an Obj header (type, GC mark, `next`
 * pointer). Allocation chains each new object onto `vm.objects` so the
 * collector (and freeObjects()) can find all of them.
 *
 * Still work in progress.
 */

#include "object.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "chunk.h"
#include "hash.h"
#include "memory.h"
#include "value.h"
#include "vm.h"

/* --- MACROS --- */

/** @brief Allocates an object of C type @p type tagged with @p objectType,
 *         returning a correctly typed pointer. */
#define ALLOCATE_OBJ(type, objectType) \
  (type*)allocateObject(sizeof(type), objectType)

/**
 * @brief Allocates @p size bytes for a new object and links it into the
 *        VM's object list.
 *
 * Initializes only the Obj header; the caller fills in the type-specific
 * fields. The object starts unmarked (white) for the GC.
 *
 * @param size Total byte size of the concrete object struct.
 * @param type Object type tag.
 * @return Pointer to the new object, viewed as a generic Obj*.
 */
static Obj* allocateObject(size_t size, ObjType type) {
  Obj* object = (Obj*)reallocate(NULL, 0, size);

  object->type = type;
  object->isMarked = false;
  object->next = vm.objects;  // push onto the head of the all-objects list
  vm.objects = object;

#ifdef DEBUG_LOG_GC
  printf("%p allocate %zu for %d\n", (void*)object, size, type);
#endif /* ifdef DEBUG_LOG_GC */

  return object;
}

/**
 * @brief Creates a closure over @p function.
 *
 * Allocates the upvalue pointer array first and NULLs every slot, so the GC
 * never sees uninitialized pointers while the closure is being filled in by
 * OP_CLOSURE.
 *
 * @param function Compiled function to wrap.
 * @return New closure with `function->upvalueCount` empty upvalue slots.
 */
ObjClosure* newClosure(ObjFunction* function) {
  ObjUpvalue** upvalues = ALLOCATE(ObjUpvalue*, function->upvalueCount);
  for (int i = 0; i < function->upvalueCount; i++) {
    upvalues[i] = NULL;
  }

  ObjClosure* closure = ALLOCATE_OBJ(ObjClosure, OBJ_CLOSURE);
  closure->function = function;
  closure->upvalues = upvalues;
  closure->upvalueCount = function->upvalueCount;
  return closure;
}

/** @brief Creates an empty function object (arity 0, no name, empty chunk),
 *         to be filled in by the compiler. */
ObjFunction* newFunction() {
  ObjFunction* function = ALLOCATE_OBJ(ObjFunction, OBJ_FUNCTION);
  function->arity = 0;
  function->upvalueCount = 0;
  function->name = NULL;
  initChunk(&function->chunk);
  return function;
}

/** @brief Wraps a C function pointer as a callable Lox object. */
ObjNative* newNative(NativeFn function) {
  ObjNative* native = ALLOCATE_OBJ(ObjNative, OBJ_NATIVE);
  native->function = function;
  return native;
}

/**
 * @brief Creates an ObjString and interns it in `vm.strings`.
 *
 * The string is pushed on the stack around tableSet() so a GC triggered by
 * a table resize cannot free it.
 *
 * @param chars  Heap buffer; ownership transfers to the string.
 * @param length Length excluding the NUL terminator.
 * @param hash   Precomputed hash of the contents.
 */
static ObjString* allocateString(char* chars, int length, uint32_t hash) {
  ObjString* string = ALLOCATE_OBJ(ObjString, OBJ_STRING);
  string->length = length;
  string->chars = chars;
  string->hash = hash;

  push(OBJ_VAL(string));
  tableSet(&vm.strings, string, NULL_VAL);
  pop();

  return string;
}

/**
 * @brief 32-bit FNV-1a hash of @p length bytes at @p key.
 *
 * XOR each byte into the running hash, then multiply by the FNV prime.
 */
static uint32_t hashString(const char* key, int length) {
  uint32_t hash = 2166136261u;  // FNV offset basis
  for (int i = 0; i < length; i++) {
    hash ^= (uint8_t)key[i];
    hash *= 16777619;  // FNV prime
  }
  return hash;
}

/**
 * @brief Interns a string, TAKING OWNERSHIP of @p chars.
 *
 * If an identical string already exists, @p chars is freed and the existing
 * one is returned; otherwise a new ObjString adopts the buffer. Used when the
 * caller already built the heap buffer (e.g. concatenation).
 *
 * @param chars  Heap-allocated, NUL-terminated buffer of @p length + 1 bytes.
 * @param length Length excluding the terminator.
 */
ObjString* takeString(char* chars, int length) {
  uint32_t hash = hashString(chars, length);
  ObjString* interned = tableFindString(&vm.strings, chars, length, hash);

  if (interned != NULL) {
    FREE_ARRAY(char, chars, length + 1);
    return interned;
  }

  return allocateString(chars, length, hash);
}

/**
 * @brief Interns a string by COPYING @p chars (the caller keeps ownership).
 *
 * Returns the existing interned string if there is one; otherwise copies the
 * characters into a new NUL-terminated heap buffer.
 *
 * @param chars  Source characters (need not be NUL-terminated).
 * @param length Number of characters.
 */
ObjString* copyString(const char* chars, int length) {
  uint32_t hash = hashString(chars, length);
  ObjString* interned = tableFindString(&vm.strings, chars, length, hash);

  if (interned != NULL) return interned;

  char* heapChars = ALLOCATE(char, length + 1);
  memcpy(heapChars, chars, length);
  heapChars[length] = '\0';

  return allocateString(heapChars, length, hash);
}

/**
 * @brief Creates an OPEN upvalue pointing at stack slot @p slot.
 *
 * `closed` is unused until closeUpvalues() moves the value into it, and
 * `next` is linked into vm.openUpvalues by captureUpvalue().
 */
ObjUpvalue* newUpvalue(Value* slot) {
  ObjUpvalue* upvalue = ALLOCATE_OBJ(ObjUpvalue, OBJ_UPVALUE);

  upvalue->closed = NULL_VAL;
  upvalue->location = slot;
  upvalue->next = NULL;
  return upvalue;
}

/** @brief Prints a function as `<fn name>`, or `<script>` for the top-level
 *         (unnamed) function. */
static void printFunction(ObjFunction* function) {
  if (function->name == NULL) {
    printf("<script>");
    return;
  }
  printf("<fn %s>", function->name->chars);
}

/**
 * @brief Prints a heap object value. Closures print like their function.
 * @param value A Value for which IS_OBJ() is true.
 */
void printObject(Value value) {
  switch (OBJ_TYPE(value)) {
    case OBJ_CLOSURE:
      printFunction(AS_CLOSURE(value)->function);
      break;
    case OBJ_FUNCTION:
      printFunction(AS_FUNCTION(value));
      break;
    case OBJ_NATIVE:
      printf("<native fn>");
      break;
    case OBJ_STRING:
      printf("%s", AS_CSTRING(value));
      break;
    case OBJ_UPVALUE:
      printf("upvalue");
      break;
  }
}
