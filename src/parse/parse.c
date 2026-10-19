/*
 * SPDX-License-Identifier: LGPL-3.0-only
 * Copyright (C) 2026 Corey Pennycuff
 */
#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/lang-wasm/macros.h>
#include <ghoti.io/lang-wasm/parse.h>

#include <string.h>

struct GLWASM_Text {
  GLWASM_Allocator allocator;
  GLWASM_TextKind kind;
  GLWASM_SourceLoc loc;
  uint8_t * bytes;
  size_t length;
  struct GLWASM_Text ** children;
  size_t count;
  size_t capacity;
};

typedef struct Parser {
  const uint8_t * s;
  size_t n, p;
  uint32_t line, col;
  const GLWASM_Allocator * a;
  GLWASM_ParseError * error;
  GLWASM_Result result;
} Parser;

static GLWASM_SourceLoc loc(Parser * p) {
  GLWASM_SourceLoc l = {p->p, p->line, p->col};
  return l;
}
static void advance(Parser * p) {
  if (p->s[p->p++] == '\n') {
    p->line++;
    p->col = 1;
  }
  else
    p->col++;
}
static int fail(Parser * p, const char * message) {
  if (p->result == GLWASM_OK) {
    p->result = GLWASM_ERR_CORRUPT;
    if (p->error) {
      p->error->result = p->result;
      p->error->location = loc(p);
      p->error->message = message;
    }
  }
  return 0;
}
static int fail_at(Parser * p, GLWASM_SourceLoc where, const char * message) {
  if (p->result == GLWASM_OK) {
    p->result = GLWASM_ERR_CORRUPT;
    if (p->error) {
      p->error->result = p->result;
      p->error->location = where;
      p->error->message = message;
    }
  }
  return 0;
}
static int oom(Parser * p) {
  p->result = GLWASM_ERR_OOM;
  if (p->error) {
    p->error->result = p->result;
    p->error->location = loc(p);
    p->error->message = "allocation failed";
  }
  return 0;
}
static void * alloc(Parser * p, size_t n) {
  void * v = gcu_allocator_malloc(p->a, n ? n : 1);
  if (!v)
    oom(p);
  return v;
}
static GLWASM_Text * node(Parser * p, GLWASM_TextKind k) {
  GLWASM_Text * v = alloc(p, sizeof(*v));
  if (v) {
    memset(v, 0, sizeof(*v));
    v->allocator = *p->a;
    v->kind = k;
    v->loc = loc(p);
  }
  return v;
}
static void free_node(const GLWASM_Allocator * a, GLWASM_Text * v) {
  size_t i;
  if (!v)
    return;
  for (i = 0; i < v->count; i++)
    free_node(a, v->children[i]);
  gcu_allocator_free(a, v->children);
  gcu_allocator_free(a, v->bytes);
  gcu_allocator_free(a, v);
}
static int add(Parser * p, GLWASM_Text * list, GLWASM_Text * v) {
  if (list->count == list->capacity) {
    if (list->capacity > SIZE_MAX / 2 / sizeof(*list->children))
      return oom(p);
    size_t cap = list->capacity ? list->capacity * 2 : 4;
    void * next = gcu_allocator_realloc(p->a, list->children,
                                        cap * sizeof(*list->children));
    if (!next)
      return oom(p);
    list->children = next;
    list->capacity = cap;
  }
  list->children[list->count++] = v;
  return 1;
}
static int ws(uint8_t c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
static int skip(Parser * p) {
  for (;;) {
    while (p->p < p->n && ws(p->s[p->p]))
      advance(p);
    if (p->p + 1 < p->n && p->s[p->p] == ';' && p->s[p->p + 1] == ';') {
      /* A line comment ends at any newline: LF, CR, or CR LF. */
      while (p->p < p->n && p->s[p->p] != '\n' && p->s[p->p] != '\r')
        advance(p);
      continue;
    }
    if (p->p + 1 < p->n && p->s[p->p] == '(' && p->s[p->p + 1] == ';') {
      GLWASM_SourceLoc start = loc(p);
      unsigned depth = 0;
      do {
        if (p->p + 1 < p->n && p->s[p->p] == '(' && p->s[p->p + 1] == ';') {
          advance(p);
          advance(p);
          depth++;
        }
        else if (p->p + 1 < p->n && p->s[p->p] == ';' &&
                 p->s[p->p + 1] == ')') {
          advance(p);
          advance(p);
          depth--;
        }
        else if (p->p < p->n)
          advance(p);
        else {
          return fail_at(p, start, "unterminated nested comment");
        }
      } while (depth);
      continue;
    }
    return 1;
  }
}
static int hex(uint8_t c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}
static int append_byte(Parser * p, uint8_t ** b, size_t * n, size_t * cap,
                       uint8_t x) {
  if (*n == *cap) {
    if (*cap > SIZE_MAX / 2)
      return oom(p);
    size_t c = *cap ? *cap * 2 : 16;
    void * q = gcu_allocator_realloc(p->a, *b, c);
    if (!q)
      return oom(p);
    *b = q;
    *cap = c;
  }
  (*b)[(*n)++] = x;
  return 1;
}
static int string(Parser * p, GLWASM_Text ** out) {
  GLWASM_Text * v = node(p, GLWASM_TEXT_STRING);
  uint8_t * b = NULL;
  size_t n = 0, cap = 0;
  if (!v)
    return 0;
  advance(p);
  while (p->p < p->n && p->s[p->p] != '"') {
    uint8_t c = p->s[p->p];
    advance(p);
    if (c == '\\') {
      int h1, h2;
      if (p->p >= p->n) {
        free_node(p->a, v);
        return fail(p, "incomplete string escape");
      }
      c = p->s[p->p];
      advance(p);
      if (ws(c)) {
        while (p->p < p->n && ws(p->s[p->p]))
          advance(p);
        if (p->p >= p->n || p->s[p->p] != '\\') {
          gcu_allocator_free(p->a, b);
          free_node(p->a, v);
          return fail(p, "invalid string continuation");
        }
        advance(p);
        continue;
      }
      if (c == 'u') {
        uint32_t cp = 0;
        unsigned digits = 0;
        if (p->p >= p->n || p->s[p->p] != '{') {
          gcu_allocator_free(p->a, b);
          free_node(p->a, v);
          return fail(p, "invalid Unicode escape");
        }
        advance(p);
        while (p->p < p->n && p->s[p->p] != '}') {
          int d = hex(p->s[p->p]);
          if (d < 0 || digits == 6 || cp > 0x10ffffu / 16u) {
            gcu_allocator_free(p->a, b);
            free_node(p->a, v);
            return fail(p, "invalid Unicode escape");
          }
          cp = (cp << 4) | (uint32_t)d;
          digits++;
          advance(p);
        }
        if (!digits || p->p >= p->n || p->s[p->p] != '}' || cp > 0x10ffffu ||
            (cp >= 0xd800u && cp <= 0xdfffu)) {
          gcu_allocator_free(p->a, b);
          free_node(p->a, v);
          return fail(p, "invalid Unicode escape");
        }
        advance(p);
        if (cp < 0x80u) {
          if (!append_byte(p, &b, &n, &cap, (uint8_t)cp)) {
            gcu_allocator_free(p->a, b);
            free_node(p->a, v);
            return 0;
          }
        }
        else if (cp < 0x800u) {
          if (!append_byte(p, &b, &n, &cap, (uint8_t)(0xc0u | (cp >> 6))) ||
              !append_byte(p, &b, &n, &cap, (uint8_t)(0x80u | (cp & 0x3fu)))) {
            gcu_allocator_free(p->a, b);
            free_node(p->a, v);
            return 0;
          }
        }
        else if (cp < 0x10000u) {
          if (!append_byte(p, &b, &n, &cap, (uint8_t)(0xe0u | (cp >> 12))) ||
              !append_byte(p, &b, &n, &cap,
                           (uint8_t)(0x80u | ((cp >> 6) & 0x3fu))) ||
              !append_byte(p, &b, &n, &cap, (uint8_t)(0x80u | (cp & 0x3fu)))) {
            gcu_allocator_free(p->a, b);
            free_node(p->a, v);
            return 0;
          }
        }
        else {
          if (!append_byte(p, &b, &n, &cap, (uint8_t)(0xf0u | (cp >> 18))) ||
              !append_byte(p, &b, &n, &cap,
                           (uint8_t)(0x80u | ((cp >> 12) & 0x3fu))) ||
              !append_byte(p, &b, &n, &cap,
                           (uint8_t)(0x80u | ((cp >> 6) & 0x3fu))) ||
              !append_byte(p, &b, &n, &cap, (uint8_t)(0x80u | (cp & 0x3fu)))) {
            gcu_allocator_free(p->a, b);
            free_node(p->a, v);
            return 0;
          }
        }
        continue;
      }
      if (c == '\\' || c == '"') {
      }
      else if (c == 'n')
        c = '\n';
      else if (c == 'r')
        c = '\r';
      else if (c == 't')
        c = '\t';
      else {
        h1 = hex(c);
        if (h1 < 0 || p->p >= p->n || (h2 = hex(p->s[p->p])) < 0) {
          gcu_allocator_free(p->a, b);
          free_node(p->a, v);
          return fail(p, "invalid string escape");
        }
        advance(p);
        c = (uint8_t)((h1 << 4) | h2);
      }
    }
    else if (c < 0x20 || c == 0x7f) {
      gcu_allocator_free(p->a, b);
      free_node(p->a, v);
      return fail(p, "unescaped control byte in string");
    }
    if (!append_byte(p, &b, &n, &cap, c)) {
      gcu_allocator_free(p->a, b);
      free_node(p->a, v);
      return 0;
    }
  }
  if (p->p >= p->n) {
    gcu_allocator_free(p->a, b);
    free_node(p->a, v);
    return fail(p, "unterminated string");
  }
  advance(p);
  v->bytes = b;
  v->length = n;
  *out = v;
  return 1;
}
static int delimiter(uint8_t c) {
  return ws(c) || c == '(' || c == ')' || c == '"';
}
static int expr(Parser * p, GLWASM_Text ** out, unsigned depth) {
  GLWASM_Text * v;
  size_t start;
  /* Keep recursive descent below a depth that is safe on small thread stacks. */
  if (depth > 256)
    return fail(p, "nesting limit exceeded");
  if (!skip(p))
    return 0;
  if (p->p >= p->n)
    return fail(p, "expected expression");
  if (p->s[p->p] == '(') {
    GLWASM_SourceLoc start_loc = loc(p);
    advance(p);
    v = node(p, GLWASM_TEXT_LIST);
    if (!v)
      return 0;
    v->loc = start_loc;
    for (;;) {
      if (!skip(p)) {
        free_node(p->a, v);
        return 0;
      }
      if (p->p >= p->n) {
        free_node(p->a, v);
        return fail(p, "unterminated list");
      }
      if (p->s[p->p] == ')') {
        advance(p);
        *out = v;
        return 1;
      }
      GLWASM_Text * child = NULL;
      if (!expr(p, &child, depth + 1) || !add(p, v, child)) {
        free_node(p->a, child);
        free_node(p->a, v);
        return 0;
      }
    }
  }
  if (p->s[p->p] == ')')
    return fail(p, "unexpected closing delimiter");
  if (p->s[p->p] == '$' && p->p + 1 < p->n && p->s[p->p + 1] == '"') {
    GLWASM_Text * quoted = NULL;
    v = node(p, GLWASM_TEXT_ATOM);
    if (!v)
      return 0;
    advance(p);
    if (!string(p, &quoted)) {
      free_node(p->a, v);
      return 0;
    }
    if (quoted->length == SIZE_MAX) {
      free_node(p->a, quoted);
      free_node(p->a, v);
      return oom(p);
    }
    v->bytes = alloc(p, quoted->length + 1);
    if (!v->bytes) {
      free_node(p->a, quoted);
      free_node(p->a, v);
      return 0;
    }
    v->bytes[0] = '$';
    if (quoted->length)
      memcpy(v->bytes + 1, quoted->bytes, quoted->length);
    v->length = quoted->length + 1;
    free_node(p->a, quoted);
    *out = v;
    return 1;
  }
  if (p->s[p->p] == '"')
    return string(p, out);
  v = node(p, GLWASM_TEXT_ATOM);
  if (!v)
    return 0;
  start = p->p;
  while (p->p < p->n && !delimiter(p->s[p->p])) {
    if (p->s[p->p] < 0x20 || p->s[p->p] == 0x7f) {
      free_node(p->a, v);
      return fail(p, "control byte in atom");
    }
    if (p->s[p->p] == ';' && p->p + 1 < p->n && p->s[p->p + 1] == ';')
      break;
    advance(p);
  }
  if (start == p->p) {
    free_node(p->a, v);
    return fail(p, "invalid token");
  }
  v->length = p->p - start;
  v->bytes = alloc(p, v->length);
  if (!v->bytes) {
    free_node(p->a, v);
    return 0;
  }
  memcpy(v->bytes, p->s + start, v->length);
  *out = v;
  return 1;
}
static int valid_utf8(const uint8_t * s, size_t n, size_t * bad) {
  for (size_t i = 0; i < n;) {
    uint8_t c = s[i];
    size_t width = 1;
    uint32_t value;
    if (c < 0x80) {
      i++;
      continue;
    }
    if (c >= 0xc2 && c <= 0xdf) { width = 2; value = c & 0x1fu; }
    else if (c >= 0xe0 && c <= 0xef) { width = 3; value = c & 0x0fu; }
    else if (c >= 0xf0 && c <= 0xf4) { width = 4; value = c & 0x07u; }
    else { *bad = i; return 0; }
    if (width > n - i) { *bad = i; return 0; }
    for (size_t j = 1; j < width; ++j) {
      if ((s[i + j] & 0xc0u) != 0x80u) { *bad = i + j; return 0; }
      value = (value << 6) | (s[i + j] & 0x3fu);
    }
    if ((width == 3 && value < 0x800u) ||
        (width == 4 && value < 0x10000u) ||
        (value >= 0xd800u && value <= 0xdfffu) || value > 0x10ffffu) {
      *bad = i;
      return 0;
    }
    i += width;
  }
  return 1;
}
GLWASM_Result glwasm_text_parse(const uint8_t * s, size_t n,
                                const GLWASM_Allocator * a,
                                GLWASM_ParseError * e, GLWASM_Text ** out) {
  Parser p;
  GLWASM_Text * root;
  size_t before;
  if (!out || (!s && n))
    return GLWASM_ERR_INVALID;
  if (e) {
    e->result = GLWASM_OK;
    e->location = (GLWASM_SourceLoc){0, 1, 1};
    e->message = NULL;
  }
  if (n) {
    size_t bad = 0;
    if (!valid_utf8(s, n, &bad)) {
      if (e) {
        uint32_t line = 1, column = 1;
        for (size_t i = 0; i < bad; ++i) {
          if (s[i] == '\n') { line++; column = 1; }
          else column++;
        }
        e->result = GLWASM_ERR_CORRUPT;
        e->location = (GLWASM_SourceLoc){bad, line, column};
        e->message = "malformed UTF-8 in text source";
      }
      return GLWASM_ERR_CORRUPT;
    }
  }
  p = (Parser){s, n, 0, 1, 1, a ? a : glwasm_allocator_default(), e, GLWASM_OK};
  root = node(&p, GLWASM_TEXT_LIST);
  if (!root)
    return p.result;
  while (1) {
    if (!skip(&p)) {
      free_node(p.a, root);
      return p.result;
    }
    if (p.p == p.n)
      break;
    before = p.p;
    GLWASM_Text * v = NULL;
    if (!expr(&p, &v, 0) || !add(&p, root, v)) {
      free_node(p.a, v);
      free_node(p.a, root);
      return p.result ? p.result : GLWASM_ERR_CORRUPT;
    }
    if (p.p <= before) {
      free_node(p.a, root);
      return GLWASM_ERR_INTERNAL;
    }
  }
  *out = root;
  return GLWASM_OK;
}
void glwasm_text_destroy(GLWASM_Text * v) {
  if (v)
    free_node(&v->allocator, v);
}
GLWASM_TextKind glwasm_text_kind(const GLWASM_Text * v) {
  return v ? v->kind : GLWASM_TEXT_LIST;
}
GLWASM_SourceLoc glwasm_text_location(const GLWASM_Text * v) {
  GLWASM_SourceLoc l = {0, 0, 0};
  return v ? v->loc : l;
}
size_t glwasm_text_size(const GLWASM_Text * v) {
  return v ? v->count : 0;
}
const GLWASM_Text * glwasm_text_at(const GLWASM_Text * v, size_t i) {
  return v && i < v->count ? v->children[i] : NULL;
}
const uint8_t * glwasm_text_bytes(const GLWASM_Text * v, size_t * n) {
  if (n)
    *n = v ? v->length : 0;
  return v ? v->bytes : NULL;
}
