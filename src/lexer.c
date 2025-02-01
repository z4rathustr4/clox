#include "lexer.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "common.h"

typedef struct {
  const char* start;
  const char* current;
  int line;
} Lexer;

Lexer lexer;

void initLexer(const char* source) {
  lexer.start = source;
  lexer.current = source;
  lexer.line = 1;
}

static bool isAtEnd() { return *lexer.current == '\0'; }

static char advance() {
  lexer.current++;
  return lexer.current[-1];
}

static char peek() { return *lexer.current; }

static char peekNext() {
  if (isAtEnd()) {
    return '\0';
  }
  return lexer.current[1];
}

static bool match(char expected) {
  if (isAtEnd()) {
    return false;
  }
  if (*lexer.current != expected) {
    return false;
  }

  lexer.current++;
  return true;
}

static Token makeToken(TokenType type) {
  Token token;
  token.type = type;
  token.start = lexer.start;
  token.length = (int)(lexer.current - lexer.start);
  token.line = lexer.line;

  return token;
}

static Token errorToken(const char* message) {
  Token token;
  token.type = TOKEN_ERROR;
  token.start = message;
  token.length = (int)strlen(message);
  token.line = lexer.line;

  return token;
}

static void skipWhitespace() {
  for (;;) {
    char c = peek();
    switch (c) {
      case ' ':
      case '\r':
      case '\t':
        advance();
        break;
      case '\n':
        lexer.line++;
        advance();
        break;
      case '/':
        if (peekNext() == '/') {
          while (peek() != '\n' && !isAtEnd()) {
            advance();
          }
        } else {
          return;
        }
        break;

      default:
        return;
    }
  }
}

Token scanToken() {
  skipWhitespace();

  lexer.start = lexer.current;

  if (isAtEnd()) {
    return makeToken(TOKEN_EOF);
  }

  char c = advance();

  switch (c) {
    case '(':
      return makeToken(TOKEN_LEFT_PAREN);
    case ')':
      return makeToken(TOKEN_RIGHT_PAREN);
    case '{':
      return makeToken(TOKEN_LEFT_BRACE);
    case '}':
      return makeToken(TOKEN_RIGHT_BRACE);
    case ';':
      return makeToken(TOKEN_SEMICOLON);
    case ',':
      return makeToken(TOKEN_COMMA);
    case '.':
      return makeToken(TOKEN_DOT);
    case '-':
      return makeToken(TOKEN_MINUS);
    case '+':
      return makeToken(TOKEN_PLUS);
    case '/':
      return makeToken(TOKEN_SLASH);
    case '*':
      return makeToken(TOKEN_STAR);

    case '!':
      return makeToken(match('=') ? TOKEN_BANG_EQUAL : TOKEN_BANG);
    case '=':
      return makeToken(match('=') ? TOKEN_EQUAL_EQUAL : TOKEN_EQUAL);
    case '<':
      return makeToken(match('=') ? TOKEN_LESS_EQUAL : TOKEN_LESS);
    case '>':
      return makeToken(match('=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);
  }

  return errorToken("Unexpected character.");
}
