/**
 * @file compiler.c
 * @brief Single-pass compiler: source text -> bytecode (ObjFunction).
 *
 * Combines a Pratt (precedence-climbing) expression parser with a recursive
 * descent statement parser. No AST is built: bytecode is emitted into the
 * current function's Chunk as the parser consumes tokens.
 *
 * Each function being compiled has its own Compiler record; these form a
 * linked list (innermost first via `Compiler::enclosing`) mirroring lexical
 * nesting. The global `current` points at the innermost one.
 *
 * Still work in progress.
 */

#include "compiler.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chunk.h"
#include "common.h"
#include "lexer.h"
#include "memory.h"
#include "object.h"
#include "value.h"

#ifdef DEBUG_PRINT_CODE
#include "debug.h"
#endif /* ifdef DEBUG_PRINT_CODE */

/* ======================================================================
 * Types
 * ====================================================================== */

/** @brief Parser state: a two-token window plus error flags. */
typedef struct {
  Token current;   ///< Token about to be consumed (one-token lookahead).
  Token previous;  ///< Token most recently consumed.
  bool hadError;   ///< True if any compile error occurred (suppresses output).
  bool panicMode;  ///< True while recovering from an error; silences cascades.
} Parser;

/**
 * @brief Operator precedence levels, lowest to highest.
 *
 * Order matters: the parser compares these numerically, and binary()
 * parses its right operand at (precedence + 1) to get left associativity.
 */
typedef enum {
  PREC_NONE,
  PREC_ASSIGN,  ///< =
  PREC_OR,      ///< or
  PREC_AND,     ///< and
  PREC_EQ,      ///< == !=
  PREC_COMP,    ///< < > <= >=
  PREC_TERM,    ///< + -
  PREC_FACTOR,  ///< * /
  PREC_UNARY,   ///< ! -
  PREC_CALL,    ///< ( ) call
  PREC_PRIMARY
} Precedence;

/**
 * @brief Signature of a parse function in the Pratt table.
 * @param canAssign True if an `=` after this expression is a valid
 *        assignment (i.e. we are at assignment precedence or lower).
 */
typedef void (*ParseFn)(bool canAssign);

/** @brief One row of the Pratt table, indexed by TokenType. */
typedef struct {
  ParseFn prefix;         ///< Handler when the token STARTS an expression.
  ParseFn infix;          ///< Handler when the token appears BETWEEN operands.
  Precedence precedence;  ///< Precedence of the token as an infix operator.
} ParseRule;

/** @brief A local variable tracked at compile time. */
typedef struct {
  Token name;       ///< Identifier token (used for name lookup).
  int depth;        ///< Scope depth of declaration; -1 = declared, not yet
                    ///< initialized (guards `let a = a;`).
  bool isCaptured;  ///< True if some closure captures it (changes how it is
                    ///< discarded: OP_CLOSE_UPVALUE instead of OP_POP).
} Local;

/** @brief Compile-time description of one upvalue of a function. */
typedef struct {
  uint8_t idx;   ///< Local slot (if isLocal) or enclosing-upvalue index.
  bool isLocal;  ///< True: captures a local of the immediately enclosing
                 ///< function. False: captures one of its upvalues.
} Upvalue;

/** @brief Kind of function body being compiled. */
typedef enum {
  TYPE_FUNCTION,  ///< A user-declared function.
  TYPE_SCRIPT,    ///< The implicit top-level function wrapping the program.
} FunctionType;

/** @brief Per-function compilation state. */
typedef struct Compiler {
  struct Compiler* enclosing;  ///< Compiler of the lexically enclosing
                               ///< function (NULL for the script).
  ObjFunction* function;       ///< Function object receiving the bytecode.
  FunctionType type;           ///< Script or ordinary function.

  Local locals[UINT8_COUNT];      ///< Live locals, in declaration order. The
                                  ///< index IS the VM stack slot.
  int localCount;                 ///< Number of live entries in locals[].
  Upvalue upvalues[UINT8_COUNT];  ///< Upvalues this function captures.
  int scopeDepth;                 ///< 0 = global scope, >0 = nested blocks.
} Compiler;

/* ======================================================================
 * Globals
 * ====================================================================== */

Parser parser;             ///< The one parser instance.
Compiler* current = NULL;  ///< Innermost function currently being compiled.

Chunk* compilingChunk;  ///< Unused: superseded by currentChunk().

/* --- BEGIN STATIC FUNCTION SIGNATURES --- */

