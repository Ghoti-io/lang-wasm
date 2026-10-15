/*
 * SPDX-License-Identifier: LGPL-3.0-only
 * Copyright (C) 2026 Corey Pennycuff
 */
#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/lang-wasm/macros.h>
#include <ghoti.io/lang-wasm/script.h>

#include <string.h>

struct GLWASM_Script {
  GLWASM_Allocator allocator;
  GLWASM_Text * syntax;
};

static int atom_eq(const GLWASM_Text * node, const char * word) {
  size_t n = 0;
  const uint8_t * bytes = glwasm_text_bytes(node, &n);
  return node && glwasm_text_kind(node) == GLWASM_TEXT_ATOM &&
         n == strlen(word) && memcmp(bytes, word, n) == 0;
}

static int annotation(const GLWASM_Text * node) {
  size_t n = 0;
  const uint8_t * bytes;
  const GLWASM_Text * first;
  if (!node || glwasm_text_kind(node) != GLWASM_TEXT_LIST ||
      glwasm_text_size(node) == 0)
    return 0;
  first = glwasm_text_at(node, 0);
  if (glwasm_text_kind(first) != GLWASM_TEXT_ATOM)
    return 0;
  bytes = glwasm_text_bytes(first, &n);
  return n != 0 && bytes[0] == '@';
}

static const GLWASM_Text * semantic_child(const GLWASM_Text * node,
                                          size_t wanted) {
  size_t i, seen = 0;
  if (!node || glwasm_text_kind(node) != GLWASM_TEXT_LIST)
    return NULL;
  for (i = 0; i < glwasm_text_size(node); ++i) {
    const GLWASM_Text * child = glwasm_text_at(node, i);
    if (annotation(child))
      continue;
    if (seen++ == wanted)
      return child;
  }
  return NULL;
}

static const GLWASM_Text * head(const GLWASM_Text * node) {
  return semantic_child(node, 0);
}

static size_t semantic_size(const GLWASM_Text * node) {
  size_t i, count = 0;
  for (i = 0; node && i < glwasm_text_size(node); ++i)
    if (!annotation(glwasm_text_at(node, i)))
      ++count;
  return count;
}

static int is_string(const GLWASM_Text * node) {
  return node && glwasm_text_kind(node) == GLWASM_TEXT_STRING;
}

static int is_list(const GLWASM_Text * node) {
  return node && glwasm_text_kind(node) == GLWASM_TEXT_LIST;
}

