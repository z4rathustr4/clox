/**
 * @file lexer.c
 * @brief On-demand lexer: turns source text into Tokens, one per scanToken().
 *
 * Tokens do not copy text. Each Token points into the original source
 * buffer (`start` + `length`), so the source must outlive any token use.
 *
 * Still work in progress.
 */

#include "lexer.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "common.h"

static char advance();
static TokenType checkKeyword(int start, int length, const char* rest,
                              TokenType type);
static Token errorToken(const char* message);
static Token identifier();
static TokenType identifierType();
static bool isAlpha(char c);
static bool isAtEnd();
static bool isDigit(char c);
static Token makeToken(TokenType type);
static bool match(char expected);
static Token number();
static char peek();
static char peekNext();
static void skipWhitespace();
static Token string();

/** @brief Scanner state. */
typedef struct {
  const char* start;    ///< First character of the token being scanned.
  const char* current;  ///< Next character to be consumed.
  int line;             ///< Current 1-based source line.
} Lexer;

Lexer lexer;  ///< The one lexer instance.

/**
 * @brief Points the lexer at the beginning of @p source.
 * @param source NUL-terminated source text. Must stay alive while tokens
 *               derived from it are in use.
 */
void initLexer(const char* source) {
  lexer.start = source;
  lexer.current = source;
  lexer.line = 1;
}

/* ---- Character classification -------------------------------------- */

/** @brief True for letters and underscore (valid identifier starts). */
static bool isAlpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

/** @brief True for ASCII digits 0-9. */
static bool isDigit(char c) { return c >= '0' && c <= '9'; }

/** @brief True once the NUL terminator has been reached. */
static bool isAtEnd() { return *lexer.current == '\0'; }

/* ---- Cursor primitives --------------------------------------------- */

/** @brief Consumes and returns the current character. */
static char advance() {
  lexer.current++;
  return lexer.current[-1];
}

/** @brief Returns the current character without consuming it. */
static char peek() { return *lexer.current; }

/** @brief Returns the character AFTER the current one (two-char lookahead);
 *         '\0' at end of input. */
static char peekNext() {
  if (isAtEnd()) {
    return '\0';
  }
  return lexer.current[1];
}

/** @brief Consumes the current character only if it equals @p expected.
 *         Used for two-character operators like `==` and `<=`. */
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

/* ---- Token construction -------------------------------------------- */

/** @brief Builds a token of @p type spanning [lexer.start, lexer.current). */
static Token makeToken(TokenType type) {
  Token token;
  token.type = type;
  token.start = lexer.start;
  token.length = (int)(lexer.current - lexer.start);
  token.line = lexer.line;

  return token;
}

/**
 * @brief Builds a TOKEN_ERROR whose "lexeme" is the error message itself.
 * @param message Static string; it is referenced, not copied.
 */
static Token errorToken(const char* message) {
  Token token;
  token.type = TOKEN_ERROR;
  token.start = message;
  token.length = (int)strlen(message);
  token.line = lexer.line;

  return token;
}

/* ---- Whitespace and comments --------------------------------------- */

/**
 * @brief Skips spaces, tabs, carriage returns, newlines and `//` comments.
 *
 * A comment is consumed up to, but not including, the newline so the
 * newline case handles it on the next iteration. A lone `/` is a division
 * operator, so it is left for scanToken().
 *
 * @bug The newline case reads `lexer.line;` (a no-op expression) instead of
 *      `lexer.line++;`, so line numbers only advance inside multi-line
 *      strings and every error/stack trace reports the wrong line.
 */
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

/* ---- Identifiers and keywords -------------------------------------- */

/**
 * @brief Checks whether the current lexeme is exactly a given keyword.
 *
 * The first @p start characters have already been matched by the caller's
 * switch; this verifies the remaining characters and that the lexeme has
 * exactly the keyword's length (so `classy` is not `class`).
 *
 * @param start  Offset in the lexeme where @p rest begins.
 * @param length Length of @p rest.
 * @param rest   Remainder of the keyword after the matched prefix.
 * @param type   Token type to return on a match.
 * @return @p type if it matches, otherwise TOKEN_IDENTIFIER.
 */
static TokenType checkKeyword(int start, int length, const char* rest,
                              TokenType type) {
  if (lexer.current - lexer.start == start + length &&
      memcmp(lexer.start + start, rest, length) == 0) {
    return type;
  }
  return TOKEN_IDENTIFIER;
}

