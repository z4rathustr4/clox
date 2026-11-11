/**
 * @file value.c
 * @brief Runtime value helpers: dynamic arrays of Values (used for chunk
 *        constant pools), equality, and printing.
 *
 *  Still work in progress.
 */

#include "value.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "memory.h"
#include "object.h"

/** @brief Initializes @p array as empty (no allocation yet). */
void initValueArray(ValueArray* array) {
  array->values = NULL;
  array->capacity = 0;
  array->count = 0;
}

/**
 * @brief Appends @p value, growing the backing array geometrically when full.
 *
 * Amortized O(1) per append thanks to GROW_CAPACITY's doubling.
 *
 * @param array Array to append to.
 * @param value Value to store.
 */
void writeValueArray(ValueArray* array, Value value) {
  if (array->capacity < array->count + 1) {
    int oldCapacity = array->capacity;
    array->capacity = GROW_CAPACITY(oldCapacity);
    array->values =
        GROW_ARRAY(Value, array->values, oldCapacity, array->capacity);
  }

  array->values[array->count] = value;
  array->count++;
}

/** @brief Frees the array's storage and resets it to the empty state. Heap
 *         objects the values point at are owned by the GC, not freed here. */
void freeValueArray(ValueArray* array) {
  FREE_ARRAY(Value, array->values, array->capacity);
  initValueArray(array);
}

/**
 * @brief Structural equality for two Values.
 *
 * Values of different types are never equal. Within a type: booleans and
 * numbers compare by value, `null` equals `null`, and objects compare by
 * identity (pointer). Since strings are interned, identity equality is also
 * content equality for strings.
 *
 * @return true if @p a and @p b are equal.
 */
bool valuesEqual(Value a, Value b) {
  if (a.type != b.type) {
    return false;
  }

  switch (a.type) {
    case VAL_BOOL:
      return AS_BOOL(a) == AS_BOOL(b);
    case VAL_NULL:
      return true;
    case VAL_NUMBER:
      return AS_NUMBER(a) == AS_NUMBER(b);
    case VAL_OBJ:
      return AS_OBJ(a) == AS_OBJ(b);
    default:
      return false;  // unreachable
  }
}

/**
 * @brief Prints @p value to stdout, without a trailing newline.
 *
 * Numbers use `%g` (so 3.0 prints as `3`); objects are delegated to
 * printObject().
 */
void printValue(Value value) {
  switch (value.type) {
    case VAL_BOOL:
      printf(AS_BOOL(value) ? "true" : "false");
      break;
    case VAL_NULL:
      printf("null");
      break;
    case VAL_NUMBER:
      printf("%g", AS_NUMBER(value));
      break;
    case VAL_OBJ:
      printObject(value);
      break;
  }
}