static void advance();
static void and_(bool canAssign);
static uint8_t argumentList();
static void beginScope();
static void binary(bool canAssign);
static bool check(TokenType type);
static void block();
static void call(bool canAssign);
static void consume(TokenType type, const char* message);
static Chunk* currentChunk();
static void declaration();
static void addLocal(Token name);
static void declareVariable();
static void defineVariable(uint8_t gobal);
static void emitByte(uint8_t byte);
static void emitBytes(uint8_t byte1, uint8_t byte2);
static int emitJmp(uint8_t instruction);
static void emitLoop(int loopStart);
static void emitReturn();
static ObjFunction* endCompiler();
static void endScope();
static void emitConstant(Value value);
static void error(const char* message);
static void errorAt(Token* token, const char* message);
static void errorAtCurrent(const char* message);
static void expression();
static void expressionStatement();
static void forStatement();
static void function(FunctionType type);
static void fnDeclaration();
static ParseRule* getRule(TokenType type);
static void grouping(bool canAssign);
static uint8_t identifierConstant(Token* name);
static bool identifiersEqual(Token* a, Token* b);
static void ifStatement();
static void literal(bool canAssign);
static void markInitialized();
static bool match(TokenType type);
static void namedVariable(Token token, bool canAssign);
static void number(bool canAssign);
static uint8_t makeConstant(Value value);
static void or_(bool canAssign);
static void parsePrecedence(Precedence precedence);
static uint8_t parseVariable(const char* errorMessage);
static void patchJmp(int offset);
static void putsStatement();
static int resolveLocal(Compiler* compiler, Token* name);
static int resolveUpvalue(Compiler* compiler, Token* name);
static int addUpvalue(Compiler* compiler, uint8_t idx, bool isLocal);
static void returnStatement();
static void statement();
static void string(bool canAssign);
static void synchronize();
static void varDeclaration();
static void variable(bool canAssign);
static void whileStatement();
static void unary(bool canAssign);

/* --- END STATIC FUNCTION SIGNATURES --- */

/* ======================================================================
 * Pratt parsing table
 * ====================================================================== */

/**
 * @brief Pratt table: for each token, how it parses as a prefix operator, as
 *        an infix operator, and its infix precedence.
 *
 * Indexed by TokenType via designated initializers. Tokens not listed (e.g.
 * TOKEN_IF) are zero-initialized, i.e. {NULL, NULL, PREC_NONE}.
 */
ParseRule rules[] = {
    [TOKEN_LEFT_PAREN] = {grouping,   call,   PREC_CALL},
    [TOKEN_RIGHT_PAREN] = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_LEFT_BRACE] = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_RIGHT_BRACE] = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_COMMA] = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_DOT] = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_MINUS] = {   unary, binary,   PREC_TERM},
    [TOKEN_PLUS] = {    NULL, binary,   PREC_TERM},
    [TOKEN_SEMICOLON] = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_SLASH] = {    NULL, binary, PREC_FACTOR},
    [TOKEN_STAR] = {    NULL, binary, PREC_FACTOR},
    [TOKEN_BANG] = {   unary,   NULL,   PREC_NONE},
    [TOKEN_BANG_EQUAL] = {    NULL, binary,     PREC_EQ},
    [TOKEN_EQUAL] = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_EQUAL_EQUAL] = {    NULL, binary,     PREC_EQ},
    [TOKEN_GREATER] = {    NULL, binary,   PREC_COMP},
    [TOKEN_GREATER_EQUAL] = {    NULL, binary,   PREC_COMP},
    [TOKEN_LESS] = {    NULL, binary,   PREC_COMP},
    [TOKEN_LESS_EQUAL] = {    NULL, binary,   PREC_COMP},
    [TOKEN_IDENTIFIER] = {variable,   NULL,   PREC_NONE},
    [TOKEN_STRING] = {  string,   NULL,   PREC_NONE},
    [TOKEN_NUMBER] = {  number,   NULL,   PREC_NONE},
    [TOKEN_AND] = {    NULL,   and_,    PREC_AND},
    [TOKEN_CLASS] = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_ELSE] = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_FALSE] = { literal,   NULL,   PREC_NONE},
    [TOKEN_FOR] = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_FN] = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_NULL] = { literal,   NULL,   PREC_NONE},
    [TOKEN_OR] = {    NULL,    or_,     PREC_OR},
    [TOKEN_PUTS] = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_RETURN] = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_SUPER] = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_THIS] = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_TRUE] = { literal,   NULL,   PREC_NONE},
    [TOKEN_LET] = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_WHILE] = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_ERROR] = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_EOF] = {    NULL,   NULL,   PREC_NONE},
};

/* ======================================================================
 * Error reporting
 * ====================================================================== */

/**
 * @brief Reports a compile error at @p token.
 *
 * Does nothing while in panic mode, so one mistake yields one message
 * instead of a cascade. Otherwise enters panic mode, prints
 * "[line N] Error at 'lexeme': message" to stderr and sets hadError.
 *
 * @param token   Token the error is attributed to.
 * @param message Human-readable description.
 */
static void errorAt(Token* token, const char* message) {
  if (parser.panicMode) {
    return;
  }

  parser.panicMode = true;

  fprintf(stderr, "[line %d] Error", token->line);

  if (token->type == TOKEN_EOF) {
    fprintf(stderr, " at EOF");
  } else if (token->type == TOKEN_ERROR) {
    // Nothing to show: an error token's "lexeme" is the lexer's message
    // itself, which is printed below as `message`.
  } else {
    fprintf(stderr, " at '%.*s'", token->length, token->start);
  }

  fprintf(stderr, ": %s\n", message);
  parser.hadError = true;
}

/** @brief Reports an error at the token just consumed (parser.previous). */
static void error(const char* message) { errorAt(&parser.previous, message); }

/** @brief Reports an error at the token about to be consumed. */
static void errorAtCurrent(const char* message) {
  errorAt(&parser.current, message);
}

