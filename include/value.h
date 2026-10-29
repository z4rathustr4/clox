#ifndef clox_value_h
#define clox_value_h

#include <stdbool.h>

#include "common.h"

typedef enum {
  VAL_BOOL,
  VAL_NULL,
  VAL_NUMBER,
} ValueType;

// tagged union Value struct:
// https://craftinginterpreters.com/image/types-of-values/value.png
typedef struct {
  ValueType type;
  union {
    bool boolean;
    double number;
  } as;
} Value;

#define IS_BOOL(value)   ((value).type == VAL_BOOL)
#define IS_NULL(value)   ((value).type == VAL_NULL)
#define IS_NUMBER(value) ((value).type == VAL_NUMBER)

#define AS_BOOL(value)   ((value).as.boolean)
#define AS_NUMBER(value) ((value).as.number)

#define BOOL_VAL(value)   ((Value){VAL_BOOL, {.boolean = value}})
#define NULL_VAL          ((Value){VAL_NULL, {.number = 0}})
#define NUMBER_VAL(value) ((Value){VAL_NUMBER, {.number = value}})

// constant pool for literal values
typedef struct {
  int capacity;
  int count;
  Value* values;
} ValueArray;

void initValueArray(ValueArray* array);
void writeValueArray(ValueArray* array, Value value);
void freeValueArray(ValueArray* array);
void printValue(Value value);

#endif
