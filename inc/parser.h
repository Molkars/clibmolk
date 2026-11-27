#ifndef MOLKIE_PARSER_H
#define MOLKIE_PARSER_H

#include <stddef.h>

#include "attr.h"
#include "cstr.h"
#include "err.h"
#include "io.h"
#include "str.h"

/// A recursive-descent text parser. Look at the next bytes with `peek*`, eat
/// them with `take*`, `expect*` or `p_*`; a failed `expect*` / `p_*` returns an
/// `Error` that says where (`at line:col`).
///
/// The input is either a `Reader` (read lazily, kept in memory so you can
/// backtrack and report positions) or bytes you already have (borrowed, not
/// copied). Don't move a Parser while a group made from it is open.
///
/// A read error ends the input early and is remembered: `Parser_end`,
/// `Parser_groupEnd`, `Parser_ioerr` and any failing `expect*` / `p_*` report
/// it. A parser that never reaches one of those can mistake a failed read for
/// the end of the input.
typedef struct Parser Parser;
struct Parser {
  Parser *root;      // parser that holds the bytes; NULL for a root itself
  Parser *parent;    // groups only
  Reader r;          // root: where more bytes come from (read == NULL: none)
  String own;        // root: bytes pulled from `r` so far
  const char *ext;   // root: borrowed bytes (Parser_newBytes), else NULL
  size_t extLen;
  int eof;           // root: no more bytes will arrive
  Error err;         // root: first read or allocation error, until reported
  size_t pos, end;   // the window [pos, end); `end` is SIZE_MAX for a root
  size_t resume;     // group: where the parent continues after groupEnd
};

/// A parser that reads [r] on demand. Pair with `Parser_free`
void Parser_new(Parser *p, Reader r);

/// A parser over the [n] bytes at [s]. They are borrowed: keep them alive and
/// unchanged until `Parser_free`. To parse a piece of a String, pass that
/// piece: `Parser_newBytes(&p, str.buf + from, len)`
void Parser_newBytes(Parser *p, const char *s, size_t n);

/// a parser over the C string [s] (borrowed, see [Parser_newBytes])
void Parser_newc(Parser *p, const char *s);

/// a parser over the contents of [s] (borrowed, see [Parser_newBytes])
void Parser_newString(Parser *p, const String *s);

/// frees what the parser holds. Safe on a group (does nothing)
void Parser_free(Parser *p);

// ---- looking and taking ---------------------------------------------------
//
// These can't fail, so they return the byte or 0. A real NUL byte in the input
// also reads as 0: use `Parser_eof` to tell the end from a NUL.

/// the next byte without consuming it, or 0 at the end
char Parser_peek1(Parser *p);

/// consumes and returns the next byte, or returns 0 at the end
char Parser_take1(Parser *p);

/// the byte [i] ahead (`Parser_peekn(p, 0)` is `Parser_peek1`) without
/// consuming anything, or 0 if the input ends first
char Parser_peekn(Parser *p, size_t i);

/// true if nothing is left (in this group, for a group)
int Parser_eof(Parser *p);

/// the next byte if it is one of the chars in [set], else 0. Nothing is
/// consumed. `Parser_peekc(p, "+-")`
char Parser_peekc(Parser *p, const char *set);

/// like [Parser_peekc], and consumes the byte when it matches
char Parser_takec(Parser *p, const char *set);

/// the first of the NULL-terminated [strs] that the input starts with, or NULL.
/// Nothing is consumed. List longer alternatives first (`"<=", "<"`), the
/// first match wins. Use through the macro:
/// `Parser_peeks(p, "true", "false")`
const char *Parser_peeks_(Parser *p, const char *const *strs);
#define Parser_peeks(p, ...)                                                   \
  Parser_peeks_((p), (const char *const[]){__VA_ARGS__, NULL})

/// like [Parser_peeks_], and consumes the match
const char *Parser_takes_(Parser *p, const char *const *strs);
#define Parser_takes(p, ...)                                                   \
  Parser_takes_((p), (const char *const[]){__VA_ARGS__, NULL})

/// consumes whitespace, newlines included
void Parser_skipws(Parser *p);

/// consumes spaces and tabs only, so a line-based format can stay on its line
void Parser_skipblank(Parser *p);

/// skips whitespace, then true if nothing is left (in this group, for a
/// group). The loop over a sequence is `while (!Parser_done(p)) { ... }`
int Parser_done(Parser *p);

/// the current position, to come back to with [Parser_reset]. Valid for the
/// parser it came from (a group has its own range)
size_t Parser_mark(const Parser *p);

/// goes back to [mark]. The bytes read since are still in memory, so this is
/// free. Don't reset past a group's start
void Parser_reset(Parser *p, size_t mark);

/// the 1-based line and column of the current position, counted from the first
/// byte the root parser was given. [line] or [col] may be NULL
void Parser_loc(Parser *p, size_t *line, size_t *col);

// ---- expecting ------------------------------------------------------------

/// Your own failure, with the position added: `code` and a printf-style
/// message, giving `<message> at line:col`. If a read error cut the input
/// short, that one is returned instead, since it is the likelier cause
NODISCARD Error Parser_fail(Parser *p, int code, const char *fmt, ...)
    PRINTF_LIKE(3, 4);

/// consumes [c], or fails with `EINVAL` (`expected ')', found 'x' at 3:7`)
NODISCARD Error Parser_expectc(Parser *p, char c);

/// consumes the string [s], or fails with `EINVAL`
NODISCARD Error Parser_expects(Parser *p, const char *s);

/// skips whitespace, then fails with `EINVAL` if anything is left. Reports a
/// read error too. Call at the end of a whole parse
NODISCARD Error Parser_end(Parser *p);

/// skips spaces and tabs, then true if the line is over: at `\n`, `\r\n` or
/// the end of the input. Nothing past the blanks is consumed
int Parser_atLineEnd(Parser *p);

/// skips spaces and tabs, then consumes the end of the line (`\n` or `\r\n`).
/// The end of the input counts as one, so a last line needs no newline. Fails
/// with `EINVAL` if anything else is there
NODISCARD Error Parser_expectNewline(Parser *p);