/* ======================================================================
 * Token stream helpers
 * ====================================================================== */

/**
 * @brief Moves the parser window forward one token.
 *
 * `current` becomes `previous`, then the lexer is asked for a new
 * `current`. Lexer error tokens are reported and skipped, so the parser
 * never sees a TOKEN_ERROR as `current` afterwards.
 */
static void advance() {
  parser.previous = parser.current;

  for (;;) {
    parser.current = scanToken();
    if (parser.current.type != TOKEN_ERROR) {
      break;
    }

    errorAtCurrent(parser.current.start);
  }
}

/**
 * @brief Consumes the current token if it has type @p type; otherwise
 *        reports @p message as an error at the current token.
 */
static void consume(TokenType type, const char* message) {
  if (parser.current.type == type) {
    advance();
    return;
  }

  errorAtCurrent(message);
}

/** @brief Returns true if the current token has type @p type (no consume). */
static bool check(TokenType type) { return parser.current.type == type; }

/** @brief Consumes the current token and returns true iff it has type @p type.
 */
static bool match(TokenType type) {
  if (!check(type)) return false;
  advance();
  return true;
}

/* ======================================================================
 * Bytecode emission
 * ====================================================================== */

/** @brief Returns the chunk of the function currently being compiled. */
static Chunk* currentChunk() { return &current->function->chunk; }

/** @brief Appends one byte to the current chunk, tagged with the line of
 *         the token just consumed (for runtime error traces). */
static void emitByte(uint8_t byte) {
  writeChunk(currentChunk(), byte, parser.previous.line);
}

/** @brief Appends two bytes (typically an opcode and its operand). */
static void emitBytes(uint8_t byte1, uint8_t byte2) {
  emitByte(byte1);
  emitByte(byte2);
}

/**
 * @brief Emits an OP_LOOP: an unconditional BACKWARD jump to @p loopStart.
 *
 * Operand is a 16-bit big-endian distance. The `+ 2` accounts for the two
 * operand bytes themselves, since the VM's ip will already be past them
 * when it subtracts the offset.
 *
 * @param loopStart Chunk offset to jump back to.
 */
static void emitLoop(int loopStart) {
  emitByte(OP_LOOP);

  int offset = currentChunk()->count - loopStart + 2;
  if (offset > UINT16_MAX) error("Loop body too large.");

  emitByte((offset >> 8) & 0xff);
  emitByte(offset & 0xff);
}

/**
 * @brief Emits a forward jump with a placeholder 16-bit operand (0xffff).
 *
 * The caller must later call patchJmp() with the returned offset once the
 * jump target is known (classic backpatching).
 *
 * @param instruction OP_JMP or OP_JMP_IF_FALSE.
 * @return Chunk offset of the operand's first byte, to pass to patchJmp().
 */
static int emitJmp(uint8_t instruction) {
  emitByte(instruction);
  emitByte(0xff);
  emitByte(0xff);
  return currentChunk()->count - 2;
}

/** @brief Emits an implicit `return null;` (OP_NULL, OP_RETURN). */
static void emitReturn() {
  emitByte(OP_NULL);
  emitByte(OP_RETURN);
}

/**
 * @brief Adds @p value to the current chunk's constant table.
 * @return Its index. Reports an error (and returns 0) if the table has
 *         outgrown the one-byte operand limit of 256 constants.
 */
static uint8_t makeConstant(Value value) {
  int constant = addConstant(currentChunk(), value);
  if (constant > UINT8_MAX) {
    error("Constant overflow in chunk.");
    return 0;
  }

  return (uint8_t)constant;
}

/** @brief Emits OP_CONST that pushes @p value (stored in the constant table).
 */
static void emitConstant(Value value) {
  emitBytes(OP_CONST, makeConstant(value));
}

/**
 * @brief Backpatches a forward jump so it lands at the current end of code.
 * @param offset Value returned by the matching emitJmp().
 */
static void patchJmp(int offset) {
  // -2 to adjust for the jump operand's own two bytes
  int jmp = currentChunk()->count - offset - 2;

  if (jmp > UINT16_MAX) {
    error("Cannot jump over this much code.");
  }

  currentChunk()->code[offset] = (jmp >> 8) & 0xff;
  currentChunk()->code[offset + 1] = jmp & 0xff;
}

/* ======================================================================
 * Compiler lifecycle and scopes
 * ====================================================================== */

/**
 * @brief Begins compiling a new function and makes it the `current` compiler.
 *
 * Links @p compiler to the previous `current` (lexical nesting), allocates
 * the ObjFunction, and for non-script functions names it from the identifier
 * just consumed. Reserves local slot 0 for the callee itself (matching the VM
 * stack layout, where slot 0 holds the closure); it has an empty name so user
 * code can never refer to it.
 *
 * `function` is set to NULL before newFunction() so the GC never sees an
 * uninitialized pointer if allocation triggers a collection.
 *
 * @param compiler Caller-owned (usually stack-allocated) state to initialize.
 * @param type     TYPE_SCRIPT or TYPE_FUNCTION.
 */