static int is_identifier(const GLWASM_Text * node) {
  size_t n = 0;
  const uint8_t * bytes = glwasm_text_bytes(node, &n);
  return node && glwasm_text_kind(node) == GLWASM_TEXT_ATOM && n > 1 &&
         bytes[0] == '$';
}
static int numeric_literal(const GLWASM_Text * node);
static int heap_type(const GLWASM_Text * node);
static int number_digits(const uint8_t * bytes, size_t length, size_t * offset,
                        unsigned base) {
  size_t i = *offset;
  int seen = 0;
  while (i < length) {
    uint8_t c = bytes[i];
    int valid = (c >= '0' && c <= '9' && (unsigned)(c - '0') < base) ||
                (base == 16 && ((c >= 'a' && c <= 'f') ||
                                (c >= 'A' && c <= 'F')));
    if (valid) {
      seen = 1;
      ++i;
    }
    else if (c == '_' && seen && i + 1 < length) {
      uint8_t next = bytes[i + 1];
      int next_valid = (next >= '0' && next <= '9' &&
                        (unsigned)(next - '0') < base) ||
                       (base == 16 && ((next >= 'a' && next <= 'f') ||
                                       (next >= 'A' && next <= 'F')));
      if (!next_valid)
        break;
      ++i;
      seen = 0;
    }
    else
      break;
  }
  *offset = i;
  return seen;
}
static int integer_syntax(const GLWASM_Text * node, int allow_sign) {
  size_t n = 0, i = 0;
  const uint8_t * bytes = glwasm_text_bytes(node, &n);
  if (!node || glwasm_text_kind(node) != GLWASM_TEXT_ATOM || n == 0)
    return 0;
  if (allow_sign && (bytes[i] == '+' || bytes[i] == '-'))
    ++i;
  unsigned base = 10;
  if (i + 2 < n && bytes[i] == '0' &&
      (bytes[i + 1] == 'x' || bytes[i + 1] == 'X')) {
    base = 16;
    i += 2;
  }
  return number_digits(bytes, n, &i, base) && i == n;
}
static int float_syntax(const GLWASM_Text * node) {
  size_t n = 0, i = 0;
  const uint8_t * bytes = glwasm_text_bytes(node, &n);
  if (!node || glwasm_text_kind(node) != GLWASM_TEXT_ATOM || n == 0)
    return 0;
  if (bytes[i] == '+' || bytes[i] == '-')
    ++i;
  size_t remaining = n - i;
  const uint8_t * token = bytes + i;
  if ((remaining == 3 && memcmp(token, "inf", 3) == 0) ||
      (remaining == 3 && memcmp(token, "nan", 3) == 0) ||
      (remaining == 13 && memcmp(token, "nan:canonical", 13) == 0) ||
      (remaining == 14 && memcmp(token, "nan:arithmetic", 14) == 0))
    return 1;
  if (remaining > 6 && memcmp(token, "nan:0x", 6) == 0) {
    i += 6;
    return number_digits(bytes, n, &i, 16) && i == n;
  }
  unsigned base = 10;
  if (i + 2 < n && bytes[i] == '0' &&
      (bytes[i + 1] == 'x' || bytes[i + 1] == 'X')) {
    base = 16;
    i += 2;
  }
  int before = number_digits(bytes, n, &i, base);
  int after = 0;
  if (i < n && bytes[i] == '.') {
    ++i;
    after = number_digits(bytes, n, &i, base);
  }
  if (!before && !after)
    return 0;
  if (base == 16 && i < n && (bytes[i] == 'p' || bytes[i] == 'P')) {
    ++i;
    if (i < n && (bytes[i] == '+' || bytes[i] == '-'))
      ++i;
    if (!number_digits(bytes, n, &i, 10))
      return 0;
  }
  else if (base == 10 && i < n && (bytes[i] == 'e' || bytes[i] == 'E')) {
    ++i;
    if (i < n && (bytes[i] == '+' || bytes[i] == '-'))
      ++i;
    if (!number_digits(bytes, n, &i, 10))
      return 0;
  }
  return i == n;
}
static int value_form(const GLWASM_Text * node) {
  const GLWASM_Text * h = head(node);
  const GLWASM_Text * operand = semantic_child(node, 1);
  if (is_list(node)) {
    if (atom_eq(h, "either")) {
      if (semantic_size(node) < 2)
        return 0;
      for (size_t i = 1; i < semantic_size(node); ++i)
        if (!value_form(semantic_child(node, i)))
          return 0;
      return 1;
    }
    size_t n = semantic_size(node);
    if (atom_eq(h, "ref.null"))
      return n == 1 || (n == 2 && heap_type(operand));
    if (atom_eq(h, "ref.func"))
      return n == 1 || (n == 2 &&
                        (is_identifier(operand) || numeric_literal(operand)));
    if (atom_eq(h, "ref.extern"))
      return n == 1 || (n == 2 && integer_syntax(operand, 0));
    if (atom_eq(h, "ref.host"))
      return n == 2 && integer_syntax(operand, 0);
    if (atom_eq(h, "ref.array") || atom_eq(h, "ref.struct") ||
        atom_eq(h, "ref.i31") || atom_eq(h, "ref.eq"))
      return n == 1;
    if (atom_eq(h, "ref.any") || atom_eq(h, "ref.exn") ||
        atom_eq(h, "ref.cont"))
      return n == 1 || (n == 2 && heap_type(operand));
  }
  if (atom_eq(h, "v128.const")) {
    size_t n = semantic_size(node), lanes = 0;
    const GLWASM_Text * format = operand;
    int floating = 0;
    if (atom_eq(format, "i8x16")) lanes = 16;
    else if (atom_eq(format, "i16x8")) lanes = 8;
    else if (atom_eq(format, "i32x4")) lanes = 4;
    else if (atom_eq(format, "i64x2")) lanes = 2;
    else if (atom_eq(format, "f32x4")) { lanes = 4; floating = 1; }
    else if (atom_eq(format, "f64x2")) { lanes = 2; floating = 1; }
    if (n != lanes + 2)
      return 0;
    for (size_t i = 2; i < n; ++i)
      if (!(floating ? float_syntax(semantic_child(node, i))
                     : integer_syntax(semantic_child(node, i), 1)))
        return 0;
    return 1;
  }
  if (semantic_size(node) != 2 || !operand)
    return 0;
  if (atom_eq(h, "i32.const") || atom_eq(h, "i64.const"))
    return integer_syntax(operand, 1);
  if (atom_eq(h, "f32.const") || atom_eq(h, "f64.const"))
    return float_syntax(operand);
  return 0;
}

