/**
 * @file hash.c
 * @brief Open-addressing hash table keyed by interned strings.
 *
 * Collisions are resolved by linear probing. Because every ObjString is
 * interned, keys are compared by POINTER, not by content (the one exception
 * is tableFindString(), which is what implements the interning).
 *
 * Slot states, encoded in an Entry:
 *  - empty:     key == NULL, value == null
 *  - tombstone: key == NULL, value == true  (a deleted entry)
 *  - live:      key != NULL
 * Tombstones keep probe sequences unbroken after a delete.
 *
 * Still work in progress.
 */

#include "hash.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "object.h"
#include "value.h"

/** @brief Maximum load factor (count / capacity) before the table grows.
 *         `count` includes tombstones. */
#define TABLE_MAX_LOAD 0.75

/** @brief Initializes @p table as empty (no allocation yet). */
void initTable(HashTable* table) {
  table->count = 0;
  table->capacity = 0;
  table->entries = NULL;
}

/** @brief Frees the table's entry array and resets it to the empty state.
 *         Does not free the keys/values themselves (the GC owns them). */
void freeTable(HashTable* table) {
  FREE_ARRAY(Entry, table->entries, table->capacity);
  initTable(table);
}

/**
 * @brief Finds the slot for @p key, or the slot where it should be inserted.
 *
 * Probes linearly from `hash % capacity`. Returns the matching live entry if
 * found. On reaching a truly empty slot the key is absent: returns the first
 * tombstone seen along the way (so it gets reused), or else that empty slot.
 *
 * Relies on the load factor to guarantee at least one empty slot; otherwise
 * the loop would never terminate.
 *
 * @param entries  Entry array to search.
 * @param capacity Length of @p entries.
 * @param key      Interned string to look for.
 * @return Pointer to the matching or insertion slot (never NULL).
 */
static Entry* findEntry(Entry* entries, int capacity, ObjString* key) {
  uint32_t index = key->hash % capacity;
  Entry* empty = NULL;  // first tombstone encountered, if any
  for (;;) {
    Entry* entry = &entries[index];
    if (entry->key == NULL) {
      if (IS_NULL(entry->value)) {
        // truly empty: key is not in the table
        return empty != NULL ? empty : entry;
      } else {
        // tombstone: remember the first one so an insert can reuse it
        if (empty == NULL) empty = entry;
      }
    } else if (entry->key == key) {
      // found key
      return entry;
    }

    index = (index + 1) % capacity;  // linear probe, wrapping
  }
}

/**
 * @brief Looks up @p key.
 * @param table Table to search.
 * @param key   Interned string key.
 * @param value Out: receives the value on success (untouched on failure).
 * @return true if the key was found.
 */
bool tableGet(HashTable* table, ObjString* key, Value* value) {
  if (table->count == 0) return false;

  Entry* entry = findEntry(table->entries, table->capacity, key);
  if (entry->key == NULL) return false;
  *value = entry->value;
  return true;
}

/**
 * @brief Resizes the table to @p capacity and rehashes every live entry.
 *
 * Allocates a fresh array, then reinserts each live entry (tombstones are
 * dropped, so `count` is recomputed to count only live entries).
 *
 * @bug The skip condition is inverted: `if (entry->key) continue;` skips
 *      LIVE entries and tries to move EMPTY ones, passing a NULL key into
 *      findEntry() (which dereferences it). It should be
 *      `if (entry->key == NULL) continue;`. This crashes on the second
 *      resize, once the table actually contains entries.
 */
static void adjustCapacity(HashTable* table, int capacity) {
  Entry* entries = ALLOCATE(Entry, capacity);
  for (int i = 0; i < capacity; i++) {
    entries[i].key = NULL;
    entries[i].value = NULL_VAL;
  }

  table->count = 0;
  for (int i = 0; i < table->capacity; i++) {
    Entry* entry = &table->entries[i];
    if (entry->key) continue;

    Entry* dest = findEntry(entries, capacity, entry->key);
    dest->key = entry->key;
    dest->value = entry->value;
    table->count++;
  }

  FREE_ARRAY(Entry, table->entries, table->capacity);
  table->entries = entries;
  table->capacity = capacity;
}

/**
 * @brief Inserts or updates @p key -> @p value, growing the table if needed.
 *
 * `count` tracks live entries plus tombstones, so it is incremented only when
 * a truly EMPTY slot is consumed. Overwriting a tombstone leaves it
 * unchanged (it was already counted).
 *
 * @return true if @p key was not present before (a new key was added).
 */
bool tableSet(HashTable* table, ObjString* key, Value value) {
  if (table->count + 1 > table->capacity * TABLE_MAX_LOAD) {
    int capacity = GROW_CAPACITY(table->capacity);
    adjustCapacity(table, capacity);
  }

  Entry* entry = findEntry(table->entries, table->capacity, key);
  bool isNewKey = entry->key == NULL;
  if (isNewKey && IS_NULL(entry->value)) table->count++;

  entry->key = key;
  entry->value = value;
  return isNewKey;
}

/**
 * @brief Removes @p key, leaving a tombstone.
 * @return true if the key existed and was removed.
 */
bool tableDelete(HashTable* table, ObjString* key) {
  if (table->count == 0) return false;

  Entry* entry = findEntry(table->entries, table->capacity, key);
  if (entry->key == NULL) return false;

  // Place a tombstone: key cleared, value set to a non-null sentinel.
  entry->key = NULL;
  entry->value = BOOL_VAL(true);
  return true;
}

/** @brief Copies every live entry of @p from into @p to (overwriting
 *         duplicates). */
void tableAddAll(HashTable* from, HashTable* to) {
  for (int i = 0; i < from->capacity; i++) {
    Entry* entry = &from->entries[i];
    if (entry->key != NULL) {
      tableSet(to, entry->key, entry->value);
    }
  }
}

/**
 * @brief Finds an interned string by CONTENT (the basis of string interning).
 *
 * Unlike findEntry(), this compares length, hash and bytes rather than
 * pointers, because it is used before a string object exists for the
 * candidate characters. Tombstones are skipped; an empty slot ends the probe.
 *
 * @param table  The string-intern table (`vm.strings`).
 * @param chars  Candidate characters (need not be NUL-terminated).
 * @param length Number of characters.
 * @param hash   Precomputed hash of the characters.
 * @return The existing interned ObjString, or NULL if none.
 */
ObjString* tableFindString(HashTable* table, const char* chars, int length,
                           uint32_t hash) {
  if (table->count == 0) return NULL;

  uint32_t index = hash % table->capacity;
  for (;;) {
    Entry* entry = &table->entries[index];
    if (entry->key == NULL) {
      // empty (not tombstone): the string isn't in the table
      if (IS_NULL(entry->value)) return NULL;
    } else if (entry->key->length == length && entry->key->hash == hash &&
               memcmp(entry->key->chars, chars, length) == 0) {
      return entry->key;
    }

    index = (index + 1) % table->capacity;
  }
}

/**
 * @brief GC hook: marks every key and value in @p table as reachable.
 *
 * Used for `vm.globals`. markObject() ignores NULL, so empty slots and
 * tombstones are harmless.
 */
void markTable(HashTable* table) {
  for (int i = 0; i < table->capacity; i++) {
    Entry* entry = &table->entries[i];
    markObject((Obj*)entry->key);
    markValue(entry->value);
  }
}