static void initCompiler(Compiler* compiler, FunctionType type) {
  compiler->enclosing = current;
  compiler->function = NULL;
  compiler->type = type;
  compiler->localCount = 0;
  compiler->scopeDepth = 0;
  compiler->function = newFunction();

  current = compiler;
  if (type != TYPE_SCRIPT) {
    current->function->name =
        copyString(parser.previous.start, parser.previous.length);
  }

  Local* local = &current->locals[current->localCount++];
  local->depth = 0;
  local->isCaptured = false;
  local->name.start = "";
  local->name.length = 0;
}

/**
 * @brief Finishes the current function and pops back to the enclosing one.
 *
 * Emits an implicit return (so every function body ends with one), optionally
 * disassembles the result, and restores `current`. Note it does not free the
 * Compiler: that lives in the caller's stack frame.
 *
 * @return The completed ObjFunction.
 */
static ObjFunction* endCompiler() {
  emitReturn();
  ObjFunction* function = current->function;

#ifdef DEBUG_PRINT_CODE
  if (!parser.hadError) {
    disassembleChunk(currentChunk(), function->name != NULL
                                         ? function->name->chars
                                         : "<script>");
  }
#endif /* ifdef DEBUG_PRINT_CODE */

  current = current->enclosing;
  return function;
}

/** @brief Enters a new block scope (increments scopeDepth). */
static void beginScope() { current->scopeDepth++; }

/**
 * @brief Leaves a block scope and discards its locals from the VM stack.
 *
 * Emits one instruction per dying local, innermost first: OP_CLOSE_UPVALUE if
 * a closure captured it (hoists it to the heap before popping), else OP_POP.
 */
static void endScope() {
  current->scopeDepth--;

  while (current->localCount > 0 &&
         current->locals[current->localCount - 1].depth > current->scopeDepth) {
    if (current->locals[current->localCount - 1].isCaptured) {
      emitByte(OP_CLOSE_UPVALUE);
    } else {
      emitByte(OP_POP);
    }
    current->localCount--;
  }
}

/* ======================================================================
 * Variable resolution
 * ====================================================================== */

/** @brief Interns @p name as a string constant; returns its constant index
 *         (used as the operand of global get/set/define). */
static uint8_t identifierConstant(Token* name) {
  return makeConstant(OBJ_VAL(copyString(name->start, name->length)));
}

/** @brief Returns true if two identifier tokens spell the same name. */
static bool identifiersEqual(Token* a, Token* b) {
  if (a->length != b->length) return false;
  return memcmp(a->start, b->start, a->length) == 0;
}

/**
 * @brief Looks up @p name among @p compiler's locals.
 *
 * Searches from the most recently declared backwards so inner declarations
 * shadow outer ones. A local whose depth is -1 is still being initialized,
 * so reading it is an error (`let a = a;`).
 *
 * @return Stack slot index, or -1 if not a local of this function.
 */
static int resolveLocal(Compiler* compiler, Token* name) {
  for (int i = compiler->localCount - 1; i >= 0; i--) {
    Local* local = &compiler->locals[i];
    if (identifiersEqual(name, &local->name)) {
      if (local->depth == -1) {
        error("Cant read local variable in its own initializer");
      }
      return i;
    }
  }
  return -1;
}

/**
 * @brief Resolves @p name as an upvalue of @p compiler's function.
 *
 * Recurses outward through enclosing functions:
 *  1. Found as a LOCAL of the immediately enclosing function: mark that
 *     local captured and record an upvalue with isLocal = true.
 *  2. Otherwise resolve it as an upvalue of the enclosing function
 *     (recursively) and record an upvalue with isLocal = false, chaining
 *     the capture through every intermediate function.
 *
 * @return Upvalue index within @p compiler's function, or -1 if the name is
 *         not found in any enclosing function (so it must be a global).
 */
static int resolveUpvalue(Compiler* compiler, Token* name) {
  if (compiler->enclosing == NULL) return -1;

  int local = resolveLocal(compiler->enclosing, name);
  if (local != -1) {
    compiler->enclosing->locals[local].isCaptured = true;
    return addUpvalue(compiler, (uint8_t)local, true);
  }

  int upvalue = resolveUpvalue(compiler->enclosing, name);
  if (upvalue != -1) {
    return addUpvalue(compiler, (uint8_t)upvalue, false);
  }

  return -1;
}

/**
 * @brief Registers an upvalue in @p compiler, de-duplicating repeats.
 *
 * @param compiler Function that needs the upvalue.
 * @param idx      Local slot or enclosing-upvalue index (see Upvalue).
 * @param isLocal  Whether @p idx refers to a local of the enclosing function.
 * @return Index of the (existing or new) upvalue.
 *
 * @bug The "Too many closure variables" check sits INSIDE the for loop, so it
 *      never fires when the list is empty and is evaluated redundantly per
 *      iteration. It belongs after the loop, before the new entry is written;
 *      as written, a 257th upvalue writes past `upvalues[]`.
 */
static int addUpvalue(Compiler* compiler, uint8_t idx, bool isLocal) {
  int upvalueCount = compiler->function->upvalueCount;

  for (int i = 0; i < upvalueCount; i++) {
    Upvalue* upvalue = &compiler->upvalues[i];
    if (upvalue->idx == idx && upvalue->isLocal == isLocal) {
      return i;
    }

    if (upvalueCount == UINT8_COUNT) {
      error("Too many closure variables in function.");
    }
  }

  compiler->upvalues[upvalueCount].isLocal = isLocal;
  compiler->upvalues[upvalueCount].idx = idx;

  return compiler->function->upvalueCount++;
}