static int module_field(const GLWASM_Text * h);
static int numeric_literal(const GLWASM_Text * node);
static int ref_type(const GLWASM_Text * node);
static int descriptor_form(const GLWASM_Text * node) {
  const GLWASM_Text * h = head(node);
  const GLWASM_Text * index = semantic_child(node, 1);
  return is_list(node) && semantic_size(node) == 2 &&
         (atom_eq(h, "func") || atom_eq(h, "table") ||
          atom_eq(h, "memory") || atom_eq(h, "global") ||
          atom_eq(h, "tag")) &&
         (is_identifier(index) || numeric_literal(index));
}
static int numeric_literal(const GLWASM_Text * node) {
  return integer_syntax(node, 0);
}
static int heap_type(const GLWASM_Text * node) {
  static const char * names[] = {
      "func", "extern", "any", "eq", "i31", "struct", "array", "none",
      "nofunc", "noextern", "exn", "noexn"};
  if (is_identifier(node) || numeric_literal(node))
    return 1;
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
    if (atom_eq(node, names[i]))
      return 1;
  return 0;
}
static int ref_type(const GLWASM_Text * node) {
  size_t n = semantic_size(node);
  const GLWASM_Text * first = semantic_child(node, 1);
  if (!is_list(node) || !atom_eq(head(node), "ref") || n < 2 || n > 3)
    return 0;
  if (n == 3) {
    if (!atom_eq(first, "null"))
      return 0;
    first = semantic_child(node, 2);
  }
  return heap_type(first);
}
static int value_type(const GLWASM_Text * node) {
  if (atom_eq(node, "i32") || atom_eq(node, "i64") ||
      atom_eq(node, "f32") || atom_eq(node, "f64") ||
      atom_eq(node, "v128") || atom_eq(node, "funcref") ||
      atom_eq(node, "externref"))
    return 1;
  return ref_type(node);
}
static int parameter_or_result(const GLWASM_Text * node, int allow_names) {
  size_t n = semantic_size(node), i = 1;
  if (!is_list(node))
    return 0;
  if (allow_names && i < n && is_identifier(semantic_child(node, i)))
    ++i;
  if (i == n)
    return 0;
  for (; i < n; ++i)
    if (!value_type(semantic_child(node, i)))
      return 0;
  return 1;
}
static int function_import_descriptor(const GLWASM_Text * node) {
  int saw_id = 0;
  for (size_t i = 1; i < semantic_size(node); ++i) {
    const GLWASM_Text * child = semantic_child(node, i);
    const GLWASM_Text * h = head(child);
    if (is_identifier(child) && !saw_id && i == 1) {
      saw_id = 1;
      continue;
    }
    if (!is_list(child))
      return 0;
    if (atom_eq(h, "type")) {
      const GLWASM_Text * index = semantic_child(child, 1);
      if (semantic_size(child) != 2 ||
          !(is_identifier(index) || numeric_literal(index)))
        return 0;
    }
    else if (atom_eq(h, "param") && parameter_or_result(child, 1)) {
    }
    else if (atom_eq(h, "result") && parameter_or_result(child, 0)) {
    }
    else
      return 0;
  }
  return 1;
}
static int memory_import_descriptor(const GLWASM_Text * node) {
  size_t i = 1, n = semantic_size(node);
  int has_max = 0, shared = 0;
  if (i < n && is_identifier(semantic_child(node, i)))
    ++i;
  if (i < n && (atom_eq(semantic_child(node, i), "i32") ||
                atom_eq(semantic_child(node, i), "i64")))
    ++i;
  if (i >= n || !numeric_literal(semantic_child(node, i++)))
    return 0;
  if (i < n && numeric_literal(semantic_child(node, i))) {
    ++i;
    has_max = 1;
  }
  if (i < n && atom_eq(semantic_child(node, i), "shared")) {
    ++i;
    shared = 1;
  }
  return i == n && (!shared || has_max);
}
static int table_import_descriptor(const GLWASM_Text * node) {
  size_t i = 1, n = semantic_size(node);
  const GLWASM_Text * element;
  if (i < n && is_identifier(semantic_child(node, i)))
    ++i;
  if (i < n && atom_eq(semantic_child(node, i), "i64"))
    ++i;
  if (i >= n || !numeric_literal(semantic_child(node, i++)))
    return 0;
  if (i < n && numeric_literal(semantic_child(node, i)))
    ++i;
  if (i >= n)
    return 0;
  element = semantic_child(node, i++);
  if (!(atom_eq(element, "funcref") || atom_eq(element, "externref") ||
        ref_type(element)))
    return 0;
  return i == n;
}
static int global_import_descriptor(const GLWASM_Text * node) {
  size_t n = semantic_size(node);
  size_t i = 1;
  if (i < n && is_identifier(semantic_child(node, i)))
    ++i;
  if (i + 1 != n)
    return 0;
  const GLWASM_Text * type = semantic_child(node, i);
  if (value_type(type))
    return 1;
  return is_list(type) && atom_eq(head(type), "mut") &&
         semantic_size(type) == 2 && value_type(semantic_child(type, 1));
}
static int tag_import_descriptor(const GLWASM_Text * node) {
  size_t n = semantic_size(node);
  for (size_t i = 1; i < n; ++i) {
    const GLWASM_Text * child = semantic_child(node, i);
    const GLWASM_Text * h = head(child);
    if (is_identifier(child) && i == 1)
      continue;
    if (atom_eq(h, "param") && parameter_or_result(child, 1))
      continue;
    if (atom_eq(h, "type")) {
      const GLWASM_Text * index = semantic_child(child, 1);
      if (semantic_size(child) == 2 &&
          (is_identifier(index) || numeric_literal(index)))
        continue;
    }
    return 0;
  }
  return n >= 2;
}
static int import_descriptor(const GLWASM_Text * node) {
  const GLWASM_Text * h = head(node);
  if (!is_list(node))
    return 0;
  if (atom_eq(h, "func"))
    return function_import_descriptor(node);
  if (atom_eq(h, "table"))
    return table_import_descriptor(node);
  if (atom_eq(h, "memory"))
    return memory_import_descriptor(node);
  if (atom_eq(h, "global"))
    return global_import_descriptor(node);
  if (atom_eq(h, "tag"))
    return tag_import_descriptor(node);
  return 0;
}
static int type_field_form(const GLWASM_Text * node) {
  size_t i = 1, n = semantic_size(node);
  if (i < n && is_identifier(semantic_child(node, i)))
    ++i;
  return i + 1 == n &&
         atom_eq(head(semantic_child(node, i)), "func") &&
         function_import_descriptor(semantic_child(node, i));
}
static int function_field_form(const GLWASM_Text * node) {
  size_t i = 1, n = semantic_size(node);
  int saw_import = 0, saw_body = 0;
  if (i < n && is_identifier(semantic_child(node, i)))
    ++i;
  for (; i < n; ++i) {
    const GLWASM_Text * child = semantic_child(node, i);
    const GLWASM_Text * h = head(child);
    if (!is_list(child))
      return 0;
    if (atom_eq(h, "import")) {
      if (saw_import || saw_body || semantic_size(child) != 3 ||
          !is_string(semantic_child(child, 1)) ||
          !is_string(semantic_child(child, 2)))
        return 0;
      saw_import = 1;
      continue;
    }
    if (atom_eq(h, "export")) {
      if (saw_body || semantic_size(child) != 2 ||
          !is_string(semantic_child(child, 1)))
        return 0;
      continue;
    }
    if (atom_eq(h, "type")) {
      const GLWASM_Text * index = semantic_child(child, 1);
      if (saw_body || semantic_size(child) != 2 ||
          !(is_identifier(index) || numeric_literal(index)))
        return 0;
      continue;
    }
    if (atom_eq(h, "param") || atom_eq(h, "result")) {
      if (saw_body || !parameter_or_result(child, atom_eq(h, "param")))
        return 0;
      continue;
    }
    if (atom_eq(h, "local")) {
      if (saw_body || !parameter_or_result(child, 1))
        return 0;
      continue;
    }
    /* The WAT module parser owns instruction grammar. */
    if (saw_import)
      return 0;
    saw_body = 1;
  }
  return 1;
}
static int constant_expression(const GLWASM_Text * node) {
  const GLWASM_Text * op = head(node);
  const GLWASM_Text * operand = semantic_child(node, 1);
  if (value_form(node))
    return 1;
  return is_list(node) && semantic_size(node) == 2 && operand &&
         (atom_eq(op, "global.get") || atom_eq(op, "ref.func")) &&
         (is_identifier(operand) || numeric_literal(operand));
}
static int global_field_form(const GLWASM_Text * node) {
  size_t i = 1, n = semantic_size(node);
  const GLWASM_Text * type;
  const GLWASM_Text * initializer;
  if (i < n && is_identifier(semantic_child(node, i)))
    ++i;
  if (i + 1 >= n)
    return 0;
  type = semantic_child(node, i++);
  if (!value_type(type) &&
      !(is_list(type) && atom_eq(head(type), "mut") &&
        semantic_size(type) == 2 && value_type(semantic_child(type, 1))))
    return 0;
  initializer = semantic_child(node, i);
  return constant_expression(initializer);
}
static int memory_field_form(const GLWASM_Text * node) {
  size_t i = 1, n = semantic_size(node);
  const GLWASM_Text * item;
  size_t numeric_count = 0;
  int shared = 0;
  if (i < n && is_identifier(semantic_child(node, i)))
    ++i;
  if (i < n && (atom_eq(semantic_child(node, i), "i32") ||
                atom_eq(semantic_child(node, i), "i64")))
    ++i;
  if (i >= n)
    return 0;
  item = semantic_child(node, i++);
  if (numeric_literal(item))
    numeric_count = 1;
  else if (
      !(is_list(item) && atom_eq(head(item), "data")))
    return 0;
  for (; i < n; ++i) {
    item = semantic_child(node, i);
    if (numeric_literal(item) && numeric_count == 1) {
      if (shared)
        return 0;
      ++numeric_count;
      continue;
    }
    if (atom_eq(item, "shared")) {
      if (shared)
        return 0;
      shared = 1;
      continue;
    }
    if (is_list(item) &&
        (atom_eq(head(item), "data") || atom_eq(head(item), "import") ||
         atom_eq(head(item), "export")))
      continue;
    return 0;
  }
  return !shared || numeric_count == 2;
}
static int table_field_form(const GLWASM_Text * node) {
  size_t i = 1, n = semantic_size(node);
  const GLWASM_Text * item;
  if (i < n && is_identifier(semantic_child(node, i)))
    ++i;
  if (i < n && atom_eq(semantic_child(node, i), "i64"))
    ++i;
  if (i >= n || !numeric_literal(semantic_child(node, i++)))
    return 0;
  if (i < n && numeric_literal(semantic_child(node, i)))
    ++i;
  if (i >= n)
    return 0;
  item = semantic_child(node, i++);
  if (!(atom_eq(item, "funcref") || atom_eq(item, "externref") ||
        ref_type(item)))
    return 0;
  for (; i < n; ++i) {
    item = semantic_child(node, i);
    if (is_list(item) &&
        (atom_eq(head(item), "elem") || atom_eq(head(item), "import") ||
         atom_eq(head(item), "export")))
      continue;
    return 0;
  }
  return 1;
}
static int element_item_expression(const GLWASM_Text * node) {
  const GLWASM_Text * h = head(node);
  const GLWASM_Text * operand = semantic_child(node, 1);
  if (atom_eq(h, "ref.func"))
    return semantic_size(node) == 2 &&
           (is_identifier(operand) || numeric_literal(operand));
  if (atom_eq(h, "ref.null"))
    return semantic_size(node) == 1 ||
           (semantic_size(node) == 2 && heap_type(operand));
  return constant_expression(node);
}
static int segment_field_form(const GLWASM_Text * node, int data) {
  size_t i = 1, n = semantic_size(node);
  int payload = 0, refs_expected = 0, saw_string = 0, saw_offset = 0;
  int saw_element_kind = 0;
  if (i < n && is_identifier(semantic_child(node, i)))
    ++i;
  for (; i < n; ++i) {
    const GLWASM_Text * item = semantic_child(node, i);
    const GLWASM_Text * h = head(item);
    if (data && is_string(item)) {
      saw_string = 1;
      payload = 1;
      continue;
    }
    if (!data && (atom_eq(item, "declare") || atom_eq(item, "funcref") ||
                  atom_eq(item, "externref")))
      continue;
    if (!data && (atom_eq(item, "func") || atom_eq(item, "extern"))) {
      if (saw_element_kind)
        return 0;
      saw_element_kind = 1;
      refs_expected = 1;
      continue;
    }
    if (!data && refs_expected &&
        (is_identifier(item) || numeric_literal(item))) {
      payload = 1;
      continue;
    }
    if (data && is_list(item) && !saw_string && !saw_offset &&
        constant_expression(item) &&
        (atom_eq(h, "i32.const") || atom_eq(h, "i64.const") ||
         atom_eq(h, "global.get"))) {
      saw_offset = 1;
      payload = 1;
      continue;
    }
    if (!data && is_list(item) &&
        (constant_expression(item) ||
         (atom_eq(h, "item") && semantic_size(item) == 2 &&
          element_item_expression(semantic_child(item, 1))))) {
      payload = 1;
      continue;
    }
    return 0;
  }
  return payload;
}
static int command_kind(const GLWASM_Text * syntax, GLWASM_CommandKind * out);
static int action_form(const GLWASM_Text * node) {
  const GLWASM_Text * h = head(node);
  GLWASM_CommandKind kind;
  return (atom_eq(h, "invoke") || atom_eq(h, "get")) &&
         command_kind(node, &kind);
}
static int module_form(const GLWASM_Text * node) {
  GLWASM_CommandKind kind;
  return atom_eq(head(node), "module") && command_kind(node, &kind);
}