/// takes and returns the read or allocation error that cut the input short
/// (ok if there is none). The caller owns it
NODISCARD Error Parser_ioerr(Parser *p);

// ---- extracting -----------------------------------------------------------

/// Parses a number at the current position. The token is the longest run that
/// looks like one and the value must pass the matching `cstr_to_*` (`EINVAL`
/// if there is no number here, `ERANGE` if it doesn't fit). On failure nothing
/// is consumed and `*out` is untouched. A number ends where its characters
/// do: `"12abc"` gives 12 and leaves `abc`.
/// - integers: digits, with an optional `+`/`-` for the signed types
/// - floats: `[+-]digits[.digits][e[+-]digits]`, or letters (`inf`, `nan`)
/// - bool: `true` or `false`
///
/// `Parser_p_<name>(Parser *, T *)` for each (T, name) in `CSTR_TO_TYPES`,
/// `Parser_p_sizet`, or let the destination pick: `Parser_p(p, &n)`
#define PARSER_DECLARE_P_(T, N)                                                \
  NODISCARD Error Parser_p_##N(Parser *p, T *out);
CSTR_TO_TYPES(PARSER_DECLARE_P_)
#undef PARSER_DECLARE_P_

NODISCARD Error Parser_p_sizet(Parser *p, size_t *out);

#define PARSER_P_ASSOC_(T, N) T *: Parser_p_##N,
#define Parser_p(p, dst)                                                       \
  _Generic((dst), CSTR_TO_TYPES(PARSER_P_ASSOC_) struct parser_no_parse_ *:    \
               Parser_p_int)((p), (dst))

/// replaces [out] with the bytes up to (not including) the first one in
/// [stop], or up to the end if [stop] is NULL or never matches. The stop byte
/// is not consumed. Fails only if memory runs out
NODISCARD Error Parser_p_until(Parser *p, const char *stop, String *out);

/// replaces [out] with the rest of the line, without the `\n` or `\r\n`, and
/// consumes the newline. A last line with no newline is returned as is. At the
/// end of the input it gives an empty string, so check `Parser_eof` first:
/// `while (!Parser_eof(p)) { etry(Parser_p_line(p, &line)); ... }`
NODISCARD Error Parser_p_line(Parser *p, String *out);

/// parses one item of a list, see [Parser_p_list]
typedef Error ParserItemFn(Parser *p, void *ctx);

/// Parses zero or more items with [item] up to the end of the input (of the
/// group, for a group), calling `item(p, ctx)` for each. Whitespace around the
/// items is skipped. Items are separated by the string [sep], or by whitespace
/// alone if [sep] is NULL. A missing separator or a trailing one (the item then
/// sees the end of the input and fails) is an error, and so is an item that
/// consumes nothing. The list is empty if there is nothing to parse
NODISCARD Error Parser_p_list(Parser *p, const char *sep, ParserItemFn *item,
                              void *ctx);

/// replaces [out] with the longest run of bytes for which [pred] (an `isdigit`
/// style function) is true: `Parser_p_while(p, isalpha, &word)`. May be empty
NODISCARD Error Parser_p_while(Parser *p, int (*pred)(int), String *out);

// ---- groups ---------------------------------------------------------------

/// Parses a delimited piece on its own. The input must be at [start]; [sub]
/// becomes a parser over what lies between [start] and its matching [end], so
/// it can't read past the end. When [start] and [end] differ the group nests
/// (`( a (b) c )` ends at the last `)`); when they are the same it ends at the
/// next one. There are no escapes. Fails with `EINVAL` if the input isn't at
/// [start] or the group is never closed. Nothing is consumed from [p] until
/// [Parser_groupEnd]
///
/// [sub] is a plain value, no free needed:
/// ```
/// Parser g;
/// etry(Parser_group(p, &g, "(", ")"));
/// ... parse from g ...
/// etry(Parser_groupEnd(&g));      // g must be used up; p moves past ')'
/// ```
NODISCARD Error Parser_group(Parser *p, Parser *sub, const char *start,
                             const char *end);

/// Finishes [sub]: skips whitespace, fails with `EINVAL` if anything is left
/// in the group, and moves the parent past the group's [end] string
NODISCARD Error Parser_groupEnd(Parser *sub);

#endif // MOLKIE_PARSER_H

#if defined(MOLKIE_PARSER_IMPL) && !defined(MOLKIE_PARSER_IMPL_DONE)
#define MOLKIE_PARSER_IMPL_DONE
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---- bytes ----------------------------------------------------------------

static Parser *_proot(Parser *p) { return p->root ? p->root : p; }

static size_t _plen(Parser *root) {
  return root->ext ? root->extLen : root->own.len;
}

static const char *_pbytes(Parser *root) {
  return root->ext ? root->ext : String_cstr(&root->own);
}

// reads one more chunk into the root's buffer, or marks the end
static void _ppull(Parser *root) {
  enum { CHUNK = 4096 };
  if (root->eof)
    return;
  if (!root->r.read) {
    root->eof = 1;
    return;
  }
  if (!String_grow(&root->own, CHUNK)) {
    root->eof = 1;
    if (root->err.ok)
      root->err = enew(ENOMEM, "parser: out of memory");
    return;
  }
  size_t n = 0;
  Error e = Rread(root->r, root->own.buf + root->own.len, CHUNK, &n);
  if (!e.ok) {
    root->eof = 1;
    if (root->err.ok)
      root->err = e;
    else
      efree(&e);
    return;
  }
  root->own.len += n;
  root->own.buf[root->own.len] = 0;
  if (n < CHUNK) // a short read is the end of input
    root->eof = 1;
}

// how many bytes (at most [want]) can be read at the position, never past the
// end of a group
static size_t _pavail(Parser *p, size_t want) {
  Parser *root = _proot(p);
  size_t room = p->end - p->pos;
  if (want > room)
    want = room;
  while (_plen(root) - p->pos < want && !root->eof)
    _ppull(root);
  size_t have = _plen(root) - p->pos;
  return have < want ? have : want;
}