/**
 * @brief Adds a new local to the current function in the "uninitialized"
 *        state (depth = -1). markInitialized() finalizes it.
 */
static void addLocal(Token name) {
  if (current->localCount == UINT8_COUNT) {
    error("Too many local variables in function.");
    return;
  }

  Local* local = &current->locals[current->localCount++];

  local->name = name;
  local->depth = -1;
  local->isCaptured = false;
}

/**
 * @brief Declares the identifier just consumed as a local variable.
 *
 * No-op at global scope (globals are late-bound by name). Otherwise checks
 * for a redeclaration within the SAME scope (locals from outer scopes may be
 * shadowed freely) and then calls addLocal().
 */
static void declareVariable() {
  if (current->scopeDepth == 0) return;
  Token* name = &parser.previous;

  for (int i = current->localCount - 1; i >= 0; i--) {
    Local* local = &current->locals[i];
    if (local->depth != -1 && local->depth < current->scopeDepth) {
      break;
    }

    if (identifiersEqual(name, &local->name)) {
      error("Variable name already assigned in this scope");
    }
  }

  addLocal(*name);
}

/**
 * @brief Marks the most recently declared local as initialized (available).
 *
 * No-op at global scope. Called after a variable's initializer is compiled,
 * or early for functions so they can refer to themselves recursively.
 */
static void markInitialized() {
  if (current->scopeDepth == 0) return;
  current->locals[current->localCount - 1].depth = current->scopeDepth;
}

/**
 * @brief Consumes an identifier and declares it.
 * @param errorMessage Message if no identifier follows.
 * @return Constant index of the name for globals; 0 for locals, which are
 *         addressed by stack slot and need no name constant.
 */
static uint8_t parseVariable(const char* errorMessage) {
  consume(TOKEN_IDENTIFIER, errorMessage);

  declareVariable();
  if (current->scopeDepth > 0) return 0;

  return identifierConstant(&parser.previous);
}

/**
 * @brief Finishes a variable definition.
 *
 * Globals: emits OP_DEFINE_GLOBAL. Locals: nothing to emit, since the
 * initializer's value already sits in the right stack slot; just mark the
 * local as usable.
 *
 * @param global Name constant index from parseVariable().
 */
static void defineVariable(uint8_t global) {
  if (current->scopeDepth > 0) {
    markInitialized();
    return;
  }

  emitBytes(OP_DEFINE_GLOBAL, global);
}

/* ======================================================================
 * Expression parsing (Pratt)
 * ====================================================================== */

/** @brief Returns the Pratt table row for @p type. */
static ParseRule* getRule(TokenType type) { return &rules[type]; }

/**
 * @brief Core of the Pratt parser: parses an expression whose operators all
 *        bind at least as tightly as @p precedence.
 *
 * Algorithm: consume a token and run its PREFIX handler (error if none), then
 * keep consuming infix operators as long as their precedence is >=
 * @p precedence, running each INFIX handler. `canAssign` is true only at
 * assignment level, so `a + b = c` is rejected ("Invalid assignment target")
 * instead of being parsed as `a + (b = c)`.
 */
static void parsePrecedence(Precedence precedence) {
  advance();
  ParseFn prefixRule = getRule(parser.previous.type)->prefix;
  if (prefixRule == NULL) {
    error("Expected expression.");
    return;
  }

  bool canAssign = precedence <= PREC_ASSIGN;
  prefixRule(canAssign);

  while (precedence <= getRule(parser.current.type)->precedence) {
    advance();
    ParseFn infixRule = getRule(parser.previous.type)->infix;
    infixRule(canAssign);

    if (canAssign && match(TOKEN_EQUAL)) {
      error("Invalid assignment target.");
    }
  }
}

/** @brief Parses a full expression (lowest precedence: assignment). */
static void expression() { parsePrecedence(PREC_ASSIGN); }

/** @brief Prefix handler for `true`, `false`, `null`: emits the literal op. */
static void literal(bool canAssign) {
  switch (parser.previous.type) {
    case TOKEN_FALSE:
      emitByte(OP_FALSE);
      break;
    case TOKEN_NULL:
      emitByte(OP_NULL);
      break;
    case TOKEN_TRUE:
      emitByte(OP_TRUE);
      break;
    default:
      return;
  }
}

/** @brief Prefix handler for number literals: converts and emits a constant. */
static void number(bool canAssign) {
  double value = strtod(parser.previous.start, NULL);
  emitConstant(NUMBER_VAL(value));
}

/** @brief Prefix handler for string literals: strips the surrounding quotes,
 *         interns the contents and emits a constant. */
static void string(bool canAssign) {
  emitConstant(OBJ_VAL(
      copyString(parser.previous.start + 1, parser.previous.length - 2)));
}

/** @brief Prefix handler for `( expr )`. */
static void grouping(bool canAssign) {
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expected ')' after expression.");
}

/**
 * @brief Prefix handler for unary operators (currently only `-`).
 *
 * The operand is compiled first so its value is on the stack when the
 * operator instruction runs. Parsed at PREC_UNARY so `-a.b` negates the
 * whole call/property chain and `--a` nests.
 */
