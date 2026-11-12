/* @file hash.h */
#ifndef CLOX_HASH_H
#define CLOX_HASH_H

#include <stdint.h>

#include "common.h"
#include "value.h"

typedef struct {
  ObjString* key;
  Value value;
} Entry;

typedef struct {
  int count;
  int capacity;
  Entry* entries;
} HashTable;

void initTable(HashTable* table);
void freeTable(HashTable* table);

bool tableSet(HashTable* table, ObjString* key, Value value);
bool tableGet(HashTable* table, ObjString* key, Value* value);
bool tableDelete(HashTable* table, ObjString* key);
void tableAddAll(HashTable* from, HashTable* to);

ObjString* tableFindString(HashTable* table, const char* chars, int length,
                           uint32_t hash);
void tableRemoveWhite(HashTable* table);

void markTable(HashTable* table);

#endif  // !CLOX_HASH_H
