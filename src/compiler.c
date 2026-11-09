#include "compiler.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chunk.h"
#include "common.h"
#include "lexer.h"
#include "object.h"
#include "value.h"

#ifdef DEBUG_PRINT_CODE
#include "debug.h"
#endif /* ifdef DEBUG_PRINT_CODE */

typedef struct {
  Token current;
  Token previous;
  bool  hadError;
  bool  panicMode;
} Parser;

typedef enum {
  PREC_NONE,
  PREC_ASSIGN,
  PREC_OR,
  PREC_AND,
  PREC_EQ,
  PREC_COMP,
  PREC_TERM,
  PREC_FACTOR,
  PREC_UNARY,
  PREC_CALL,
  PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(bool canAssign);

typedef struct {
  ParseFn    prefix;
  ParseFn    infix;
  Precedence precedence;
} ParseRule;

typedef struct {
  Token name;
  int   depth;
  bool  isCaptured;
} Local;

typedef struct {
  uint8_t idx;
  bool    isLocal;
} Upvalue;

typedef enum {
  TYPE_FUNCTION,
  TYPE_SCRIPT,
} FunctionType;

typedef struct Compiler {
  struct Compiler* enclosing;
  ObjFunction*     function;
  FunctionType     type;

  Local   locals[UINT8_COUNT];
  int     localCount;
  Upvalue upvalues[UINT8_COUNT];
  int     scopeDepth;
} Compiler;

Parser    parser;
Compiler* current = NULL;

Chunk* compilingChunk;

/* --- BEGIN STATIC FUNCTION SIGNATURES --- */

static void         advance();
static void         and_(bool canAssign);
static uint8_t      argumentList();
static void         beginScope();
static void         binary(bool canAssign);
static bool         check(TokenType type);
static void         block();
static void         call(bool canAssign);
static void         consume(TokenType type, const char* message);
static Chunk*       currentChunk();
static void         declaration();
static void         addLocal(Token name);
static void         declareVariable();
static void         defineVariable(uint8_t gobal);
static void         emitByte(uint8_t byte);
static void         emitBytes(uint8_t byte1, uint8_t byte2);
static int          emitJmp(uint8_t instruction);
static void         emitLoop(int loopStart);
static void         emitReturn();
static ObjFunction* endCompiler();
static void         endScope();
static void         emitConstant(Value value);
static void         error(const char* message);
static void         errorAt(Token* token, const char* message);
static void         errorAtCurrent(const char* message);
static void         expression();
static void         expressionStatement();
static void         forStatement();
static void         function(FunctionType type);
static void         fnDeclaration();
static ParseRule*   getRule(TokenType type);
static void         grouping(bool canAssign);
static uint8_t      identifierConstant(Token* name);
static bool         identifiersEqual(Token* a, Token* b);
static void         ifStatement();
static void         literal(bool canAssign);
static void         markInitialized();
static bool         match(TokenType type);
static void         namedVariable(Token token, bool canAssign);
static void         number(bool canAssign);
static uint8_t      makeConstant(Value value);
static void         or_(bool canAssign);
static void         parsePrecedence(Precedence precedence);
static uint8_t      parseVariable(const char* errorMessage);
static void         patchJmp(int offset);
static void         putsStatement();
static int          resolveLocal(Compiler* compiler, Token* name);
static int          resolveUpvalue(Compiler* compiler, Token* name);
static int          addUpvalue(Compiler* compiler, uint8_t idx, bool isLocal);
static void         returnStatement();
static void         statement();
static void         string(bool canAssign);
static void         synchronize();
static void         varDeclaration();
static void         variable(bool canAssign);
static void         whileStatement();
static void         unary(bool canAssign);

/* --- END STATIC FUNCTION SIGNATURES --- */

//--        ***        --//
//--        ***        --//
//--        ***        --//

/* --- BEGIN PARSING RULES --- */

ParseRule rules[] = {
    [TOKEN_LEFT_PAREN]    = {grouping,   call,   PREC_CALL},
    [TOKEN_RIGHT_PAREN]   = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_LEFT_BRACE]    = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_RIGHT_BRACE]   = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_COMMA]         = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_DOT]           = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_MINUS]         = {   unary, binary,   PREC_TERM},
    [TOKEN_PLUS]          = {    NULL, binary,   PREC_TERM},
    [TOKEN_SEMICOLON]     = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_SLASH]         = {    NULL, binary, PREC_FACTOR},
    [TOKEN_STAR]          = {    NULL, binary, PREC_FACTOR},
    [TOKEN_BANG]          = {   unary,   NULL,   PREC_NONE},
    [TOKEN_BANG_EQUAL]    = {    NULL, binary,     PREC_EQ},
    [TOKEN_EQUAL]         = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_EQUAL_EQUAL]   = {    NULL, binary,     PREC_EQ},
    [TOKEN_GREATER]       = {    NULL, binary,   PREC_COMP},
    [TOKEN_GREATER_EQUAL] = {    NULL, binary,   PREC_COMP},
    [TOKEN_LESS]          = {    NULL, binary,   PREC_COMP},
    [TOKEN_LESS_EQUAL]    = {    NULL, binary,   PREC_COMP},
    [TOKEN_IDENTIFIER]    = {variable,   NULL,   PREC_NONE},
    [TOKEN_STRING]        = {  string,   NULL,   PREC_NONE}, // string,NULL,PREC_NONE
    [TOKEN_NUMBER]        = {  number,   NULL,   PREC_NONE},
    [TOKEN_AND]           = {    NULL,   and_,    PREC_AND},
    [TOKEN_CLASS]         = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_ELSE]          = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_FALSE]         = { literal,   NULL,   PREC_NONE},
    [TOKEN_FOR]           = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_FN]            = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_NULL]          = { literal,   NULL,   PREC_NONE},
    [TOKEN_OR]            = {    NULL,    or_,     PREC_OR},
    [TOKEN_PUTS]          = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_RETURN]        = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_SUPER]         = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_THIS]          = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_TRUE]          = { literal,   NULL,   PREC_NONE},
    [TOKEN_LET]           = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_WHILE]         = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_ERROR]         = {    NULL,   NULL,   PREC_NONE},
    [TOKEN_EOF]           = {    NULL,   NULL,   PREC_NONE},
};

