#include "include/chunk.h"
#include "include/common.h"
#include "include/debug.h"
#include "vm.h"

int main(int argc, const char* argv[]) {
  initVM();

  Chunk chunk;
  initChunk(&chunk);

  int constant = addConstant(&chunk, 1.2);

  writeChunk(&chunk, OP_CONST, 123);
  writeChunk(&chunk, constant, 123);
  writeChunk(&chunk, OP_NEGATE, 123);
  writeChunk(&chunk, OP_RETURN, 123);

  interpret(&chunk);
  disassembleChunk(&chunk, "testing testicles testosterone");

  freeVM();
  freeChunk(&chunk);
  return 0;
}