/* Check the command envelope here; field and instruction grammar is owned by
 * the WAT module parser. This prevents a recognized head from making an
 * otherwise malformed WAST command look valid. */
static int command_shape(const GLWASM_Text * syntax, GLWASM_CommandKind kind) {
  size_t n = semantic_size(syntax);
  const GLWASM_Text * first = semantic_child(syntax, 1);
  const GLWASM_Text * second = semantic_child(syntax, 2);
  switch (kind) {
  case GLWASM_COMMAND_MODULE_TEXT:
    if (n == 0)
      return 0;
    for (size_t i = 1; i < n; ++i) {
      const GLWASM_Text * child = semantic_child(syntax, i);
      if (i == 1 && is_identifier(child))
        continue;
      if (!is_list(child) || !module_field(head(child)))
        return 0;
    }
    return 1;
  case GLWASM_COMMAND_MODULE_DEFINITION:
    if (n < 3 || !atom_eq(semantic_child(syntax, 1), "definition"))
      return 0;
    for (size_t i = 2; i < n; ++i) {
      const GLWASM_Text * child = semantic_child(syntax, i);
      if (i == 2 && is_identifier(child))
        continue;
      if (!is_list(child) || !module_field(head(child)))
        return 0;
    }
    return 1;
  case GLWASM_COMMAND_MODULE_INSTANCE:
    return n == 4 && atom_eq(semantic_child(syntax, 1), "instance") &&
           is_identifier(semantic_child(syntax, 2)) &&
           is_identifier(semantic_child(syntax, 3));
  case GLWASM_COMMAND_MODULE_BINARY:
    if (!atom_eq(semantic_child(syntax, 1), "binary") &&
        !atom_eq(semantic_child(syntax, 2), "binary"))
      return 0;
    if (n < 3)
      return 0;
    for (size_t i = atom_eq(semantic_child(syntax, 1), "binary") ? 2 : 3;
         i < n; ++i)
      if (!is_string(semantic_child(syntax, i)))
        return 0;
    return 1;
  case GLWASM_COMMAND_MODULE_QUOTE:
    if (n < 3 || (!atom_eq(semantic_child(syntax, 1), "quote") &&
                  !atom_eq(semantic_child(syntax, 2), "quote")))
      return 0;
    for (size_t i = atom_eq(semantic_child(syntax, 1), "quote") ? 2 : 3;
         i < n; ++i)
      if (!is_string(semantic_child(syntax, i)))
        return 0;
    return 1;
  case GLWASM_COMMAND_REGISTER:
    return n >= 2 && n <= 3 && is_string(first) &&
           (n == 2 || is_identifier(second));
  case GLWASM_COMMAND_INVOKE:
    if (!((n >= 2 && is_string(first)) ||
          (n >= 3 && is_identifier(first) && is_string(second))))
      return 0;
    for (size_t i = is_string(first) ? 2 : 3; i < n; ++i)
      if (!value_form(semantic_child(syntax, i)))
        return 0;
    return 1;
  case GLWASM_COMMAND_GET:
    return (n == 2 && is_string(first)) ||
           (n == 3 && is_identifier(first) && is_string(second));
  case GLWASM_COMMAND_ASSERT_RETURN:
    if (n < 2 || !action_form(first))
      return 0;
    for (size_t i = 2; i < n; ++i)
      if (!value_form(semantic_child(syntax, i)))
        return 0;
    return 1;
  case GLWASM_COMMAND_ASSERT_RETURN_CANONICAL_NAN:
  case GLWASM_COMMAND_ASSERT_RETURN_ARITHMETIC_NAN:
    return n == 2 && action_form(first);
  case GLWASM_COMMAND_ASSERT_TRAP:
  case GLWASM_COMMAND_ASSERT_EXHAUSTION:
  case GLWASM_COMMAND_ASSERT_MALFORMED:
  case GLWASM_COMMAND_ASSERT_INVALID:
  case GLWASM_COMMAND_ASSERT_UNLINKABLE:
  case GLWASM_COMMAND_ASSERT_UNINSTANTIABLE:
  case GLWASM_COMMAND_ASSERT_MALFORMED_CUSTOM:
  case GLWASM_COMMAND_ASSERT_INVALID_CUSTOM:
    if (n != 3 || !is_string(second))
      return 0;
    if (kind == GLWASM_COMMAND_ASSERT_MALFORMED ||
        kind == GLWASM_COMMAND_ASSERT_INVALID ||
        kind == GLWASM_COMMAND_ASSERT_UNLINKABLE ||
        kind == GLWASM_COMMAND_ASSERT_UNINSTANTIABLE ||
        kind == GLWASM_COMMAND_ASSERT_MALFORMED_CUSTOM ||
        kind == GLWASM_COMMAND_ASSERT_INVALID_CUSTOM)
      return module_form(first);
    return action_form(first) ||
           (kind == GLWASM_COMMAND_ASSERT_TRAP && module_form(first));
  case GLWASM_COMMAND_ASSERT_EXCEPTION:
  case GLWASM_COMMAND_ASSERT_SUSPENSION:
    return n == 2 && action_form(first);
  case GLWASM_COMMAND_MODULE_FIELD:
    if (atom_eq(head(syntax), "func"))
      return function_field_form(syntax);
    if (atom_eq(head(syntax), "export"))
      return n == 3 && is_string(semantic_child(syntax, 1)) &&
             descriptor_form(semantic_child(syntax, 2));
    if (atom_eq(head(syntax), "import"))
      return n == 4 && is_string(semantic_child(syntax, 1)) &&
             is_string(semantic_child(syntax, 2)) &&
             import_descriptor(semantic_child(syntax, 3));
    if (atom_eq(head(syntax), "start"))
      return n == 2 && (is_identifier(semantic_child(syntax, 1)) ||
                        numeric_literal(semantic_child(syntax, 1)));
    if (atom_eq(head(syntax), "type"))
      return type_field_form(syntax);
    if (atom_eq(head(syntax), "memory"))
      return memory_field_form(syntax);
    if (atom_eq(head(syntax), "table"))
      return table_field_form(syntax);
    if (atom_eq(head(syntax), "global"))
      return global_field_form(syntax);
    if (atom_eq(head(syntax), "tag"))
      return tag_import_descriptor(syntax);
    if (atom_eq(head(syntax), "data"))
      return segment_field_form(syntax, 1);
    if (atom_eq(head(syntax), "elem"))
      return segment_field_form(syntax, 0);
    return module_field(head(syntax)) && n >= 2;
  }
  return 0;
}