/* --- END PARSING RULES --- */

//--        ***        --//
//--        ***        --//
//--        ***        --//

/* --- BEGIN ERROR HELPER FUNCTIONS --- */

static void errorAt(Token* token, const char* message) {
  if (parser.panicMode) {
    return;
  }

  parser.panicMode = true;

  fprintf(stderr, "[line %d] Error", token->line);

  if (token->type == TOKEN_EOF) {
    fprintf(stderr, " at EOF");
  } else if (token->type == TOKEN_ERROR) {
    // huh?
  } else {
    fprintf(stderr, " at '%.*s'", token->length, token->start);
  }

  fprintf(stderr, ": %s\n", message);
  parser.hadError = true;
}

static void error(const char* message) { errorAt(&parser.previous, message); }

static void errorAtCurrent(const char* message) {
  errorAt(&parser.current, message);
}

/* --- END ERROR HELPER FUNCTIONS --- */

//--        ***        --//
//--        ***        --//
//--        ***        --//

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

static void consume(TokenType type, const char* message) {
  if (parser.current.type == type) {
    advance();
    return;
  }

  errorAtCurrent(message);
}

static bool check(TokenType type) { return parser.current.type == type; }

static bool match(TokenType type) {
  if (!check(type)) return false;
  advance();
  return true;
}

static void emitByte(uint8_t byte) {
  writeChunk(currentChunk(), byte, parser.previous.line);
}

static void emitBytes(uint8_t byte1, uint8_t byte2) {
  emitByte(byte1);
  emitByte(byte2);
}

static void emitLoop(int loopStart) {
  emitByte(OP_LOOP);

  int offset = currentChunk()->count - loopStart + 2;
  if (offset > UINT16_MAX) error("Loop body too large.");

  emitByte((offset >> 8) & 0xff);
  emitByte(offset & 0xff);
}

static int emitJmp(uint8_t instruction) {
  emitByte(instruction);
  emitByte(0xff);
  emitByte(0xff);
  return currentChunk()->count - 2;
}

static void emitReturn() {
  emitByte(OP_NULL);
  emitByte(OP_RETURN);
}

static uint8_t makeConstant(Value value) {
  int constant = addConstant(currentChunk(), value);
  if (constant > UINT8_MAX) {
    error("Constant overflow in chunk.");
    return 0;
  }

  return (uint8_t)constant;
}
static void emitConstant(Value value) {
  emitBytes(OP_CONST, makeConstant(value));
}

static void patchJmp(int offset) {
  // -2 to adjust for jump offset
  int jmp = currentChunk()->count - offset - 2;

  if (jmp > UINT16_MAX) {
    error("Cannot jump over this much code.");
  }

  currentChunk()->code[offset]     = (jmp >> 8) & 0xff;
  currentChunk()->code[offset + 1] = jmp & 0xff;
}

static void initCompiler(Compiler* compiler, FunctionType type) {
  compiler->enclosing  = current;
  compiler->function   = NULL;
  compiler->type       = type;
  compiler->localCount = 0;
  compiler->scopeDepth = 0;
  compiler->function   = newFunction();

  current = compiler;
  if (type != TYPE_SCRIPT) {
    current->function->name =
        copyString(parser.previous.start, parser.previous.length);
  }

  Local* local       = &current->locals[current->localCount++];
  local->depth       = 0;
  local->isCaptured  = false;
  local->name.start  = "";
  local->name.length = 0;
}

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