static void unary(bool canAssign) {
  TokenType operatorType = parser.previous.type;

  // compile operand
  parsePrecedence(PREC_UNARY);

  // emit operator instruction
  switch (operatorType) {
    case TOKEN_MINUS:
      emitByte(OP_NEGATE);
      break;
    default:
      return;
  }
}

/**
 * @brief Infix handler for binary operators.
 *
 * The left operand is already compiled. Parses the right operand at
 * (precedence + 1), which makes every operator left-associative, then emits
 * the operator. There are no dedicated opcodes for `!=`, `<=`, `>=`, so they
 * compile to the negation of the opposite comparison (e.g. `a <= b` is
 * `!(a > b)`).
 */
static void binary(bool canAssign) {
  TokenType operatorType = parser.previous.type;
  ParseRule* rule = getRule(operatorType);
  parsePrecedence((Precedence)(rule->precedence + 1));

  switch (operatorType) {
    case TOKEN_BANG_EQUAL:
      emitBytes(OP_EQU, OP_NOT);
      break;
    case TOKEN_EQUAL_EQUAL:
      emitByte(OP_EQU);
      break;
    case TOKEN_GREATER:
      emitByte(OP_GREATER);
      break;
    case TOKEN_GREATER_EQUAL:
      emitBytes(OP_LESS, OP_NOT);
      break;
    case TOKEN_LESS:
      emitByte(OP_LESS);
      break;
    case TOKEN_LESS_EQUAL:
      emitBytes(OP_GREATER, OP_NOT);
      break;
    case TOKEN_PLUS:
      emitByte(OP_ADD);
      break;
    case TOKEN_BANG:  // unreachable: `!` has no infix rule
      emitByte(OP_NOT);
      break;
    case TOKEN_MINUS:
      emitByte(OP_SUBTRACT);
      break;
    case TOKEN_STAR:
      emitByte(OP_MULTIPLY);
      break;
    case TOKEN_SLASH:
      emitByte(OP_DIVIDE);
      break;
  }
}

/**
 * @brief Parses a call's argument list, after the opening `(`.
 * @return Number of arguments (max 255; more is a compile error).
 */
static uint8_t argumentList() {
  uint8_t argCount = 0;
  if (!check(TOKEN_RIGHT_PAREN)) {
    do {
      expression();
      if (argCount == 255) {
        error("Maximum arguments is 255.");
      }
      argCount++;
    } while (match(TOKEN_COMMA));
  }
  consume(TOKEN_RIGHT_PAREN, "Expected ')' after arguments.");
  return argCount;
}

/** @brief Infix handler for `(`: compiles a call. The callee and arguments
 *         end up on the stack, then OP_CALL consumes them. */
static void call(bool canAssign) {
  uint8_t argCount = argumentList();
  emitBytes(OP_CALL, argCount);
}

/**
 * @brief Infix handler for short-circuit `and`.
 *
 * If the left operand is falsey, jump past the right operand leaving the left
 * value as the result. Otherwise pop it and evaluate the right operand.
 */
static void and_(bool canAssign) {
  int endJmp = emitJmp(OP_JMP_IF_FALSE);

  emitByte(OP_POP);
  parsePrecedence(PREC_AND);

  patchJmp(endJmp);
}

/**
 * @brief Infix handler for short-circuit `or`.
 *
 * There is no jump-if-true opcode, so it is built from two jumps: if the left
 * operand is falsey, skip the unconditional jump and evaluate the right
 * operand; if truthy, the OP_JMP skips the right operand, leaving the left
 * value as the result.
 */
static void or_(bool canAssign) {
  int elseJmp = emitJmp(OP_JMP_IF_FALSE);
  int endJmp = emitJmp(OP_JMP);

  patchJmp(elseJmp);
  emitByte(OP_POP);

  parsePrecedence(PREC_OR);
  patchJmp(endJmp);
}

/**
 * @brief Compiles a variable read or assignment.
 *
 * Resolution order decides the opcode family: local (stack slot), then
 * upvalue (captured from an enclosing function), then global (by name). If
 * `=` follows and assignment is permitted here, compiles the right-hand side
 * and emits the SET op; otherwise emits the GET op.
 *
 * @param name      Identifier token.
 * @param canAssign Whether `=` may follow.
 */
static void namedVariable(Token name, bool canAssign) {
  uint8_t getOp, setOp;
  int arg = resolveLocal(current, &name);
  if (arg != -1) {
    getOp = OP_GET_LOCAL;
    setOp = OP_SET_LOCAL;
  } else if ((arg = resolveUpvalue(current, &name)) != -1) {
    getOp = OP_GET_UPVALUE;
    setOp = OP_SET_UPVALUE;
  } else {
    arg = identifierConstant(&name);
    getOp = OP_GET_GLOBAL;
    setOp = OP_SET_GLOBAL;
  }

  if (canAssign && match(TOKEN_EQUAL)) {
    expression();
    emitBytes(setOp, (uint8_t)arg);
  } else {
    emitBytes(getOp, (uint8_t)arg);
  }
}