static int module_field(const GLWASM_Text * h) {
  return atom_eq(h, "type") || atom_eq(h, "rec") || atom_eq(h, "sub") ||
         atom_eq(h, "func") || atom_eq(h, "table") || atom_eq(h, "memory") ||
         atom_eq(h, "global") || atom_eq(h, "tag") || atom_eq(h, "import") ||
         atom_eq(h, "export") || atom_eq(h, "start") || atom_eq(h, "elem") ||
         atom_eq(h, "data");
}

static int command_kind(const GLWASM_Text * syntax, GLWASM_CommandKind * out) {
  const GLWASM_Text * h = head(syntax);
  if (atom_eq(h, "module")) {
    const GLWASM_Text * form = semantic_child(syntax, 1);
    const GLWASM_Text * named_form = semantic_child(syntax, 2);
    *out = atom_eq(form, "binary") || atom_eq(named_form, "binary")
               ? GLWASM_COMMAND_MODULE_BINARY
           : atom_eq(form, "quote") || atom_eq(named_form, "quote")
               ? GLWASM_COMMAND_MODULE_QUOTE
           : atom_eq(form, "definition")
               ? GLWASM_COMMAND_MODULE_DEFINITION
           : atom_eq(form, "instance")
               ? GLWASM_COMMAND_MODULE_INSTANCE
               : GLWASM_COMMAND_MODULE_TEXT;
    return command_shape(syntax, *out);
  }
  if (atom_eq(h, "register"))
    *out = GLWASM_COMMAND_REGISTER;
  else if (atom_eq(h, "invoke"))
    *out = GLWASM_COMMAND_INVOKE;
  else if (atom_eq(h, "get"))
    *out = GLWASM_COMMAND_GET;
  else if (atom_eq(h, "assert_return"))
    *out = GLWASM_COMMAND_ASSERT_RETURN;
  else if (atom_eq(h, "assert_return_canonical_nan"))
    *out = GLWASM_COMMAND_ASSERT_RETURN_CANONICAL_NAN;
  else if (atom_eq(h, "assert_return_arithmetic_nan"))
    *out = GLWASM_COMMAND_ASSERT_RETURN_ARITHMETIC_NAN;
  else if (atom_eq(h, "assert_trap"))
    *out = GLWASM_COMMAND_ASSERT_TRAP;
  else if (atom_eq(h, "assert_exhaustion"))
    *out = GLWASM_COMMAND_ASSERT_EXHAUSTION;
  else if (atom_eq(h, "assert_exception"))
    *out = GLWASM_COMMAND_ASSERT_EXCEPTION;
  else if (atom_eq(h, "assert_malformed"))
    *out = GLWASM_COMMAND_ASSERT_MALFORMED;
  else if (atom_eq(h, "assert_invalid"))
    *out = GLWASM_COMMAND_ASSERT_INVALID;
  else if (atom_eq(h, "assert_unlinkable"))
    *out = GLWASM_COMMAND_ASSERT_UNLINKABLE;
  else if (atom_eq(h, "assert_uninstantiable"))
    *out = GLWASM_COMMAND_ASSERT_UNINSTANTIABLE;
  else if (atom_eq(h, "assert_suspension"))
    *out = GLWASM_COMMAND_ASSERT_SUSPENSION;
  else if (atom_eq(h, "assert_malformed_custom"))
    *out = GLWASM_COMMAND_ASSERT_MALFORMED_CUSTOM;
  else if (atom_eq(h, "assert_invalid_custom"))
    *out = GLWASM_COMMAND_ASSERT_INVALID_CUSTOM;
  else if (module_field(h))
    *out = GLWASM_COMMAND_MODULE_FIELD;
  else
    return 0;
  return command_shape(syntax, *out);
}