// the bytes at the position: valid for what the last _pavail promised
static const char *_pat(Parser *p) { return _pbytes(_proot(p)) + p->pos; }

// the byte [i] ahead, or -1 past the end
static int _pbyte(Parser *p, size_t i) {
  return _pavail(p, i + 1) > i ? (unsigned char)_pat(p)[i] : -1;
}

// true if the [n] bytes [i] ahead are [s]
static int _pmatch(Parser *p, size_t i, const char *s, size_t n) {
  return _pavail(p, i + n) >= i + n && memcmp(_pat(p) + i, s, n) == 0;
}

// ---- construction ---------------------------------------------------------

void Parser_new(Parser *p, Reader r) {
  assert(p && "parser is null");
  *p = (Parser){0};
  p->r = r;
  p->own = String_empty();
  p->err = eok();
  p->end = SIZE_MAX;
  p->eof = r.read == NULL;
}

void Parser_newBytes(Parser *p, const char *s, size_t n) {
  Parser_new(p, (Reader){0});
  p->ext = s;
  p->extLen = s ? n : 0;
}

void Parser_newc(Parser *p, const char *s) {
  assert(s && "string is null");
  Parser_newBytes(p, s, strlen(s));
}

void Parser_newString(Parser *p, const String *s) {
  assert(s && "string is null");
  Parser_newBytes(p, s->buf, s->len);
}

void Parser_free(Parser *p) {
  if (!p || p->root)
    return;
  String_free(&p->own);
  efree(&p->err);
  p->err = eok();
}

// ---- errors ---------------------------------------------------------------

void Parser_loc(Parser *p, size_t *line, size_t *col) {
  assert(p && "parser is null");
  const char *b = _pbytes(_proot(p));
  size_t l = 1, c = 1;
  for (size_t i = 0; i < p->pos; i++) {
    if (b[i] == '\n') {
      l++;
      c = 1;
    } else {
      c++;
    }
  }
  if (line)
    *line = l;
  if (col)
    *col = c;
}

Error Parser_ioerr(Parser *p) {
  assert(p && "parser is null");
  Parser *root = _proot(p);
  Error e = root->err;
  root->err = eok();
  return e;
}

// (Parser_fail: the read error if there is one, else `code` and the message,
// both with the position)
Error Parser_fail(Parser *p, int code, const char *fmt, ...) {
  size_t line, col;
  Parser_loc(p, &line, &col);
  Error e = Parser_ioerr(p);
  if (!e.ok) {
    ectx(&e, "reading input, at %zu:%zu", line, col);
    return e;
  }
  char m[128];
  va_list argp;
  va_start(argp, fmt);
  vsnprintf(m, sizeof m, fmt, argp);
  va_end(argp);
  return enew(code, "%s at %zu:%zu", m, line, col);
}

// what is at the position, for a message
static void _pfound(Parser *p, char *out, size_t n) {
  int c = _pbyte(p, 0);
  if (c < 0)
    snprintf(out, n, "end of input");
  else if (isprint(c))
    snprintf(out, n, "'%c'", c);
  else
    snprintf(out, n, "0x%02x", c);
}

// ---- looking and taking ---------------------------------------------------

char Parser_peek1(Parser *p) {
  assert(p && "parser is null");
  return _pavail(p, 1) ? _pat(p)[0] : 0;
}

char Parser_take1(Parser *p) {
  assert(p && "parser is null");
  if (!_pavail(p, 1))
    return 0;
  char c = _pat(p)[0];
  p->pos++;
  return c;
}

char Parser_peekn(Parser *p, size_t i) {
  assert(p && "parser is null");
  int c = _pbyte(p, i);
  return c < 0 ? 0 : (char)c;
}

int Parser_eof(Parser *p) {
  assert(p && "parser is null");
  return _pavail(p, 1) == 0;
}

char Parser_peekc(Parser *p, const char *set) {
  assert(set && "set is null");
  char c = Parser_peek1(p);
  return c && strchr(set, c) ? c : 0;
}

char Parser_takec(Parser *p, const char *set) {
  char c = Parser_peekc(p, set);
  if (c)
    p->pos++;
  return c;
}

const char *Parser_peeks_(Parser *p, const char *const *strs) {
  assert(p && strs && "null argument");
  for (; *strs; strs++)
    if (_pmatch(p, 0, *strs, strlen(*strs)))
      return *strs;
  return NULL;
}

const char *Parser_takes_(Parser *p, const char *const *strs) {
  const char *s = Parser_peeks_(p, strs);
  if (s)
    p->pos += strlen(s);
  return s;
}

void Parser_skipws(Parser *p) {
  assert(p && "parser is null");
  while (_pavail(p, 1) && isspace((unsigned char)_pat(p)[0]))
    p->pos++;
}

void Parser_skipblank(Parser *p) {
  assert(p && "parser is null");
  while (_pavail(p, 1) && (_pat(p)[0] == ' ' || _pat(p)[0] == '\t'))
    p->pos++;
}

int Parser_done(Parser *p) {
  Parser_skipws(p);
  return Parser_eof(p);
}

size_t Parser_mark(const Parser *p) {
  assert(p && "parser is null");
  return p->pos;
}

void Parser_reset(Parser *p, size_t mark) {
  assert(p && mark <= p->end && "bad mark");
  p->pos = mark;
}

// ---- expecting ------------------------------------------------------------

Error Parser_expectc(Parser *p, char c) {
  assert(p && "parser is null");
  if (_pavail(p, 1) && _pat(p)[0] == c) {
    p->pos++;
    return eok();
  }
  char found[16];
  _pfound(p, found, sizeof found);
  return Parser_fail(p, EINVAL, "expected '%c', found %s", c, found);
}

Error Parser_expects(Parser *p, const char *s) {
  assert(p && s && "null argument");
  size_t n = strlen(s);
  if (_pmatch(p, 0, s, n)) {
    p->pos += n;
    return eok();
  }
  char found[16];
  _pfound(p, found, sizeof found);
  return Parser_fail(p, EINVAL, "expected \"%s\", found %s", s, found);
}