/** @brief Prefix handler for identifiers. */
static void variable(bool canAssign) {
  namedVariable(parser.previous, canAssign);
}

/* ======================================================================
 * Statements and declarations
 * ====================================================================== */

/** @brief Parses declarations until the closing `}` (or EOF) of a block. */
static void block() {
  while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
    declaration();
  }

  consume(TOKEN_RIGHT_BRACE, "Expected '}' after block.");
}

/**
 * @brief Compiles a function's parameters and body, then emits OP_CLOSURE.
 *
 * Uses a fresh Compiler for the function (stored on the C stack). The
 * function's top-level scope is opened but never explicitly closed: when the
 * function returns, the VM discards its entire frame at once. After the body,
 * OP_CLOSURE is emitted followed by one (isLocal, index) byte pair per
 * upvalue, which the VM uses to wire up captured variables at runtime.
 *
 * @param type TYPE_FUNCTION (the name token must be `parser.previous`).
 */
static void function(FunctionType type) {
  Compiler compiler;
  initCompiler(&compiler, type);
  beginScope();

  consume(TOKEN_LEFT_PAREN, "Expected '(' after function name.");

  if (!check(TOKEN_RIGHT_PAREN)) {
    do {
      current->function->arity++;
      if (current->function->arity > 255) {
        errorAtCurrent("Maximum parameters is 255.");
      }
      uint8_t constant = parseVariable("Expected parameter name.");
      defineVariable(constant);
    } while (match(TOKEN_COMMA));
  }

  consume(TOKEN_RIGHT_PAREN, "Expected ')' after function parameters.");
  consume(TOKEN_LEFT_BRACE, "Expected '{' after function body.");
  block();

  ObjFunction* function = endCompiler();
  emitBytes(OP_CLOSURE, makeConstant(OBJ_VAL(function)));

  for (int i = 0; i < function->upvalueCount; i++) {
    emitByte(compiler.upvalues[i].isLocal ? 1 : 0);
    emitByte(compiler.upvalues[i].idx);
  }
}

/**
 * @brief Compiles `fn name(...) { ... }`.
 *
 * The name is marked initialized BEFORE compiling the body so the function
 * can call itself recursively.
 */
static void fnDeclaration() {
  uint8_t global = parseVariable("Expected function name.");
  markInitialized();
  function(TYPE_FUNCTION);
  defineVariable(global);
}

/** @brief Compiles `let name [= expr];`. A missing initializer yields null. */
static void varDeclaration() {
  uint8_t global = parseVariable("Expected variable name after declaration.");

  if (match(TOKEN_EQUAL)) {
    expression();
  } else {
    emitByte(OP_NULL);
  }
  consume(TOKEN_SEMICOLON, "Expected ';' after variable declaration.");
  defineVariable(global);
}

/** @brief Compiles `expr;`. The value is evaluated for side effects, then
 *         popped. */
static void expressionStatement() {
  expression();
  consume(TOKEN_SEMICOLON, "Expected ';' after expression");
  emitByte(OP_POP);
}

/**
 * @brief Compiles `for (init; cond; incr) body`.
 *
 * Emitted layout:
 * @code
 *   init
 * loopStart:
 *   cond ; OP_JMP_IF_FALSE exit ; OP_POP
 *   OP_JMP body                       ; skip over the increment on entry
 * incrementStart:
 *   incr ; OP_POP
 *   OP_LOOP loopStart                 ; after incr, re-test the condition
 * body:
 *   body
 *   OP_LOOP incrementStart            ; (loopStart was repointed above)
 * exit:
 *   OP_POP                            ; pop the condition on the false path
 * @endcode
 * The whole statement is wrapped in a scope so a `let` in the initializer is
 * local to the loop. Each clause is optional.
 */
static void forStatement() {
  beginScope();
  consume(TOKEN_LEFT_PAREN, "Expected '(' after 'for' statement.");
  if (match(TOKEN_SEMICOLON)) {
    // no initializer
  } else if (match(TOKEN_LET)) {
    varDeclaration();
  } else {
    expressionStatement();
  }

  int loopStart = currentChunk()->count;
  int exitJmp = -1;
  if (!match(TOKEN_SEMICOLON)) {
    expression();
    consume(TOKEN_SEMICOLON, "Expected ';' after loop condition.");

    // jump out of the loop if false
    exitJmp = emitJmp(OP_JMP_IF_FALSE);
    emitByte(OP_POP);
  }

  if (!match(TOKEN_RIGHT_PAREN)) {
    int bodyJmp = emitJmp(OP_JMP);
    int incrementStart = currentChunk()->count;
    expression();
    emitByte(OP_POP);
    consume(TOKEN_RIGHT_PAREN, "Expected ')' closing 'for' statement.");

    emitLoop(loopStart);
    loopStart = incrementStart;
    patchJmp(bodyJmp);
  }

  statement();

  if (exitJmp != -1) {
    patchJmp(exitJmp);
    emitByte(OP_POP);
  }

  endScope();
}

/**
 * @brief Compiles `if (cond) stmt [else stmt]`.
 *
 * The condition is left on the stack by OP_JMP_IF_FALSE, so each branch
 * begins with an explicit OP_POP. The `else` jump is always emitted (even
 * without an else clause) to keep the structure uniform.
 */
