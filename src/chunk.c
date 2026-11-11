/**
 * @file chunk.c
 * @brief Chunk: a growable sequence of bytecode plus its constant pool and
 *        per-byte source line numbers.
 *
 * `code` and `lines` are parallel arrays (one line entry per byte), which is
 * what lets runtime errors map a bytecode offset back to a source line.
 *
 * Still work in progress.
 */

#include "chunk.h"

#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>

#include "memory.h"
#include "value.h"

/** @brief Initializes @p chunk as empty (no allocation yet). */
void initChunk(Chunk* chunk) {
  chunk->count = 0;
  chunk->capacity = 0;
  chunk->code = NULL;
  chunk->lines = NULL;
  initValueArray(&chunk->constants);
}

/** @brief Frees the chunk's code, line table and constants, then resets it to
 *         the empty state. */
void freeChunk(Chunk* chunk) {
  FREE_ARRAY(uint8_t, chunk->code, chunk->capacity);
  FREE_ARRAY(int, chunk->lines, chunk->capacity);
  freeValueArray(&chunk->constants);
  initChunk(chunk);
}

/**
 * @brief Appends one byte of bytecode and its source line.
 *
 * Grows both parallel arrays together (geometrically) when full.
 *
 * @param chunk Chunk to append to.
 * @param byte  Opcode or operand byte.
 * @param line  Source line the byte was generated from.
 */
void writeChunk(Chunk* chunk, uint8_t byte, int line) {
  if (chunk->capacity < chunk->count + 1) {
    int oldCapacity = chunk->capacity;
    chunk->capacity = GROW_CAPACITY(oldCapacity);
    chunk->code =
        GROW_ARRAY(uint8_t, chunk->code, oldCapacity, chunk->capacity);
    chunk->lines = GROW_ARRAY(int, chunk->lines, oldCapacity, chunk->capacity);
  }
  chunk->code[chunk->count] = byte;
  chunk->lines[chunk->count] = line;
  chunk->count++;
}

/**
 * @brief Adds @p value to the chunk's constant pool.
 *
 * Does not de-duplicate: adding the same value twice yields two entries.
 *
 * @param chunk Chunk whose pool to extend.
 * @param value Constant to store.
 * @return Index of the new constant (operands are one byte, so callers must
 *         check it fits in 0..255).
 */
int addConstant(Chunk* chunk, Value value) {
  writeValueArray(&chunk->constants, value);
  return chunk->constants.count - 1;
}
