#ifndef clox_memory_h
#define clox_memory_h

#include "common.h"

//--- MACROS ---//
// Calculate a new capacity from the currenct one, scaling based on the old
// size by a factor of 2
#define GROW_CAPACITY(capacity) ((capacity) < 8 ? 8 : (capacity) * 2)

// Wrapper around `reallocate()` to obtain the size of the array's element
// type and cast the resulting void* to a pointer of the correct type
#define GROW_ARRAY(type, pointer, oldCount, newCount)   \
  (type*)reallocate(pointer, sizeof(type) * (oldCount), \
                    sizeof(type) * (newCount))

// Free the memory by passing in zero for the new size using `reallocate()`
#define FREE_ARRAY(type, pointer, oldCount) \
  reallocate(pointer, sizeof(type) * (oldCount), 0)

void* reallocate(void* pointer, size_t oldSize, size_t newSize);

#endif