static void beginScope() { current->scopeDepth++; }

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

static uint8_t identifierConstant(Token* name) {
  return makeConstant(OBJ_VAL(copyString(name->start, name->length)));
}

static bool identifiersEqual(Token* a, Token* b) {
  if (a->length != b->length) return false;
  return memcmp(a->start, b->start, a->length) == 0;
}

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
  compiler->upvalues[upvalueCount].idx     = idx;

  return compiler->function->upvalueCount++;
}

static void addLocal(Token name) {
  if (current->localCount == UINT8_COUNT) {
    error("Too many local variables in function.");
    return;
  }

  Local* local = &current->locals[current->localCount++];

  local->name       = name;
  local->depth      = -1;
  local->isCaptured = false;
}

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

static void binary(bool canAssign) {
  TokenType  operatorType = parser.previous.type;
  ParseRule* rule         = getRule(operatorType);
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
    case TOKEN_BANG:
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

static void call(bool canAssign) {
  uint8_t argCount = argumentList();
  emitBytes(OP_CALL, argCount);
}

static Chunk* currentChunk() { return &current->function->chunk; }

static void grouping(bool canAssign) {
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expected ')' after expression.");
}

static void number(bool canAssign) {
  double value = strtod(parser.previous.start, NULL);
  emitConstant(NUMBER_VAL(value));
}

static void or_(bool canAssign) {
  int elseJmp = emitJmp(OP_JMP_IF_FALSE);
  int endJmp  = emitJmp(OP_JMP);

  patchJmp(elseJmp);
  emitByte(OP_POP);

  parsePrecedence(PREC_OR);
  patchJmp(endJmp);
}

static void string(bool canAssign) {
  emitConstant(OBJ_VAL(
      copyString(parser.previous.start + 1, parser.previous.length - 2)));
}

static void namedVariable(Token name, bool canAssign) {
  uint8_t getOp, setOp;
  int     arg = resolveLocal(current, &name);
  if (arg != -1) {
    getOp = OP_GET_LOCAL;
    setOp = OP_SET_LOCAL;
  } else if ((arg = resolveUpvalue(current, &name)) != -1) {
    getOp = OP_GET_UPVALUE;
    setOp = OP_SET_UPVALUE;
  } else {
    arg   = identifierConstant(&name);
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

static void variable(bool canAssign) {
  namedVariable(parser.previous, canAssign);
}

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

static uint8_t parseVariable(const char* errorMessage) {
  consume(TOKEN_IDENTIFIER, errorMessage);

  declareVariable();
  if (current->scopeDepth > 0) return 0;

  return identifierConstant(&parser.previous);
}

static void markInitialized() {
  if (current->scopeDepth == 0) return;
  current->locals[current->localCount - 1].depth = current->scopeDepth;
}

static void defineVariable(uint8_t global) {
  if (current->scopeDepth > 0) {
    markInitialized();
    return;
  }

  emitBytes(OP_DEFINE_GLOBAL, global);
}

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

static void and_(bool canAssign) {
  int endJmp = emitJmp(OP_JMP_IF_FALSE);

  emitByte(OP_POP);
  parsePrecedence(PREC_AND);

  patchJmp(endJmp);
}

static ParseRule* getRule(TokenType type) { return &rules[type]; }

static void expression() { parsePrecedence(PREC_ASSIGN); }

static void block() {
  while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
    declaration();
  }

  consume(TOKEN_RIGHT_BRACE, "Expected '}' after block.");
}

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

static void fnDeclaration() {
  uint8_t global = parseVariable("Expected function name.");
  markInitialized();
  function(TYPE_FUNCTION);
  defineVariable(global);
}

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

static void expressionStatement() {
  expression();
  consume(TOKEN_SEMICOLON, "Expected ';' after expression");
  emitByte(OP_POP);
}

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
  int exitJmp   = -1;
  if (!match(TOKEN_SEMICOLON)) {
    expression();
    consume(TOKEN_SEMICOLON, "Expected ';' after loop condition.");

    // jump out of the loop if false
    exitJmp = emitJmp(OP_JMP_IF_FALSE);
    emitByte(OP_POP);
  }

  if (!match(TOKEN_RIGHT_PAREN)) {
    int bodyJmp        = emitJmp(OP_JMP);
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

static void putsStatement() {
  expression();
  consume(TOKEN_SEMICOLON, "Expected ';' after value.");
  emitByte(OP_PUTS);
}

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

ObjFunction* compile(const char* source) {
  initLexer(source);
  Compiler compiler;
  initCompiler(&compiler, TYPE_SCRIPT);

  parser.hadError  = false;
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
