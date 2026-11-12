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

#define GC_HEAP_GROW_FACTOR 2

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
 *
 *  TODO: Update doc
 */
void* reallocate(void* pointer, size_t oldSize, size_t newSize) {
  vm.bytesAllocated += newSize - oldSize;
  if (newSize > oldSize) {
#ifdef DEBUG_STRESS_GC
    collectGarbage();
#endif /* ifdef DEBUG_STRESS_GC */

    if (vm.bytesAllocated > vm.nextGC) {
      collectGarbage();
    }
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
 * @brief Marks an object reachable and queues it on the gray worklist.
 *
 * Already-marked objects are skipped, so cycles terminate.
 *
 * @param object Object to mark; NULL is ignored.
 */
void markObject(Obj* object) {
  if (object == NULL) return;
  if (object->isMarked) return;

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

static void markArray(ValueArray* array) {
  for (int i = 0; i < array->count; i++) {
    markValue(array->values[i]);
  }
}

static void blackenObject(Obj* object) {
#ifdef DEBUG_LOG_GC
  printf("%p blacken", (void*)object);
  printValue(OBJ_VAL(object));
  printf("\n");
#endif /* ifdef DEBUG_LOG_GC */
  switch (object->type) {
    case OBJ_CLOSURE: {
      ObjClosure* closure = (ObjClosure*)object;
      markObject((Obj*)closure->function);
      for (int i = 0; i < closure->upvalueCount; i++) {
        markObject((Obj*)closure->upvalues[i]);
      }
      break;
    }
    case OBJ_FUNCTION: {
      ObjFunction* function = (ObjFunction*)object;
      markObject((Obj*)function->name);
      markArray(&function->chunk.constants);
    }
    case OBJ_UPVALUE:
      markValue(((ObjUpvalue*)object)->closed);
      break;
    case OBJ_NATIVE:
    case OBJ_STRING:
      break;
  }
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
 * @brief Blackens gray objects until the worklist is empty.
 *
 * @note Needs markObject() to skip already-marked objects, or cycles never
 *       terminate.
 */
static void traceReferences() {
  while (vm.grayCount > 0) {
    Obj* object = vm.grayStack[--vm.grayCount];
    blackenObject(object);
  }
}

/**
 * @brief Unlinks and frees unmarked objects; clears marks on survivors.
 */
static void sweep() {
  Obj* previous = NULL;
  Obj* object = vm.objects;
  while (object != NULL) {
    if (object->isMarked) {
      object->isMarked = false;
      previous = object;
      object = object->next;
    } else {
      Obj* unreached = object;
      object = object->next;
      if (previous != NULL) {
        previous->next = object;
      } else {
        vm.objects = object;
      }

      freeObject(unreached);
    }
  }
}

/**
 * @brief Runs a mark-sweep collection and sets the next GC threshold to
 *        live size * GC_HEAP_GROW_FACTOR.
 */
void collectGarbage() {
#ifdef DEBUG_LOG_GC
  printf(" --- GC BEGIN\n");
  size_t before = vm.bytesAllocated;
#endif /* ifdef DEBUG_LOG_GC */

  markRoots();
  traceReferences();
  tableRemoveWhite(&vm.strings);
  sweep();

  vm.nextGC = vm.bytesAllocated * GC_HEAP_GROW_FACTOR;
#ifdef DEBUG_LOG_GC
  printf(" --- GC END\n");
  printf("     collected %zu bytes (from %zu to %zu) next at %zu\n",
         before - vm.bytesAllocated, before, vm.bytesAllocated, vm.nextGC);
#endif /* ifdef DEBUG_LOG_GC */
}

/**
 * @brief Frees every object in the VM's all-objects list, plus the gray
 *        worklist. Called once at shutdown.
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