/**
 * @brief Classifies the current lexeme as a keyword or plain identifier.
 *
 * Implemented as a hand-rolled trie: branch on the first character, then
 * (for `f` and `t`, which start several keywords) on the second, and finally
 * compare the tail with checkKeyword().
 */
static TokenType identifierType() {
  switch (lexer.start[0]) {
    case 'a':
      return checkKeyword(1, 2, "nd", TOKEN_AND);
    case 'c':
      return checkKeyword(1, 4, "lass", TOKEN_CLASS);
    case 'e':
      return checkKeyword(1, 3, "lse", TOKEN_ELSE);
    case 'f':
      if (lexer.current - lexer.start > 1) {
        switch (lexer.start[1]) {
          case 'a':
            return checkKeyword(2, 3, "lse", TOKEN_FALSE);
          case 'o':
            return checkKeyword(2, 1, "r", TOKEN_FOR);
          case 'n':
            return checkKeyword(2, 0, "", TOKEN_FN);
        }
      }
      break;
    case 'i':
      return checkKeyword(1, 1, "f", TOKEN_IF);
    case 'n':
      return checkKeyword(1, 3, "ull", TOKEN_NULL);
    case 'o':
      return checkKeyword(1, 1, "r", TOKEN_OR);
    case 'p':
      return checkKeyword(1, 3, "uts", TOKEN_PUTS);
    case 'r':
      return checkKeyword(1, 5, "eturn", TOKEN_RETURN);
    case 's':
      return checkKeyword(1, 4, "uper", TOKEN_SUPER);
    case 't':
      if (lexer.current - lexer.start > 1) {
        switch (lexer.start[1]) {
          case 'h':
            return checkKeyword(2, 2, "is", TOKEN_THIS);
          case 'r':
            return checkKeyword(2, 2, "ue", TOKEN_TRUE);
        }
      }
      break;
    case 'l':
      return checkKeyword(1, 2, "et", TOKEN_LET);
    case 'w':
      return checkKeyword(1, 4, "hile", TOKEN_WHILE);
  }

  return TOKEN_IDENTIFIER;
}

/** @brief Scans the rest of an identifier/keyword (first char already
 *         consumed) and returns the appropriately typed token. */
static Token identifier() {
  while (isAlpha(peek()) || isDigit(peek())) {
    advance();
  }

  return makeToken(identifierType());
}

/* ---- Literals ------------------------------------------------------ */

/** @brief Scans a number: digits, optionally followed by `.` and more
 *         digits. A trailing `.` with no digit after it is not consumed. */
static Token number() {
  while (isDigit(peek())) {
    advance();
  }

  if (peek() == '.' && isDigit(peekNext())) {
    advance();  // consume the '.'

    while (isDigit(peek())) {
      advance();
    }
  }
  return makeToken(TOKEN_NUMBER);
}

/**
 * @brief Scans a string literal (opening quote already consumed).
 *
 * Strings may span lines (the line counter is bumped for embedded newlines).
 * The produced lexeme INCLUDES both quotes; the compiler strips them. There
 * are no escape sequences.
 *
 * @return TOKEN_STRING, or an error token if no closing quote is found.
 */
static Token string() {
  while (peek() != '"' && !isAtEnd()) {
    if (peek() == '\n') lexer.line++;
    advance();
  }

  if (isAtEnd()) {
    return errorToken("Unterminated string");
  }

  advance();  // closing quote
  return makeToken(TOKEN_STRING);
}

/* ---- Public API ---------------------------------------------------- */

/**
 * @brief Scans and returns the next token.
 *
 * Skips whitespace/comments, then marks the start of the new lexeme and
 * dispatches on its first character: identifier/keyword, number, one- or
 * two-character operator, or string. Returns TOKEN_EOF forever at end of
 * input and TOKEN_ERROR for unrecognized characters.
 *
 * @return The next Token (valid as long as the source buffer is).
 */
Token scanToken() {
  skipWhitespace();

  lexer.start = lexer.current;

  if (isAtEnd()) {
    return makeToken(TOKEN_EOF);
  }

  char c = advance();

  if (isAlpha(c)) {
    return identifier();
  }

  if (isDigit(c)) {
    return number();
  }

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
    case '"':
      return string();
  }

  return errorToken("Unexpected character.");
}
