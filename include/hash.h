#ifndef CLOX_HASH_H
#define CLOX_HASH_H

#include "common.h"
#include "value.h"

typedef struct {
  ObjString* key;
  Value      value;
} Entry;

typedef struct {
  int    count;
  int    capacity;
  Entry* entries;
} HashTable;

void initTable(HashTable* table);
void freeTable(HashTable* table);

#endif  // !CLOX_HASH_H