int Parser_atLineEnd(Parser *p) {
  Parser_skipblank(p);
  return Parser_eof(p) || _pbyte(p, 0) == '\n' ||
         (_pbyte(p, 0) == '\r' && _pbyte(p, 1) == '\n');
}

Error Parser_expectNewline(Parser *p) {
  if (!Parser_atLineEnd(p)) {
    char found[16];
    _pfound(p, found, sizeof found);
    return Parser_fail(p, EINVAL, "expected end of line, found %s", found);
  }
  if (_pbyte(p, 0) == '\r')
    p->pos++;
  if (_pbyte(p, 0) == '\n')
    p->pos++;
  return eok();
}

Error Parser_end(Parser *p) {
  Parser_skipws(p);
  if (!Parser_eof(p)) {
    char found[16];
    _pfound(p, found, sizeof found);
    return Parser_fail(p, EINVAL, "expected end of input, found %s", found);
  }
  return Parser_ioerr(p);
}

// ---- extracting -----------------------------------------------------------

#define PARSER_KIND_short 1
#define PARSER_KIND_int 1
#define PARSER_KIND_long 1
#define PARSER_KIND_llong 1
#define PARSER_KIND_ushort 2
#define PARSER_KIND_uint 2
#define PARSER_KIND_ulong 2
#define PARSER_KIND_ullong 2
#define PARSER_KIND_sizet 2
#define PARSER_KIND_float 3
#define PARSER_KIND_double 3
#define PARSER_KIND_bool 4

static size_t _pdigits(Parser *p, size_t i) {
  while (isdigit(_pbyte(p, i)))
    i++;
  return i;
}

// length of the number token at the position (0: none), by kind (see above)
static size_t _pnumlen(Parser *p, int kind) {
  if (kind == 4) {
    if (_pmatch(p, 0, "true", 4))
      return 4;
    return _pmatch(p, 0, "false", 5) ? 5 : 0;
  }
  size_t i = 0;
  int c = _pbyte(p, 0);
  if (kind != 2 && (c == '-' || c == '+'))
    i++;
  if (kind != 3) {
    size_t j = _pdigits(p, i);
    return j == i ? 0 : j;
  }
  if (isalpha(_pbyte(p, i))) { // inf, infinity, nan: cstr_to_ checks which
    while (isalpha(_pbyte(p, i)))
      i++;
    return i;
  }
  size_t j = _pdigits(p, i);
  if (_pbyte(p, j) == '.' && isdigit(_pbyte(p, j + 1)))
    j = _pdigits(p, j + 1);
  if (j == i)
    return 0;
  if (_pbyte(p, j) == 'e' || _pbyte(p, j) == 'E') {
    size_t k = j + 1;
    if (_pbyte(p, k) == '+' || _pbyte(p, k) == '-')
      k++;
    if (isdigit(_pbyte(p, k)))
      j = _pdigits(p, k);
  }
  return j;
}

// copies the number token into [tok] without consuming it
static Error _pnumtok(Parser *p, int kind, String *tok) {
  size_t n = _pnumlen(p, kind);
  if (n == 0) {
    char found[16];
    _pfound(p, found, sizeof found);
    return Parser_fail(p, EINVAL, "expected a %s, found %s",
                  kind == 4 ? "bool" : "number", found);
  }
  if (!String_extend(tok, _pat(p), n))
    return enew(ENOMEM, "parser: out of memory");
  return eok();
}

// adds the position to a failed conversion
static Error _pconv(Parser *p, Error e) {
  if (!e.ok) {
    size_t line, col;
    Parser_loc(p, &line, &col);
    ectx(&e, "at %zu:%zu", line, col);
  }
  return e;
}