GLWASM_Result glwasm_script_parse(const uint8_t * source, size_t length,
                                  const GLWASM_Allocator * allocator,
                                  GLWASM_ParseError * error,
                                  GLWASM_Script ** out_script) {
  const GLWASM_Allocator * a =
      allocator ? allocator : glwasm_allocator_default();
  GLWASM_Text * syntax = NULL;
  GLWASM_Script * script;
  GLWASM_Result result;
  size_t i;
  if (!out_script || (!source && length))
    return GLWASM_ERR_INVALID;
  result = glwasm_text_parse(source, length, a, error, &syntax);
  if (result != GLWASM_OK)
    return result;
  for (i = 0; i < glwasm_text_size(syntax); ++i) {
    GLWASM_CommandKind kind;
    if (!command_kind(glwasm_text_at(syntax, i), &kind)) {
      if (error) {
        error->result = GLWASM_ERR_CORRUPT;
        error->location = glwasm_text_location(glwasm_text_at(syntax, i));
        error->message = "unknown or malformed WAST command";
      }
      glwasm_text_destroy(syntax);
      return GLWASM_ERR_CORRUPT;
    }
  }
  script = gcu_allocator_malloc(a, sizeof(*script));
  if (!script) {
    glwasm_text_destroy(syntax);
    if (error) {
      error->result = GLWASM_ERR_OOM;
      error->message = "allocation failed";
    }
    return GLWASM_ERR_OOM;
  }
  script->allocator = *a;
  script->syntax = syntax;
  *out_script = script;
  return GLWASM_OK;
}

void glwasm_script_destroy(GLWASM_Script * script) {
  if (!script)
    return;
  glwasm_text_destroy(script->syntax);
  gcu_allocator_free(&script->allocator, script);
}

size_t glwasm_script_size(const GLWASM_Script * script) {
  return script ? glwasm_text_size(script->syntax) : 0;
}

GLWASM_Result glwasm_script_at(const GLWASM_Script * script, size_t index,
                               GLWASM_Command * out_command) {
  const GLWASM_Text * syntax;
  GLWASM_CommandKind kind;
  if (!script || !out_command || index >= glwasm_script_size(script))
    return GLWASM_ERR_INVALID;
  syntax = glwasm_text_at(script->syntax, index);
  if (!command_kind(syntax, &kind))
    return GLWASM_ERR_INTERNAL;
  out_command->kind = kind;
  out_command->location = glwasm_text_location(syntax);
  out_command->syntax = syntax;
  return GLWASM_OK;
}
