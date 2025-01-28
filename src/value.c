#include <stdio.h>

#include "memory.h"
#include "value.h"

// Initialize constant values pool
void initValueArray(ValueArray *array) {
  array->values   = NULL;
  array->capacity = 0;
  array->count    = 0;
}

// Add values to the constant values pool
void writeValueArray(ValueArray *array, Value value) {
  if (array->capacity < array->count +1) {
    int oldCapacity = array->capacity;
    array->capacity = GROW_CAPACITY(oldCapacity);
    array->values   = GROW_ARRAY(Value, array->values,
                                 oldCapacity, array->capacity);
  }

  array->values[array->count] = value;
  array->count++;
}

// Constant values pool cleanup
void freeValueArray(ValueArray *array) {
  FREE_ARRAY(Value, array->values, array->capacity);
  initValueArray(array);
}