#define PARSER_DEFINE_P_(T, N)                                                 \
  Error Parser_p_##N(Parser *p, T *out) {                                      \
    assert(p && out && "null argument");                                       \
    Error eret = eok();                                                        \
    String tok = String_empty();                                               \
    T v;                                                                       \
    etry(_pnumtok(p, PARSER_KIND_##N, &tok));                                  \
    etry(_pconv(p, cstr_to_##N(String_cstr(&tok), &v)));                       \
    *out = v;                                                                  \
    p->pos += tok.len;                                                         \
  end:                                                                         \
    String_free(&tok);                                                         \
    return eret;                                                               \
  }
CSTR_TO_TYPES(PARSER_DEFINE_P_)
#undef PARSER_DEFINE_P_

Error Parser_p_sizet(Parser *p, size_t *out) {
  assert(p && out && "null argument");
  Error eret = eok();
  String tok = String_empty();
  size_t v;
  etry(_pnumtok(p, PARSER_KIND_sizet, &tok));
  etry(_pconv(p, cstr_to_sizet(String_cstr(&tok), &v)));
  *out = v;
  p->pos += tok.len;
end:
  String_free(&tok);
  return eret;
}

Error Parser_p_until(Parser *p, const char *stop, String *out) {
  assert(p && out && "null argument");
  String_reset(out);
  while (_pavail(p, 1)) {
    char c = _pat(p)[0];
    if (stop && c && strchr(stop, c))
      break;
    if (!String_putc(out, c))
      return enew(ENOMEM, "parser: out of memory");
    p->pos++;
  }
  return eok();
}

Error Parser_p_line(Parser *p, String *out) {
  assert(p && out && "null argument");
  String_reset(out);
  while (_pavail(p, 1) && _pat(p)[0] != '\n') {
    if (!String_putc(out, _pat(p)[0]))
      return enew(ENOMEM, "parser: out of memory");
    p->pos++;
  }
  if (out->len && out->buf[out->len - 1] == '\r')
    out->buf[--out->len] = 0;
  if (_pavail(p, 1))
    p->pos++; // the '\n'
  return eok();
}

Error Parser_p_list(Parser *p, const char *sep, ParserItemFn *item,
                    void *ctx) {
  assert(p && item && "null argument");
  Error eret = eok();
  size_t before = 0;
  int afterSep = 0;
  while (afterSep || !Parser_done(p)) {
    before = Parser_mark(p);
    etry(item(p, ctx));
    if (Parser_mark(p) == before) {
      eret = Parser_fail(p, EINVAL, "list item consumed nothing");
      goto end;
    }
    afterSep = 0;
    if (Parser_done(p))
      break;
    if (sep) {
      etry(Parser_expects(p, sep));
      afterSep = 1;
      Parser_skipws(p);
    }
  }
end:
  return eret;
}

Error Parser_p_while(Parser *p, int (*pred)(int), String *out) {
  assert(p && pred && out && "null argument");
  String_reset(out);
  while (_pavail(p, 1)) {
    char c = _pat(p)[0];
    if (!pred((unsigned char)c))
      break;
    if (!String_putc(out, c))
      return enew(ENOMEM, "parser: out of memory");
    p->pos++;
  }
  return eok();
}

// ---- groups ---------------------------------------------------------------

Error Parser_group(Parser *p, Parser *sub, const char *start, const char *end) {
  assert(p && sub && start && end && "null argument");
  assert(*start && *end && "empty delimiter");
  size_t sl = strlen(start), el = strlen(end);
  int nests = strcmp(start, end) != 0;
  if (!_pmatch(p, 0, start, sl)) {
    char found[16];
    _pfound(p, found, sizeof found);
    return Parser_fail(p, EINVAL, "expected \"%s\", found %s", start, found);
  }
  size_t i = sl, depth = 1;
  for (;;) {
    if (_pavail(p, i + 1) <= i)
      return Parser_fail(p, EINVAL, "\"%s\" is never closed with \"%s\"", start,
                    end);
    if (_pmatch(p, i, end, el)) {
      if (--depth == 0)
        break;
      i += el;
    } else if (nests && _pmatch(p, i, start, sl)) {
      depth++;
      i += sl;
    } else {
      i++;
    }
  }
  *sub = (Parser){0};
  sub->root = _proot(p);
  sub->parent = p;
  sub->err = eok();
  sub->pos = p->pos + sl;
  sub->end = p->pos + i;
  sub->resume = p->pos + i + el;
  return eok();
}

Error Parser_groupEnd(Parser *sub) {
  assert(sub && sub->parent && "not a group");
  Parser_skipws(sub);
  if (!Parser_eof(sub)) {
    char found[16];
    _pfound(sub, found, sizeof found);
    return Parser_fail(sub, EINVAL, "unconsumed %s at the end of the group", found);
  }
  sub->parent->pos = sub->resume;
  return eok();
}

#endif // MOLKIE_PARSER_IMPL

// Tests: compiled where MTEST (all modules) or MOLKIE_PARSER_TEST (just this
// one) is defined. Needs the IMPLs of parser and its deps in the same program.
#if (defined(MTEST) || defined(MOLKIE_PARSER_TEST)) &&                         \
    !defined(MOLKIE_PARSER_TESTS)
#define MOLKIE_PARSER_TESTS
#include <ctype.h>
#include <errno.h>
#include <string.h>

#include "test.h"

static void parser_peek_take(void) {
  Parser p;
  Parser_newc(&p, "ab");
  ASSERT_EQ(Parser_peek1(&p), 'a');
  ASSERT_EQ(Parser_peek1(&p), 'a');
  ASSERT_EQ(Parser_take1(&p), 'a');
  assert(!Parser_eof(&p));
  ASSERT_EQ(Parser_take1(&p), 'b');
  assert(Parser_eof(&p));
  ASSERT_EQ(Parser_peek1(&p), 0);
  ASSERT_EQ(Parser_take1(&p), 0);
  Parser_free(&p);

  Parser_newBytes(&p, "a\0b", 3); // a NUL is a byte, eof says when it ends
  Parser_take1(&p);
  ASSERT_EQ(Parser_take1(&p), 0);
  assert(!Parser_eof(&p));
  ASSERT_EQ(Parser_take1(&p), 'b');
  assert(Parser_eof(&p));
  Parser_free(&p);

  Parser_newBytes(&p, NULL, 0);
  assert(Parser_eof(&p));
  Parser_free(&p);
}

static void parser_peekc_takec(void) {
  Parser p;
  Parser_newc(&p, "-5");
  ASSERT_EQ(Parser_peekc(&p, "+"), 0);
  ASSERT_EQ(Parser_peekc(&p, "+-"), '-');
  ASSERT_EQ(Parser_peekc(&p, "+-"), '-');
  ASSERT_EQ(Parser_takec(&p, "+-"), '-');
  ASSERT_EQ(Parser_takec(&p, "+-"), 0);
  ASSERT_EQ(Parser_peek1(&p), '5');
  Parser_take1(&p);
  ASSERT_EQ(Parser_peekc(&p, ""), 0); // the end never matches, not even ""
  Parser_free(&p);
}

static void parser_peeks_takes(void) {
  Parser p;
  Parser_newc(&p, "<=3");
  const char *m = Parser_peeks(&p, "<=", "<");
  assert(m && !strcmp(m, "<="));
  assert(Parser_peeks(&p, "x", ">") == NULL);
  m = Parser_peeks(&p, "<", "<=");  // the first listed wins
  assert(m && !strcmp(m, "<"));
  m = Parser_takes(&p, "<=", "<");
  assert(m && !strcmp(m, "<="));
  ASSERT_EQ(Parser_peek1(&p), '3');
  assert(Parser_takes(&p, "3333") == NULL); // longer than what is left
  assert(Parser_takes(&p, "3") != NULL);
  assert(Parser_takes(&p, "x") == NULL);
  Parser_free(&p);
}

static void parser_expect(void) {
  Parser p;
  Parser_newc(&p, "ab\ncd");
  assert(Parser_expectc(&p, 'a').ok);
  Error e = Parser_expectc(&p, 'x');
  ASSERT_EQ(e.code, EINVAL);
  assert(strstr(e.msg, "expected 'x', found 'b'") && strstr(e.msg, "1:2"));
  efree(&e);
  assert(Parser_expects(&p, "b\nc").ok);
  e = Parser_expects(&p, "dd");
  assert(strstr(e.msg, "expected \"dd\", found 'd'") && strstr(e.msg, "2:2"));
  efree(&e);
  assert(Parser_expectc(&p, 'd').ok);
  e = Parser_expectc(&p, 'e');
  assert(strstr(e.msg, "found end of input") && strstr(e.msg, "2:3"));
  efree(&e);
  Parser_free(&p);
}

static void parser_numbers(void) {
  Parser p;
  size_t z = 0;
  int i = 0;
  double d = 0;
  _Bool b = 0;
  Parser_newc(&p, "42 -7 2.5e3 true +x 12abc");
  assert(Parser_p_sizet(&p, &z).ok && z == 42);
  Parser_skipws(&p);
  assert(Parser_p(&p, &i).ok && i == -7);
  Parser_skipws(&p);
  assert(Parser_p(&p, &d).ok && d == 2500.0);
  Parser_skipws(&p);
  assert(Parser_p(&p, &b).ok && b);
  Parser_skipws(&p);
  Error e = Parser_p(&p, &i); // a bare sign is not a number: nothing consumed
  ASSERT_EQ(e.code, EINVAL);
  assert(strstr(e.msg, "expected a number, found '+'"));
  efree(&e);
  ASSERT_EQ(Parser_peek1(&p), '+');
  Parser_take1(&p);
  Parser_take1(&p);
  Parser_skipws(&p);
  assert(Parser_p(&p, &i).ok && i == 12);
  ASSERT_EQ(Parser_peek1(&p), 'a');
  Parser_free(&p);
}

static void parser_number_errors(void) {
  Parser p;
  size_t z = 7;
  Parser_newc(&p, "99999999999999999999999 -5 ");
  Error e = Parser_p_sizet(&p, &z);
  ASSERT_EQ(e.code, ERANGE);
  assert(z == 7 && strstr(e.msg, "1:1") == NULL); // the position is in ctx
  int located = 0;
  for (ErrCtx *c = e.ctx; c; c = c->next)
    located |= strstr(c->msg, "at 1:1") != NULL;
  assert(located);
  efree(&e);
  ASSERT_EQ(Parser_mark(&p), 0); // not consumed
  Parser_reset(&p, 24);
  e = Parser_p_sizet(&p, &z); // unsigned: the sign is not part of the number
  ASSERT_EQ(e.code, EINVAL);
  efree(&e);
  Parser_free(&p);

  Parser_newc(&p, "1.5.3 .5 1e 3");
  double d;
  assert(Parser_p(&p, &d).ok && d == 1.5);
  ASSERT_EQ(Parser_peek1(&p), '.');
  Parser_take1(&p);
  assert(Parser_p(&p, &d).ok && d == 3.0);
  Parser_skipws(&p);
  assert(Parser_p(&p, &d).ok && d == 0.5);
  Parser_skipws(&p);
  assert(Parser_p(&p, &d).ok && d == 1.0); // "1e" is 1 then 'e'
  ASSERT_EQ(Parser_peek1(&p), 'e');
  Parser_free(&p);
}

static void parser_until_while(void) {
  Parser p;
  String s = String_fromc("stale");
  Parser_newc(&p, "key=value;rest");
  assert(Parser_p_while(&p, isalpha, &s).ok && String_eq(&s, "key"));
  assert(Parser_expectc(&p, '=').ok);
  assert(Parser_p_until(&p, ";,", &s).ok && String_eq(&s, "value"));
  ASSERT_EQ(Parser_peek1(&p), ';');
  Parser_take1(&p);
  assert(Parser_p_until(&p, NULL, &s).ok && String_eq(&s, "rest"));
  assert(Parser_eof(&p));
  assert(Parser_p_until(&p, ",", &s).ok && s.len == 0);
  String_free(&s);
  Parser_free(&p);
}

static void parser_mark_reset(void) {
  Parser p;
  Parser_newc(&p, "abc");
  size_t m = Parser_mark(&p);
  Parser_take1(&p);
  Parser_take1(&p);
  Parser_reset(&p, m);
  ASSERT_EQ(Parser_peek1(&p), 'a');
  Parser_free(&p);
}

static void parser_end(void) {
  Parser p;
  Parser_newc(&p, "  \n ");
  assert(Parser_end(&p).ok);
  Parser_free(&p);
  Parser_newc(&p, " x");
  Error e = Parser_end(&p);
  ASSERT_EQ(e.code, EINVAL);
  assert(strstr(e.msg, "found 'x'"));
  efree(&e);
  Parser_free(&p);
}

static void parser_group(void) {
  Parser p, g, h;
  String s = String_empty();
  Parser_newc(&p, "(a (b) c) rest");
  assert(Parser_group(&p, &g, "(", ")").ok);
  ASSERT_EQ(Parser_peek1(&p), '('); // the parent hasn't moved
  assert(Parser_p_while(&g, isalpha, &s).ok && String_eq(&s, "a"));
  Parser_skipws(&g);
  assert(Parser_group(&g, &h, "(", ")").ok); // groups nest
  assert(Parser_p_until(&h, NULL, &s).ok && String_eq(&s, "b"));
  assert(Parser_groupEnd(&h).ok);
  Parser_skipws(&g);
  ASSERT_EQ(Parser_take1(&g), 'c');
  assert(Parser_eof(&g)); // the group can't see ") rest"
  assert(Parser_groupEnd(&g).ok);
  Parser_skipws(&p);
  assert(Parser_p_until(&p, NULL, &s).ok && String_eq(&s, "rest"));
  String_free(&s);
  Parser_free(&p);
}

static void parser_group_errors(void) {
  Parser p, g;
  Parser_newc(&p, "(ab (c) ");
  Error e = Parser_group(&p, &g, "(", ")");
  ASSERT_EQ(e.code, EINVAL);
  assert(strstr(e.msg, "never closed"));
  efree(&e);
  e = Parser_group(&p, &g, "[", "]");
  assert(strstr(e.msg, "expected \"[\", found '('"));
  efree(&e);
  Parser_free(&p);

  Parser_newc(&p, "(ab)x");
  assert(Parser_group(&p, &g, "(", ")").ok);
  Parser_take1(&g);
  e = Parser_groupEnd(&g); // 'b' is left over
  ASSERT_EQ(e.code, EINVAL);
  assert(strstr(e.msg, "unconsumed 'b'") && strstr(e.msg, "1:3"));
  efree(&e);
  ASSERT_EQ(Parser_peek1(&p), '('); // the parent did not move
  Parser_free(&p);

  // same delimiter: no nesting; multi-char delimiters; empty group
  Parser_newc(&p, "\"a\"b\"");
  assert(Parser_group(&p, &g, "\"", "\"").ok);
  ASSERT_EQ(Parser_take1(&g), 'a');
  assert(Parser_groupEnd(&g).ok);
  ASSERT_EQ(Parser_peek1(&p), 'b');
  Parser_free(&p);
  Parser_newc(&p, "<<>><<x>> ");
  assert(Parser_group(&p, &g, "<<", ">>").ok && Parser_eof(&g));
  assert(Parser_groupEnd(&g).ok);
  assert(Parser_group(&p, &g, "<<", ">>").ok);
  ASSERT_EQ(Parser_take1(&g), 'x');
  assert(Parser_groupEnd(&g).ok);
  Parser_free(&p);
}

typedef struct {
  size_t id;
  String name;
  size_t nums[8];
  size_t n;
} Rec;

// "<number> (<some chars>) <number> <number> ... <number>"
static Error rec_parse(Parser *p, Rec *out) {
  Error eret = eok();
  Parser g;
  etry(Parser_p_sizet(p, &out->id));
  Parser_skipws(p);
  etry(Parser_group(p, &g, "(", ")"));
  etry(Parser_p_until(&g, NULL, &out->name));
  etry(Parser_groupEnd(&g));
  while (!Parser_done(p)) {
    if (out->n == 8) {
      eret = Parser_fail(p, E2BIG, "too many numbers");
      goto end;
    }
    etry(Parser_p_sizet(p, &out->nums[out->n++]));
  }
  etry(Parser_end(p));
end:
  return eret;
}

static void parser_record(void) {
  Parser p;
  Rec r = {0};
  Parser_newc(&p, "12 (hello world) 1 2 3\n");
  assert(rec_parse(&p, &r).ok);
  ASSERT_EQ(r.id, 12);
  assert(String_eq(&r.name, "hello world"));
  ASSERT_EQ(r.n, 3);
  assert(r.nums[0] == 1 && r.nums[1] == 2 && r.nums[2] == 3);
  String_free(&r.name);
  Parser_free(&p);

  Rec bad = {0};
  Parser_newc(&p, "5 (x)\n7 oops");
  Error e = rec_parse(&p, &bad);
  assert(!e.ok && e.ctx);
  assert(strstr(e.msg, "expected a number, found 'o'") && strstr(e.msg, "2:3"));
  efree(&e);
  String_free(&bad.name);
  Parser_free(&p);
}

static void parser_fail(void) {
  Parser p;
  Parser_newc(&p, "ab\ncd");
  Parser_take1(&p);
  Parser_take1(&p);
  Parser_take1(&p);
  Error e = Parser_fail(&p, E2BIG, "too many %s (%d)", "things", 3);
  ASSERT_EQ(e.code, E2BIG);
  assert(!strcmp(e.msg, "too many things (3) at 2:1"));
  efree(&e);
  Parser_free(&p);
}

static void parser_peekn(void) {
  Parser p;
  Parser_newc(&p, "-12");
  ASSERT_EQ(Parser_peekn(&p, 0), '-');
  ASSERT_EQ(Parser_peekn(&p, 2), '2');
  ASSERT_EQ(Parser_peekn(&p, 3), 0);
  ASSERT_EQ(Parser_peekn(&p, 9999), 0);
  ASSERT_EQ(Parser_mark(&p), 0); // nothing consumed
  Parser_free(&p);
}

static void parser_done(void) {
  Parser p;
  Parser_newc(&p, " \n ");
  assert(Parser_done(&p));
  Parser_free(&p);
  Parser_newc(&p, " \n x");
  assert(!Parser_done(&p));
  ASSERT_EQ(Parser_peek1(&p), 'x'); // the whitespace was skipped
  Parser_free(&p);
}

static void parser_lines(void) {
  Parser p;
  size_t a, b, sum = 0, lines = 0;
  Parser_newc(&p, "1 2\n3\t4\r\n5 6");
  while (!Parser_eof(&p)) {
    Parser_skipblank(&p);
    assert(Parser_p_sizet(&p, &a).ok);
    Parser_skipblank(&p);
    assert(Parser_p_sizet(&p, &b).ok);
    assert(Parser_expectNewline(&p).ok); // "\r\n" too, and the end of input
    sum += a * 10 + b;
    lines++;
  }
  ASSERT_EQ(lines, 3);
  ASSERT_EQ(sum, 12 + 34 + 56);
  Parser_free(&p);

  // skipblank stops at the newline, where skipws would not
  Parser_newc(&p, "1\n2");
  assert(Parser_p_sizet(&p, &a).ok);
  Parser_skipblank(&p);
  assert(Parser_atLineEnd(&p));
  Error e = Parser_p_sizet(&p, &b); // the number is on the next line
  ASSERT_EQ(e.code, EINVAL);
  efree(&e);
  Parser_free(&p);

  Parser_newc(&p, "1 x");
  assert(Parser_p_sizet(&p, &a).ok);
  e = Parser_expectNewline(&p);
  ASSERT_EQ(e.code, EINVAL);
  assert(strstr(e.msg, "expected end of line, found 'x'"));
  efree(&e);
  Parser_free(&p);
}

static void parser_p_line(void) {
  Parser p;
  String s = String_fromc("stale");
  Parser_newc(&p, "ab c\r\n\nlast");
  assert(Parser_p_line(&p, &s).ok && String_eq(&s, "ab c"));
  assert(Parser_p_line(&p, &s).ok && String_eq(&s, ""));
  assert(!Parser_eof(&p));
  assert(Parser_p_line(&p, &s).ok && String_eq(&s, "last"));
  assert(Parser_eof(&p));
  assert(Parser_p_line(&p, &s).ok && s.len == 0);
  String_free(&s);
  Parser_free(&p);
}

typedef struct {
  size_t v[8];
  size_t n;
} Nums;

static Error nums_item(Parser *p, void *ctx) {
  Nums *o = ctx;
  return Parser_p_sizet(p, &o->v[o->n++ % 8]);
}

static Error nothing_item(Parser *p, void *ctx) {
  (void)p;
  (void)ctx;
  return eok();
}

static void parser_list(void) {
  Parser p;
  Nums n = {0};
  Parser_newc(&p, " 1, 2 ,3\n");
  assert(Parser_p_list(&p, ",", nums_item, &n).ok);
  assert(n.n == 3 && n.v[0] == 1 && n.v[1] == 2 && n.v[2] == 3);
  assert(Parser_eof(&p));
  Parser_free(&p);

  n = (Nums){0};
  Parser_newc(&p, "4 5\n6");
  assert(Parser_p_list(&p, NULL, nums_item, &n).ok);
  assert(n.n == 3 && n.v[2] == 6);
  Parser_free(&p);

  n = (Nums){0};
  Parser_newc(&p, "  ");
  assert(Parser_p_list(&p, ",", nums_item, &n).ok && n.n == 0);
  Parser_free(&p);

  // a list stops at the end of its group
  Parser g;
  n = (Nums){0};
  Parser_newc(&p, "(1,2) 3");
  assert(Parser_group(&p, &g, "(", ")").ok);
  assert(Parser_p_list(&g, ",", nums_item, &n).ok && n.n == 2);
  assert(Parser_groupEnd(&g).ok);
  Parser_free(&p);

  const char *bad[] = {"1,", "1 2", "1,,2", ",1"};
  for (size_t i = 0; i < 4; i++) {
    n = (Nums){0};
    Parser_newc(&p, bad[i]);
    Error e = Parser_p_list(&p, ",", nums_item, &n);
    assert(!e.ok && e.code == EINVAL);
    efree(&e);
    Parser_free(&p);
  }

  Parser_newc(&p, "x");
  Error e = Parser_p_list(&p, NULL, nothing_item, NULL);
  assert(!e.ok && strstr(e.msg, "consumed nothing"));
  efree(&e);
  Parser_free(&p);
}

static void parser_stream(void) {
  // more than one read chunk, so the buffer grows while groups are open
  String in = String_empty();
  size_t want = 0;
  assert(String_puts(&in, "(") && String_puts(&in, "x"));
  for (size_t i = 0; i < 3000; i++) {
    assert(String_printf(&in, " %zu", i));
    want += i;
  }
  assert(String_puts(&in, ") 5 6"));
  Cursor c = Cursor_init(in.buf, in.len);
  Parser p, g;
  Parser_new(&p, Cursor_reader(&c));
  assert(Parser_group(&p, &g, "(", ")").ok);
  assert(Parser_expectc(&g, 'x').ok);
  size_t sum = 0, v, cnt = 0;
  for (Parser_skipws(&g); !Parser_eof(&g); Parser_skipws(&g)) {
    assert(Parser_p_sizet(&g, &v).ok);
    sum += v;
    cnt++;
  }
  ASSERT_EQ(cnt, 3000);
  ASSERT_EQ(sum, want);
  assert(Parser_groupEnd(&g).ok);
  Parser_skipws(&p);
  assert(Parser_p_sizet(&p, &v).ok && v == 5);
  Parser_skipws(&p);
  assert(Parser_p_sizet(&p, &v).ok && v == 6);
  assert(Parser_end(&p).ok);
  Parser_free(&p);
  String_free(&in);
}

static Error parser_failing_read(void *ctx, char *buf, size_t n,
                                 size_t *nRead) {
  (void)buf;
  (void)n;
  (void)nRead;
  (void)ctx;
  return enew(EIO, "disk on fire");
}

static void parser_read_error(void) {
  Parser p;
  Parser_new(&p, (Reader){parser_failing_read, NULL});
  ASSERT_EQ(Parser_peek1(&p), 0); // looks like the end...
  Error e = Parser_end(&p);       // ...but the error is not lost
  ASSERT_EQ(e.code, EIO);
  efree(&e);
  assert(Parser_ioerr(&p).ok); // reported once
  Parser_free(&p);

  Parser_new(&p, (Reader){parser_failing_read, NULL});
  e = Parser_expectc(&p, 'a'); // a failure reports the read error, not "found"
  ASSERT_EQ(e.code, EIO);
  assert(e.ctx && strstr(e.ctx->msg, "at 1:1"));
  efree(&e);
  Parser_free(&p);
}

static void parser_subsection(void) {
  String s = String_fromc("skip [1 2] skip");
  Parser p;
  size_t a, b;
  Parser_newBytes(&p, s.buf + 6, 3); // just "1 2"
  assert(Parser_p_sizet(&p, &a).ok);
  Parser_skipws(&p);
  assert(Parser_p_sizet(&p, &b).ok && a == 1 && b == 2);
  assert(Parser_end(&p).ok);
  Parser_free(&p);
  Parser_newString(&p, &s);
  assert(Parser_expects(&p, "skip").ok);
  Parser_free(&p);
  String_free(&s);
}

static int parser_tests(void) {
  static const TestCase tests[] = {
      T(parser_peek_take),    T(parser_peekc_takec), T(parser_peeks_takes),
      T(parser_expect),       T(parser_numbers),     T(parser_number_errors),
      T(parser_until_while),  T(parser_mark_reset),  T(parser_end),
      T(parser_group),        T(parser_group_errors), T(parser_record),
      T(parser_stream),       T(parser_read_error),  T(parser_subsection),
      T(parser_fail),         T(parser_peekn),       T(parser_done),
      T(parser_lines),        T(parser_p_line),      T(parser_list),
  };
  size_t n = sizeof tests / sizeof *tests, passed = 0;
  for (size_t i = 0; i < n; i++)
    passed += run_in_child(&tests[i]);
  printf("parser: %zu/%zu passed\n", passed, n);
  return passed == n;
}

#endif // MTEST