static void ifStatement() {
  consume(TOKEN_LEFT_PAREN, "Expected '(' after 'if' statement.");
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expected ')' after condition.");

  int thenJmp = emitJmp(OP_JMP_IF_FALSE);
  emitByte(OP_POP);
  statement();

  int elseJmp = emitJmp(OP_JMP);
  patchJmp(thenJmp);
  emitByte(OP_POP);

  if (match(TOKEN_ELSE)) statement();
  patchJmp(elseJmp);
}

/** @brief Compiles `puts expr;`. */
static void putsStatement() {
  expression();
  consume(TOKEN_SEMICOLON, "Expected ';' after value.");
  emitByte(OP_PUTS);
}

/** @brief Compiles `return [expr];`. Illegal at top level. A bare `return;`
 *         returns null. */
static void returnStatement() {
  if (current->type == TYPE_SCRIPT) {
    error("Can't return from top level.");
  }

  if (match(TOKEN_SEMICOLON)) {
    emitReturn();
  } else {
    expression();
    consume(TOKEN_SEMICOLON, "Expected ';' after return value.");
    emitByte(OP_RETURN);
  }
}

/**
 * @brief Compiles `while (cond) stmt`.
 *
 * Layout: cond, OP_JMP_IF_FALSE exit, OP_POP, body, OP_LOOP back to the
 * condition, then at `exit` an OP_POP for the condition on the false path.
 */
static void whileStatement() {
  int loopStart = currentChunk()->count;
  consume(TOKEN_LEFT_PAREN, "Expected '(' after 'while' statement.");
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expected ')' after condition.");

  int exitJmp = emitJmp(OP_JMP_IF_FALSE);
  emitByte(OP_POP);
  statement();
  emitLoop(loopStart);

  patchJmp(exitJmp);
  emitByte(OP_POP);
}

/**
 * @brief Error recovery: discards tokens until a likely statement boundary.
 *
 * Clears panic mode, then skips forward until just after a `;` or just
 * before a token that normally starts a statement. This lets the compiler
 * keep going and report further, independent errors.
 */
static void synchronize() {
  parser.panicMode = false;

  while (parser.current.type != TOKEN_EOF) {
    if (parser.previous.type == TOKEN_SEMICOLON) return;
    switch (parser.current.type) {
      case TOKEN_CLASS:
      case TOKEN_FN:
      case TOKEN_LET:
      case TOKEN_FOR:
      case TOKEN_IF:
      case TOKEN_WHILE:
      case TOKEN_PUTS:
      case TOKEN_RETURN:
        return;

      default:;  // noop
    }

    advance();
  }
}

/** @brief Compiles one declaration (`fn`, `let`) or falls through to a
 *         statement; resynchronizes afterwards if an error put us in panic
 *         mode. */
static void declaration() {
  if (match(TOKEN_FN)) {
    fnDeclaration();
  } else if (match(TOKEN_LET)) {
    varDeclaration();
  } else {
    statement();
  }

  if (parser.panicMode) synchronize();
}

/** @brief Compiles one statement (dispatches on its first token). */
static void statement() {
  if (match(TOKEN_PUTS)) {
    putsStatement();
  } else if (match(TOKEN_IF)) {
    ifStatement();
  } else if (match(TOKEN_RETURN)) {
    returnStatement();
  } else if (match(TOKEN_WHILE)) {
    whileStatement();
  } else if (match(TOKEN_FOR)) {
    forStatement();
  } else if (match(TOKEN_LEFT_BRACE)) {
    beginScope();
    block();
    endScope();
  } else {
    expressionStatement();
  }
}

/* ======================================================================
 * Public API
 * ====================================================================== */

/**
 * @brief Compiles a source string into a top-level function.
 *
 * @param source NUL-terminated Lox source text.
 * @return The script as an ObjFunction (to be wrapped in a closure and run
 *         by the VM), or NULL if any compile error occurred.
 *
 * @bug Everything after `return ...;` (the token-dumping loop) is
 *      unreachable debug code left over from an earlier stage and can be
 *      deleted.
 */
ObjFunction* compile(const char* source) {
  initLexer(source);
  Compiler compiler;
  initCompiler(&compiler, TYPE_SCRIPT);

  parser.hadError = false;
  parser.panicMode = false;

  advance();

  while (!match(TOKEN_EOF)) {
    declaration();
  }

  ObjFunction* function = endCompiler();
  return parser.hadError ? NULL : function;

  int line = -1;
  for (;;) {
    Token token = scanToken();
    if (token.line != line) {
      printf("%4d ", token.line);
      line = token.line;
    } else {
      printf("    | ");
    }
    printf("%2d '%.*s'\n", token.type, token.length, token.start);

    if (token.type == TOKEN_EOF) {
      break;
    }
  }
}

/**
 * @brief GC hook: marks the function objects of every in-progress Compiler.
 *
 * Functions under construction are not yet reachable from the VM stack or
 * globals, so without this a collection during compilation could free them.
 * Walks the `enclosing` chain from the innermost compiler outward.
 */
void markCompilerRoots() {
  Compiler* compiler = current;
  while (compiler != NULL) {
    markObject((Obj*)compiler->function);
    compiler = compiler->enclosing;
  }
}
