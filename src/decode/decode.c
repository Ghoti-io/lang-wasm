/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Lang-wasm.
 *
 * Ghoti.io Lang-wasm is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License version
 * 3 as published by the Free Software Foundation.
 *
 * Ghoti.io Lang-wasm is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * One pass over a Wasm 3.0 binary. A deferred feature is recorded and, if
 * the rest of the module is well-formed, reported as unsupported rather
 * than corrupt. Caps are checked before the allocation they bound.
 */

#include <ghoti.io/lang-wasm/macros.h>

#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/lang-wasm/decode.h>

#include "../module_internal.h"

#include <stdio.h>
#include <string.h>

#define GLWASM_PAGE 65536ull

enum {
  GLWASM_KIND_FUNC = 1,
  GLWASM_KIND_BLOCK,
  GLWASM_KIND_LOOP,
  GLWASM_KIND_IF,
  GLWASM_KIND_TRY
};

typedef struct Reader {
  const uint8_t * p;
  const uint8_t * end;
  GLWASM_Result err;
  const char * limit;
  int deferred;
  int skip_rest;
  const GLWASM_Limits * limits;
  const GLWASM_Allocator * alloc;
  GLWASM_Module * mod;
} Reader;

static int fail(Reader * r, GLWASM_Result err) {
  r->err = err;
  return 0;
}

static int fail_limit(Reader * r, const char * field) {
  r->err = GLWASM_ERR_LIMIT;
  r->limit = field;
  return 0;
}

static void * alloc_bytes(Reader * r, size_t bytes) {
  void * p;

  if (bytes == 0) {
    bytes = 1;
  }
  p = gcu_allocator_malloc(r->alloc, bytes);
  if (!p) {
    r->err = GLWASM_ERR_OOM;
  }
  return p;
}

static void * alloc_array(Reader * r, uint32_t count, size_t elem,
    uint32_t cap, const char * field) {
  if (count > cap) {
    fail_limit(r, field);
    return NULL;
  }
  if (count == 0) {
    return NULL;
  }
  if (elem != 0 && (size_t)count > SIZE_MAX / elem) {
    r->err = GLWASM_ERR_OOM;
    return NULL;
  }
  {
    void * p = gcu_allocator_calloc(r->alloc, count, elem);
    if (!p) {
      r->err = GLWASM_ERR_OOM;
    }
    return p;
  }
}

static void mod_free(GLWASM_Module * m, void * p) {
  gcu_allocator_free(&m->alloc, p);
}

static int read_unsigned(Reader * r, uint64_t * out, int nbits) {
  int max = (nbits + 6) / 7;
  uint64_t result = 0;
  int i;

  for (i = 0; i < max; i++) {
    uint8_t byte;
    int remain;

    if (r->p >= r->end) {
      return fail(r, GLWASM_ERR_CORRUPT);
    }
    byte = *r->p++;
    result |= (uint64_t)(byte & 0x7fu) << (i * 7);
    if ((byte & 0x80u) == 0) {
      remain = nbits - i * 7;
      if (remain < 7 && ((byte & 0x7fu) >> remain) != 0) {
        return fail(r, GLWASM_ERR_CORRUPT);
      }
      *out = result;
      return 1;
    }
  }
  return fail(r, GLWASM_ERR_CORRUPT);
}

static int read_signed(Reader * r, int64_t * out, int nbits) {
  int max = (nbits + 6) / 7;
  uint64_t result = 0;
  int i;

  for (i = 0; i < max; i++) {
    uint8_t byte;

    if (r->p >= r->end) {
      return fail(r, GLWASM_ERR_CORRUPT);
    }
    byte = *r->p++;
    result |= (uint64_t)(byte & 0x7fu) << (i * 7);
    if ((byte & 0x80u) == 0) {
      int remain = nbits - i * 7;

      if (remain < 7) {
        uint8_t payload = (uint8_t)(byte & 0x7fu);
        uint8_t used = (uint8_t)((1u << remain) - 1u);
        uint8_t sign = (uint8_t)((payload >> (remain - 1)) & 1u);
        uint8_t high = (uint8_t)(payload & (uint8_t)~used);
        uint8_t expect = sign ? (uint8_t)~used & 0x7fu : 0;

        if (high != expect) {
          return fail(r, GLWASM_ERR_CORRUPT);
        }
      }
      if ((byte & 0x40u) != 0 && (i * 7 + 7) < 64) {
        result |= ~0ull << (i * 7 + 7);
      }
      *out = (int64_t)result;
      return 1;
    }
  }
  return fail(r, GLWASM_ERR_CORRUPT);
}

static int read_u32(Reader * r, uint32_t * out) {
  uint64_t v;

  if (!read_unsigned(r, &v, 32)) {
    return 0;
  }
  *out = (uint32_t)v;
  return 1;
}

static int read_u64(Reader * r, uint64_t * out) {
  return read_unsigned(r, out, 64);
}

static int read_s32(Reader * r, int32_t * out) {
  int64_t v;

  if (!read_signed(r, &v, 32)) {
    return 0;
  }
  *out = (int32_t)v;
  return 1;
}

static int read_s33(Reader * r, int64_t * out) {
  return read_signed(r, out, 33);
}

static int read_s64(Reader * r, int64_t * out) {
  return read_signed(r, out, 64);
}

static int read_byte(Reader * r, uint8_t * out) {
  if (r->p >= r->end) {
    return fail(r, GLWASM_ERR_CORRUPT);
  }
  *out = *r->p++;
  return 1;
}

static int utf8_ok(const uint8_t * s, uint32_t n) {
  uint32_t i = 0;

  while (i < n) {
    uint8_t c = s[i];
    uint32_t need;
    uint32_t cp;
    uint32_t minv;
    uint32_t k;

    if (c <= 0x7fu) {
      i++;
      continue;
    }
    if ((c & 0xe0u) == 0xc0u) {
      need = 2;
      cp = c & 0x1fu;
      minv = 0x80u;
    } else if ((c & 0xf0u) == 0xe0u) {
      need = 3;
      cp = c & 0x0fu;
      minv = 0x800u;
    } else if ((c & 0xf8u) == 0xf0u) {
      need = 4;
      cp = c & 0x07u;
      minv = 0x10000u;
    } else {
      return 0;
    }
    if (i + need > n) {
      return 0;
    }
    for (k = 1; k < need; k++) {
      uint8_t cc = s[i + k];

      if ((cc & 0xc0u) != 0x80u) {
        return 0;
      }
      cp = (cp << 6) | (cc & 0x3fu);
    }
    if (cp < minv || cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu)) {
      return 0;
    }
    i += need;
  }
  return 1;
}

static int read_name(Reader * r, GLWASM_Name * name) {
  uint32_t n;

  if (!read_u32(r, &n)) {
    return 0;
  }
  if (n > r->limits->name_length) {
    return fail_limit(r, "name_length");
  }
  if ((size_t)(r->end - r->p) < n) {
    return fail(r, GLWASM_ERR_CORRUPT);
  }
  if (!utf8_ok(r->p, n)) {
    return fail(r, GLWASM_ERR_CORRUPT);
  }
  if (n == 0) {
    name->bytes = NULL;
    name->length = 0;
    return 1;
  }
  name->bytes = alloc_bytes(r, (size_t)n + 1u);
  if (!name->bytes) {
    return 0;
  }
  memcpy(name->bytes, r->p, n);
  name->bytes[n] = '\0';
  name->length = n;
  r->p += n;
  return 1;
}

static int ref_in_scope(const GLWASM_ValType * t) {
  if (t->kind != GLWASM_VK_REF || !t->nullable) {
    return 0;
  }
  /* The bottom types are in the func, extern and exn lattices that
   * reference types and exception handling use. The any hierarchy's heap
   * types (any, eq, i31, struct, array, none) are GC and stay deferred. */
  return t->heap == GLWASM_HT_FUNC || t->heap == GLWASM_HT_EXTERN ||
      t->heap == GLWASM_HT_EXN || t->heap == GLWASM_HT_NOFUNC ||
      t->heap == GLWASM_HT_NOEXTERN || t->heap == GLWASM_HT_NOEXN;
}

static int heap_from_byte(uint8_t b, uint8_t * heap) {
  switch (b) {
    case 0x69:
      *heap = GLWASM_HT_EXN;
      return 1;
    case 0x6a:
      *heap = GLWASM_HT_ARRAY;
      return 1;
    case 0x6b:
      *heap = GLWASM_HT_STRUCT;
      return 1;
    case 0x6c:
      *heap = GLWASM_HT_I31;
      return 1;
    case 0x6d:
      *heap = GLWASM_HT_EQ;
      return 1;
    case 0x6e:
      *heap = GLWASM_HT_ANY;
      return 1;
    case 0x6f:
      *heap = GLWASM_HT_EXTERN;
      return 1;
    case 0x70:
      *heap = GLWASM_HT_FUNC;
      return 1;
    case 0x71:
      *heap = GLWASM_HT_NONE;
      return 1;
    case 0x72:
      *heap = GLWASM_HT_NOEXTERN;
      return 1;
    case 0x73:
      *heap = GLWASM_HT_NOFUNC;
      return 1;
    case 0x74:
      *heap = GLWASM_HT_NOEXN;
      return 1;
    default:
      return 0;
  }
}

static int read_heap(Reader * r, GLWASM_ValType * t, int nullable) {
  uint8_t b;
  uint8_t heap;
  int64_t idx;

  if (r->p >= r->end) {
    return fail(r, GLWASM_ERR_CORRUPT);
  }
  b = *r->p;
  if (heap_from_byte(b, &heap)) {
    r->p++;
    t->kind = GLWASM_VK_REF;
    t->nullable = (uint8_t)nullable;
    t->heap = heap;
    t->index = 0;
    if (!ref_in_scope(t)) {
      r->deferred = 1;
    }
    return 1;
  }
  if (!read_s33(r, &idx)) {
    return 0;
  }
  if (idx < 0) {
    return fail(r, GLWASM_ERR_CORRUPT);
  }
  t->kind = GLWASM_VK_REF;
  t->nullable = (uint8_t)nullable;
  t->heap = GLWASM_HT_INDEX;
  t->index = (uint32_t)idx;
  r->deferred = 1;
  return 1;
}

static int read_valtype(Reader * r, GLWASM_ValType * t) {
  uint8_t b;

  memset(t, 0, sizeof(*t));
  if (!read_byte(r, &b)) {
    return 0;
  }
  switch (b) {
    case 0x7f:
      t->kind = GLWASM_VK_I32;
      return 1;
    case 0x7e:
      t->kind = GLWASM_VK_I64;
      return 1;
    case 0x7d:
      t->kind = GLWASM_VK_F32;
      return 1;
    case 0x7c:
      t->kind = GLWASM_VK_F64;
      return 1;
    case 0x7b:
      t->kind = GLWASM_VK_V128;
      r->deferred = 1;
      return 1;
    case 0x63:
      return read_heap(r, t, 1);
    case 0x64:
      return read_heap(r, t, 0);
    default:
      break;
  }
  {
    uint8_t heap;

    if (!heap_from_byte(b, &heap)) {
      return fail(r, GLWASM_ERR_CORRUPT);
    }
    t->kind = GLWASM_VK_REF;
    t->nullable = 1;
    t->heap = heap;
    if (!ref_in_scope(t)) {
      r->deferred = 1;
    }
    return 1;
  }
}

static int peek_valtype(uint8_t b) {
  return (b >= 0x63 && b <= 0x74) || (b >= 0x7b && b <= 0x7f);
}

static int read_result(Reader * r, GLWASM_ValType * * out, uint32_t * nout) {
  uint32_t n;
  uint32_t i;
  GLWASM_ValType * types;

  if (!read_u32(r, &n)) {
    return 0;
  }
  /* A result list is bounded by the locals cap: it is a list of types, and
   * a function's parameters are locals. */
  types = alloc_array(r, n, sizeof(*types), r->limits->locals, "locals");
  if (n != 0 && !types) {
    return 0;
  }
  for (i = 0; i < n; i++) {
    if (!read_valtype(r, &types[i])) {
      gcu_allocator_free(r->alloc, types);
      return 0;
    }
  }
  *out = types;
  *nout = n;
  return 1;
}

static int read_mut(Reader * r, uint8_t * mut) {
  uint8_t b;

  if (!read_byte(r, &b)) {
    return 0;
  }
  if (b > 1) {
    return fail(r, GLWASM_ERR_CORRUPT);
  }
  *mut = b;
  return 1;
}

static int read_field(Reader * r, GLWASM_ValType * t) {
  uint8_t b;
  uint8_t mut;

  if (r->p >= r->end) {
    return fail(r, GLWASM_ERR_CORRUPT);
  }
  b = *r->p;
  if (b == 0x78 || b == 0x77) {
    r->p++;
    memset(t, 0, sizeof(*t));
    t->kind = GLWASM_VK_I32;
    t->mut = b;
    r->deferred = 1;
    if (!read_mut(r, &mut)) {
      return 0;
    }
    t->mut = mut;
    return 1;
  }
  if (!read_valtype(r, t)) {
    return 0;
  }
  if (!read_mut(r, &mut)) {
    return 0;
  }
  t->mut = mut;
  return 1;
}

typedef struct LimInfo {
  uint8_t addr64;
  uint8_t has_max;
  uint64_t min;
  uint64_t max;
} LimInfo;

static int read_limits(Reader * r, LimInfo * lim, int memory) {
  uint8_t flags;

  memset(lim, 0, sizeof(*lim));
  if (!read_byte(r, &flags)) {
    return 0;
  }
  if (flags > 0x07u) {
    return fail(r, GLWASM_ERR_CORRUPT);
  }
  /* Bit 0x02 is the shared-memory flag. On a table it is not that flag. */
  if ((flags & 0x02u) != 0) {
    if (!memory) {
      return fail(r, GLWASM_ERR_CORRUPT);
    }
    r->deferred = 1;
  }
  lim->addr64 = (uint8_t)((flags & 0x04u) != 0);
  lim->has_max = (uint8_t)((flags & 0x01u) != 0);
  if (!read_u64(r, &lim->min)) {
    return 0;
  }
  if (lim->has_max && !read_u64(r, &lim->max)) {
    return 0;
  }
  return 1;
}

static int check_memory(Reader * r, const LimInfo * lim) {
  uint64_t spec = lim->addr64 ? (1ull << 48) : 65536ull;
  uint64_t eff;
  uint64_t min_bytes;
  uint64_t eff_bytes;

  if (lim->min > UINT64_MAX / GLWASM_PAGE) {
    return fail_limit(r, "memory_initial");
  }
  min_bytes = lim->min * GLWASM_PAGE;
  if (min_bytes > r->limits->memory_initial) {
    return fail_limit(r, "memory_initial");
  }
  eff = lim->has_max ? lim->max : spec;
  if (eff > UINT64_MAX / GLWASM_PAGE) {
    return fail_limit(r, "memory_maximum");
  }
  eff_bytes = eff * GLWASM_PAGE;
  if (eff_bytes > r->limits->memory_maximum) {
    return fail_limit(r, "memory_maximum");
  }
  if (lim->min > spec || (lim->has_max && (lim->max < lim->min ||
      lim->max > spec))) {
    return fail(r, GLWASM_ERR_VALIDATE);
  }
  return 1;
}

static int check_table_size(Reader * r, const LimInfo * lim) {
  uint64_t spec = lim->addr64 ? UINT64_MAX : UINT32_MAX;
  uint64_t eff = lim->has_max ? lim->max : spec;

  if (lim->min > r->limits->table_size || eff > r->limits->table_size) {
    return fail_limit(r, "table_size");
  }
  if (lim->min > spec || (lim->has_max && (lim->max < lim->min ||
      lim->max > spec))) {
    return fail(r, GLWASM_ERR_VALIDATE);
  }
  return 1;
}

static int ensure_cap(Reader * r, void * old, void * * out, uint32_t * cap,
    uint32_t need, uint32_t limit, size_t elem, const char * field) {
  uint32_t ncap;
  void * grown;

  if (need <= *cap) {
    *out = old;
    return 1;
  }
  if (need > limit) {
    return fail_limit(r, field);
  }
  ncap = *cap ? *cap : 4u;
  while (ncap < need) {
    if (ncap > limit / 2u) {
      ncap = limit;
      break;
    }
    ncap *= 2u;
  }
  if (ncap > limit) {
    ncap = limit;
  }
  if (ncap < need) {
    return fail_limit(r, field);
  }
  if (elem != 0 && (size_t)ncap > SIZE_MAX / elem) {
    return fail(r, GLWASM_ERR_OOM);
  }
  grown = gcu_allocator_realloc(r->alloc, old, (size_t)ncap * elem);
  if (!grown) {
    return fail(r, GLWASM_ERR_OOM);
  }
  *out = grown;
  *cap = ncap;
  return 1;
}

/* Assigns through a void * so the slot's type is not punned. */
#define GLWASM_GROW(slot, capslot, need, limit, elem, field) \
  do { \
    void * glwasm_grown_ = (slot); \
    if (!ensure_cap(r, glwasm_grown_, &glwasm_grown_, &(capslot), (need), \
        (limit), (elem), (field))) { \
      return 0; \
    } \
    (slot) = glwasm_grown_; \
  } while (0)

static int push_v(Reader * r, GLWASM_Func * fn, GLWASM_ValType t,
    uint32_t * at) {
  GLWASM_GROW(fn->vpool, fn->vcap, fn->nv + 1u, r->limits->instructions,
      sizeof(GLWASM_ValType), "instructions");
  *at = fn->nv;
  fn->vpool[fn->nv++] = t;
  return 1;
}

static int push_block(Reader * r, GLWASM_Func * fn, GLWASM_BlockType bt,
    uint32_t * at) {
  GLWASM_GROW(fn->blocks, fn->block_cap, fn->nblocks + 1u,
      r->limits->instructions, sizeof(GLWASM_BlockType), "instructions");
  *at = fn->nblocks;
  fn->blocks[fn->nblocks++] = bt;
  return 1;
}

static int copy_vals(Reader * r, GLWASM_Func * fn, const GLWASM_ValType * src,
    uint32_t n, uint32_t * at) {
  uint32_t i;

  if (n == 0) {
    *at = 0;
    return 1;
  }
  GLWASM_GROW(fn->vpool, fn->vcap, fn->nv + n, r->limits->instructions,
      sizeof(GLWASM_ValType), "instructions");
  *at = fn->nv;
  for (i = 0; i < n; i++) {
    fn->vpool[fn->nv++] = src[i];
  }
  return 1;
}

static int read_blocktype(Reader * r, GLWASM_Func * fn, uint32_t * at) {
  GLWASM_BlockType bt;
  uint8_t b;

  memset(&bt, 0, sizeof(bt));
  bt.type_index = UINT32_MAX;
  bt.resolved = 1;
  if (r->p >= r->end) {
    return fail(r, GLWASM_ERR_CORRUPT);
  }
  b = *r->p;
  if (b == 0x40) {
    r->p++;
    return push_block(r, fn, bt, at);
  }
  if (peek_valtype(b)) {
    GLWASM_ValType t;

    if (!read_valtype(r, &t)) {
      return 0;
    }
    bt.nresults = 1;
    if (!copy_vals(r, fn, &t, 1, &bt.result_at)) {
      return 0;
    }
    return push_block(r, fn, bt, at);
  }
  {
    int64_t idx;

    if (!read_s33(r, &idx) || idx < 0) {
      return r->err == GLWASM_OK ? fail(r, GLWASM_ERR_CORRUPT) : 0;
    }
    bt.type_index = (uint32_t)idx;
    if (idx < r->mod->ntypes &&
        r->mod->types[idx].comp == GLWASM_COMP_FUNC) {
      GLWASM_TypeDef * td = &r->mod->types[idx];

      bt.nparams = td->nparams;
      bt.nresults = td->nresults;
      if (!copy_vals(r, fn, td->params, td->nparams, &bt.param_at)) {
        return 0;
      }
      if (!copy_vals(r, fn, td->results, td->nresults, &bt.result_at)) {
        return 0;
      }
    } else {
      bt.resolved = 0;
    }
    return push_block(r, fn, bt, at);
  }
}

static int read_memarg(Reader * r, GLWASM_Instr * ins) {
  uint32_t flags;
  uint32_t mem = 0;
  uint64_t offset;

  if (!read_u32(r, &flags)) {
    return 0;
  }
  if (flags >= 128u) {
    return fail(r, GLWASM_ERR_CORRUPT);
  }
  if (flags >= 64u) {
    if (!read_u32(r, &mem)) {
      return 0;
    }
    flags -= 64u;
  }
  if (!read_u64(r, &offset)) {
    return 0;
  }
  ins->a = flags;
  ins->b = mem;
  ins->c = offset;
  return 1;
}

static int push_ins(Reader * r, GLWASM_Func * fn, GLWASM_Instr ins) {
  if (fn->nins >= r->limits->instructions) {
    return fail_limit(r, "instructions");
  }
  GLWASM_GROW(fn->ins, fn->ins_cap, fn->nins + 1u, r->limits->instructions,
      sizeof(GLWASM_Instr), "instructions");
  fn->ins[fn->nins++] = ins;
  return 1;
}

/* The control stack grows with the depth and never past the nesting cap. */
static int ensure_kind(Reader * r, uint8_t * * kinds, uint32_t * kcap,
    int index) {
  void * old;
  void * grown;
  uint32_t ncap;

  if (index < 0 || (uint32_t)index >= r->limits->nesting) {
    return fail_limit(r, "nesting");
  }
  if ((uint32_t)index < *kcap) {
    return 1;
  }
  ncap = *kcap ? *kcap : 4u;
  while (ncap <= (uint32_t)index) {
    if (ncap > r->limits->nesting / 2u) {
      ncap = r->limits->nesting;
      break;
    }
    ncap *= 2u;
  }
  if (ncap > r->limits->nesting) {
    ncap = r->limits->nesting;
  }
  if ((uint32_t)index >= ncap) {
    return fail_limit(r, "nesting");
  }
  old = *kinds;
  grown = gcu_allocator_realloc(r->alloc, old, ncap);
  if (!grown) {
    return fail(r, GLWASM_ERR_OOM);
  }
  memset((uint8_t *)grown + *kcap, 0, (size_t)(ncap - *kcap));
  *kinds = grown;
  *kcap = ncap;
  return 1;
}

/* 1 = instruction stored, 2 = deferred and the rest of the region is opaque. */
static int read_instr(Reader * r, GLWASM_Func * fn, int * depth,
    uint8_t * * kinds, uint32_t * kcap) {
  uint8_t op;
  GLWASM_Instr ins;

  memset(&ins, 0, sizeof(ins));
  if (!read_byte(r, &op)) {
    return 0;
  }
  ins.op = op;
  switch (op) {
    case 0x00: /* unreachable */
    case 0x01: /* nop */
    case 0x0a: /* throw_ref */
    case 0x0f: /* return */
    case 0x1a: /* drop */
    case 0x1b: /* select */
    case 0xd1: /* ref.is_null */
      return push_ins(r, fn, ins);
    case 0x05: { /* else */
      if (*depth <= 1 || ((*kinds)[*depth - 1] & 0x7fu) != GLWASM_KIND_IF ||
          ((*kinds)[*depth - 1] & 0x80u) != 0) {
        return fail(r, GLWASM_ERR_CORRUPT);
      }
      (*kinds)[*depth - 1] = (uint8_t)(GLWASM_KIND_IF | 0x80u);
      return push_ins(r, fn, ins);
    }
    case 0x0b: { /* end */
      if (*depth <= 0) {
        return fail(r, GLWASM_ERR_CORRUPT);
      }
      (*depth)--;
      return push_ins(r, fn, ins);
    }
    case 0x02:
    case 0x03:
    case 0x04: {
      uint8_t kind = op == 0x02 ? GLWASM_KIND_BLOCK :
          op == 0x03 ? GLWASM_KIND_LOOP : GLWASM_KIND_IF;

      if (!ensure_kind(r, kinds, kcap, *depth)) {
        return 0;
      }
      if (!read_blocktype(r, fn, &ins.a)) {
        return 0;
      }
      (*kinds)[*depth] = kind;
      (*depth)++;
      return push_ins(r, fn, ins);
    }
    case 0x1f: { /* try_table */
      uint32_t n;
      uint32_t i;

      if (!ensure_kind(r, kinds, kcap, *depth)) {
        return 0;
      }
      if (!read_blocktype(r, fn, &ins.a)) {
        return 0;
      }
      if (!read_u32(r, &n)) {
        return 0;
      }
      if (n > r->limits->br_table) {
        return fail_limit(r, "br_table");
      }
      /* The per-instruction cap was already applied. The pool is the sum,
       * so its ceiling is not that one cap and not the instructions cap. */
      GLWASM_GROW(fn->catches, fn->catch_cap, fn->ncatches + n,
          UINT32_MAX, sizeof(GLWASM_Catch), "br_table");
      ins.b = fn->ncatches;
      ins.d = n;
      for (i = 0; i < n; i++) {
        uint8_t ck;
        GLWASM_Catch c;

        memset(&c, 0, sizeof(c));
        if (!read_byte(r, &ck)) {
          return 0;
        }
        if (ck > 3) {
          return fail(r, GLWASM_ERR_CORRUPT);
        }
        c.kind = ck;
        if (ck < 2 && !read_u32(r, &c.tag)) {
          return 0;
        }
        if (!read_u32(r, &c.label)) {
          return 0;
        }
        fn->catches[fn->ncatches++] = c;
      }
      (*kinds)[*depth] = GLWASM_KIND_TRY;
      (*depth)++;
      return push_ins(r, fn, ins);
    }
    case 0x08: /* throw */
    case 0x0c: /* br */
    case 0x0d: /* br_if */
    case 0x10: /* call */
    case 0x12: /* return_call */
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24:
    case 0x25: case 0x26:
    case 0xd2: /* ref.func */
      if (!read_u32(r, &ins.a)) {
        return 0;
      }
      return push_ins(r, fn, ins);
    case 0x11: /* call_indirect */
    case 0x13: /* return_call_indirect */
      if (!read_u32(r, &ins.a) || !read_u32(r, &ins.b)) {
        return 0;
      }
      return push_ins(r, fn, ins);
    case 0x0e: { /* br_table */
      uint32_t n;
      uint32_t i;
      uint32_t def;

      if (!read_u32(r, &n)) {
        return 0;
      }
      if (n == UINT32_MAX || n + 1u > r->limits->br_table) {
        return fail_limit(r, "br_table");
      }
      /* Same as the catch pool: one instruction was capped above. */
      GLWASM_GROW(fn->u32s, fn->u32_cap, fn->nu32 + n + 1u,
          UINT32_MAX, sizeof(uint32_t), "br_table");
      ins.a = fn->nu32;
      ins.b = n + 1u;
      for (i = 0; i < n; i++) {
        uint32_t lab;

        if (!read_u32(r, &lab)) {
          return 0;
        }
        fn->u32s[fn->nu32++] = lab;
      }
      if (!read_u32(r, &def)) {
        return 0;
      }
      fn->u32s[fn->nu32++] = def;
      return push_ins(r, fn, ins);
    }
    case 0x1c: { /* select t* */
      uint32_t n;
      GLWASM_ValType t;

      if (!read_u32(r, &n)) {
        return 0;
      }
      if (n != 1u) {
        /* The vector is part of the instruction. A huge count is the
         * instruction cap; length other than 1 is caught in validation
         * once the single type has been stored. More than one type is
         * still parsed only up to the cap. */
        if (n > r->limits->instructions) {
          return fail_limit(r, "instructions");
        }
      }
      ins.d = n;
      if (n == 0) {
        return push_ins(r, fn, ins);
      }
      if (!read_valtype(r, &t) || !push_v(r, fn, t, &ins.a)) {
        return 0;
      }
      {
        uint32_t extra;

        for (extra = 1; extra < n; extra++) {
          GLWASM_ValType skip;

          if (!read_valtype(r, &skip)) {
            return 0;
          }
        }
      }
      return push_ins(r, fn, ins);
    }
    case 0x28: case 0x29: case 0x2a: case 0x2b:
    case 0x2c: case 0x2d: case 0x2e: case 0x2f:
    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x34: case 0x35: case 0x36: case 0x37:
    case 0x38: case 0x39: case 0x3a: case 0x3b:
    case 0x3c: case 0x3d: case 0x3e:
      if (!read_memarg(r, &ins)) {
        return 0;
      }
      return push_ins(r, fn, ins);
    case 0x3f: /* memory.size */
    case 0x40: /* memory.grow */
      if (!read_u32(r, &ins.a)) {
        return 0;
      }
      return push_ins(r, fn, ins);
    case 0x41: {
      int32_t v;

      if (!read_s32(r, &v)) {
        return 0;
      }
      ins.c = (uint64_t)(int64_t)v;
      return push_ins(r, fn, ins);
    }
    case 0x42: {
      int64_t v;

      if (!read_s64(r, &v)) {
        return 0;
      }
      ins.c = (uint64_t)v;
      return push_ins(r, fn, ins);
    }
    case 0x43: {
      uint32_t bits = 0;
      int k;

      for (k = 0; k < 4; k++) {
        uint8_t b;

        if (!read_byte(r, &b)) {
          return 0;
        }
        bits |= (uint32_t)b << (8 * k);
      }
      ins.a = bits;
      return push_ins(r, fn, ins);
    }
    case 0x44: {
      uint64_t bits = 0;
      int k;

      for (k = 0; k < 8; k++) {
        uint8_t b;

        if (!read_byte(r, &b)) {
          return 0;
        }
        bits |= (uint64_t)b << (8 * k);
      }
      ins.c = bits;
      return push_ins(r, fn, ins);
    }
    case 0xd0: { /* ref.null */
      GLWASM_ValType t;

      if (!read_heap(r, &t, 1)) {
        return 0;
      }
      if (!push_v(r, fn, t, &ins.a)) {
        return 0;
      }
      return push_ins(r, fn, ins);
    }
    case 0xfc: {
      uint32_t sub;

      if (!read_u32(r, &sub)) {
        return 0;
      }
      ins.op = (uint16_t)(0xfc00u | (sub > 0xffffu ? 0xffffu : sub));
      if (sub > 17u) {
        return fail(r, GLWASM_ERR_CORRUPT);
      }
      if (sub <= 7u) {
        return push_ins(r, fn, ins);
      }
      if (sub == 8u || sub == 12u || sub == 10u || sub == 14u) {
        if (!read_u32(r, &ins.a) || !read_u32(r, &ins.b)) {
          return 0;
        }
      } else if (!read_u32(r, &ins.a)) {
        return 0;
      }
      return push_ins(r, fn, ins);
    }
    case 0x06: /* legacy try */
    case 0x07: /* legacy catch */
    case 0x09: /* legacy rethrow */
    case 0x18: /* legacy delegate */
    case 0x19: /* legacy catch_all */
    case 0x14: /* call_ref */
    case 0x15: /* return_call_ref */
    case 0xd3: /* ref.eq */
    case 0xd4: /* ref.as_non_null */
    case 0xd5: /* br_on_null */
    case 0xd6: /* br_on_non_null */
      r->deferred = 1;
      return 2;
    case 0xfb:
    case 0xfd:
    case 0xfe: {
      uint32_t sub;

      if (!read_u32(r, &sub)) {
        return 0;
      }
      (void)sub;
      r->deferred = 1;
      return 2;
    }
    default:
      if (op >= 0x45 && op <= 0xc4) {
        return push_ins(r, fn, ins);
      }
      return fail(r, GLWASM_ERR_CORRUPT);
  }
}

static int read_expr(Reader * r, GLWASM_Func * fn, int const_expr) {
  uint8_t * kinds = NULL;
  uint32_t kcap = 0;
  int depth = 1;

  fn->is_const = (uint8_t)const_expr;
  if (!ensure_kind(r, &kinds, &kcap, 0)) {
    return 0;
  }
  kinds[0] = GLWASM_KIND_FUNC;
  while (r->p < r->end && depth > 0) {
    int rc = read_instr(r, fn, &depth, &kinds, &kcap);

    if (rc == 0) {
      gcu_allocator_free(r->alloc, kinds);
      return 0;
    }
    if (rc == 2) {
      gcu_allocator_free(r->alloc, kinds);
      r->skip_rest = 1;
      return 1;
    }
  }
  gcu_allocator_free(r->alloc, kinds);
  if (depth != 0) {
    return fail(r, GLWASM_ERR_CORRUPT);
  }
  return 1;
}

static int read_locals(Reader * r, GLWASM_Func * fn) {
  uint32_t groups;
  uint32_t i;
  uint32_t n;
  uint32_t filled;
  const uint8_t * start;
  GLWASM_ValType * locals;

  if (!read_u32(r, &groups)) {
    return 0;
  }
  if (groups > r->limits->locals) {
    return fail_limit(r, "locals");
  }
  /* Sum the groups before allocating the local array, so a count past the
   * cap returns ERR_LIMIT without asking for that array. */
  start = r->p;
  n = fn->nparams;
  for (i = 0; i < groups; i++) {
    uint32_t count;
    GLWASM_ValType type;

    if (!read_u32(r, &count) || !read_valtype(r, &type)) {
      return 0;
    }
    if (count > r->limits->locals - n) {
      return fail_limit(r, "locals");
    }
    n += count;
  }
  locals = alloc_array(r, n, sizeof(*locals), r->limits->locals, "locals");
  if (n != 0 && !locals) {
    return 0;
  }
  r->p = start;
  filled = 0;
  if (fn->type_index < r->mod->ntypes &&
      r->mod->types[fn->type_index].comp == GLWASM_COMP_FUNC) {
    GLWASM_TypeDef * td = &r->mod->types[fn->type_index];
    uint32_t p;

    for (p = 0; p < td->nparams && filled < n; p++) {
      locals[filled++] = td->params[p];
    }
  }
  for (i = 0; i < groups; i++) {
    uint32_t count;
    uint32_t k;
    GLWASM_ValType type;

    if (!read_u32(r, &count) || !read_valtype(r, &type)) {
      gcu_allocator_free(r->alloc, locals);
      return 0;
    }
    for (k = 0; k < count; k++) {
      locals[filled++] = type;
    }
  }
  fn->locals = locals;
  fn->nlocals = n;
  return 1;
}

static int read_code_entry(Reader * r, GLWASM_Func * fn,
    const uint8_t * body_end) {
  const uint8_t * saved = r->end;
  int ok;

  r->end = body_end;
  ok = read_locals(r, fn);
  if (ok) {
    ok = read_expr(r, fn, 0);
  }
  if (ok && !r->skip_rest && r->p != body_end) {
    ok = fail(r, GLWASM_ERR_CORRUPT);
  }
  if (r->skip_rest) {
    r->p = body_end;
    r->skip_rest = 0;
  }
  r->end = saved;
  return ok;
}

static void free_func(GLWASM_Module * m, GLWASM_Func * fn) {
  mod_free(m, fn->locals);
  mod_free(m, fn->ins);
  mod_free(m, fn->u32s);
  mod_free(m, fn->vpool);
  mod_free(m, fn->blocks);
  mod_free(m, fn->catches);
  memset(fn, 0, sizeof(*fn));
}

static void free_name(GLWASM_Module * m, GLWASM_Name * name) {
  mod_free(m, name->bytes);
  name->bytes = NULL;
  name->length = 0;
}

static void free_type(GLWASM_Module * m, GLWASM_TypeDef * td) {
  mod_free(m, td->supers);
  mod_free(m, td->params);
  mod_free(m, td->results);
  mod_free(m, td->muts);
  memset(td, 0, sizeof(*td));
}

void glwasm_module_destroy(GLWASM_Module * module) {
  uint32_t i;

  if (!module) {
    return;
  }
  for (i = 0; i < module->ntypes; i++) {
    free_type(module, &module->types[i]);
  }
  mod_free(module, module->types);
  for (i = 0; i < module->nimports; i++) {
    free_name(module, &module->imports[i].module_name);
    free_name(module, &module->imports[i].field);
  }
  mod_free(module, module->imports);
  for (i = 0; i < module->nfuncs; i++) {
    free_func(module, &module->funcs[i]);
  }
  mod_free(module, module->funcs);
  for (i = 0; i < module->ntables; i++) {
    if (module->tables[i].has_init) {
      free_func(module, &module->tables[i].init);
    }
  }
  mod_free(module, module->tables);
  mod_free(module, module->mems);
  for (i = 0; i < module->nglobals; i++) {
    free_func(module, &module->globals[i].init);
  }
  mod_free(module, module->globals);
  for (i = 0; i < module->nexports; i++) {
    free_name(module, &module->exports[i].name);
  }
  mod_free(module, module->exports);
  for (i = 0; i < module->nelems; i++) {
    uint32_t e;

    free_func(module, &module->elems[i].offset);
    mod_free(module, module->elems[i].funcs);
    for (e = 0; e < module->elems[i].nexprs; e++) {
      free_func(module, &module->elems[i].exprs[e]);
    }
    mod_free(module, module->elems[i].exprs);
  }
  mod_free(module, module->elems);
  for (i = 0; i < module->ndatas; i++) {
    free_func(module, &module->datas[i].offset);
    mod_free(module, module->datas[i].bytes);
  }
  mod_free(module, module->datas);
  mod_free(module, module->tags);
  gcu_allocator_free(&module->alloc, module);
}

static int reserve(Reader * r, void * old, void * * out, uint32_t * n,
    uint32_t extra, uint32_t cap, size_t elem, const char * field) {
  uint32_t want;
  void * grown;

  if (extra > cap || *n > cap - extra) {
    return fail_limit(r, field);
  }
  want = *n + extra;
  if (want == 0) {
    *out = old;
    return 1;
  }
  if (elem != 0 && (size_t)want > SIZE_MAX / elem) {
    return fail(r, GLWASM_ERR_OOM);
  }
  grown = gcu_allocator_realloc(r->alloc, old, (size_t)want * elem);
  if (!grown) {
    return fail(r, GLWASM_ERR_OOM);
  }
  memset((uint8_t *)grown + (size_t)(*n) * elem, 0, (size_t)extra * elem);
  *out = grown;
  return 1;
}

#define GLWASM_RESERVE(slot, nslot, extra, cap, elem, field) \
  do { \
    void * glwasm_grown_ = (slot); \
    if (!reserve(r, glwasm_grown_, &glwasm_grown_, &(nslot), (extra), \
        (cap), (elem), (field))) { \
      return 0; \
    } \
    (slot) = glwasm_grown_; \
  } while (0)

static int read_comptype(Reader * r, GLWASM_TypeDef * td) {
  uint8_t tag;

  if (!read_byte(r, &tag)) {
    return 0;
  }
  if (tag == 0x60) {
    td->comp = GLWASM_COMP_FUNC;
    if (!read_result(r, &td->params, &td->nparams)) {
      return 0;
    }
    return read_result(r, &td->results, &td->nresults);
  }
  if (tag == 0x5e) {
    td->comp = GLWASM_COMP_ARRAY;
    r->deferred = 1;
    td->params = alloc_array(r, 1, sizeof(GLWASM_ValType), 1, "types");
    if (!td->params || !read_field(r, &td->params[0])) {
      return 0;
    }
    td->nparams = 1;
    return 1;
  }
  if (tag == 0x5f) {
    uint32_t n;
    uint32_t i;

    td->comp = GLWASM_COMP_STRUCT;
    r->deferred = 1;
    if (!read_u32(r, &n)) {
      return 0;
    }
    td->params = alloc_array(r, n, sizeof(GLWASM_ValType), r->limits->locals,
        "locals");
    if (n != 0 && !td->params) {
      return 0;
    }
    for (i = 0; i < n; i++) {
      if (!read_field(r, &td->params[i])) {
        return 0;
      }
    }
    td->nparams = n;
    return 1;
  }
  return fail(r, GLWASM_ERR_CORRUPT);
}

static int read_subtype(Reader * r, GLWASM_TypeDef * td) {
  uint8_t b;

  memset(td, 0, sizeof(*td));
  if (r->p >= r->end) {
    return fail(r, GLWASM_ERR_CORRUPT);
  }
  b = *r->p;
  td->is_final = 1;
  if (b == 0x4f || b == 0x50) {
    uint32_t n;
    uint32_t i;

    r->p++;
    td->is_final = (uint8_t)(b == 0x4f);
    if (!read_u32(r, &n)) {
      return 0;
    }
    if (n > r->limits->types) {
      return fail_limit(r, "types");
    }
    if (n != 0) {
      r->deferred = 1;
    }
    td->supers = alloc_array(r, n, sizeof(uint32_t), r->limits->types,
        "types");
    if (n != 0 && !td->supers) {
      return 0;
    }
    for (i = 0; i < n; i++) {
      if (!read_u32(r, &td->supers[i])) {
        return 0;
      }
    }
    td->nsupers = n;
  }
  return read_comptype(r, td);
}

static int read_one_type(Reader * r) {
  GLWASM_Module * m = r->mod;

  GLWASM_RESERVE(m->types, m->ntypes, 1, r->limits->types,
      sizeof(GLWASM_TypeDef), "types");
  if (!read_subtype(r, &m->types[m->ntypes])) {
    /* The slot is not counted yet, so destroy would not see its arrays. */
    free_type(m, &m->types[m->ntypes]);
    return 0;
  }
  m->ntypes++;
  return 1;
}

static int parse_types(Reader * r) {
  uint32_t n;
  uint32_t i;

  if (!read_u32(r, &n)) {
    return 0;
  }
  for (i = 0; i < n; i++) {
    uint8_t b;

    if (r->p >= r->end) {
      return fail(r, GLWASM_ERR_CORRUPT);
    }
    b = *r->p;
    if (b == 0x4e) {
      uint32_t g;
      uint32_t k;

      r->p++;
      if (!read_u32(r, &g)) {
        return 0;
      }
      if (g > r->limits->types ||
          r->mod->ntypes > r->limits->types - g) {
        return fail_limit(r, "types");
      }
      for (k = 0; k < g; k++) {
        if (!read_one_type(r)) {
          return 0;
        }
      }
    } else if (!read_one_type(r)) {
      return 0;
    }
  }
  return 1;
}

static int add_func_slot(Reader * r, uint32_t type_index, int imported) {
  GLWASM_Module * m = r->mod;
  GLWASM_Func * fn;

  GLWASM_RESERVE(m->funcs, m->nfuncs, 1, r->limits->functions,
      sizeof(GLWASM_Func), "functions");
  fn = &m->funcs[m->nfuncs];
  memset(fn, 0, sizeof(*fn));
  fn->type_index = type_index;
  fn->imported = (uint8_t)imported;
  if (type_index < m->ntypes && m->types[type_index].comp == GLWASM_COMP_FUNC) {
    fn->nparams = m->types[type_index].nparams;
  }
  m->nfuncs++;
  if (imported) {
    m->nimported_funcs++;
  }
  return 1;
}

static int add_table(Reader * r, int imported) {
  LimInfo lim;
  GLWASM_ValType elem;
  GLWASM_Table * t;
  int explicit_init = 0;

  if (!imported && r->p < r->end && *r->p == 0x40) {
    uint8_t reserved;

    r->p++;
    if (!read_byte(r, &reserved)) {
      return 0;
    }
    if (reserved != 0) {
      return fail(r, GLWASM_ERR_CORRUPT);
    }
    explicit_init = 1;
  }
  if (!read_valtype(r, &elem)) {
    return 0;
  }
  if (elem.kind != GLWASM_VK_REF) {
    return fail(r, GLWASM_ERR_CORRUPT);
  }
  if (!read_limits(r, &lim, 0) || !check_table_size(r, &lim)) {
    return 0;
  }
  GLWASM_RESERVE(r->mod->tables, r->mod->ntables, 1, r->limits->tables,
      sizeof(GLWASM_Table), "tables");
  t = &r->mod->tables[r->mod->ntables++];
  memset(t, 0, sizeof(*t));
  t->elem = elem;
  t->addr64 = lim.addr64;
  t->has_max = lim.has_max;
  t->min = lim.min;
  t->max = lim.max;
  t->imported = (uint8_t)imported;
  if (explicit_init) {
    t->has_init = 1;
    if (!read_expr(r, &t->init, 1)) {
      return 0;
    }
  }
  return 1;
}

static int add_memory(Reader * r, int imported) {
  LimInfo lim;
  GLWASM_Memory * mem;

  if (!read_limits(r, &lim, 1) || !check_memory(r, &lim)) {
    return 0;
  }
  GLWASM_RESERVE(r->mod->mems, r->mod->nmems, 1, r->limits->memories,
      sizeof(GLWASM_Memory), "memories");
  mem = &r->mod->mems[r->mod->nmems++];
  memset(mem, 0, sizeof(*mem));
  mem->addr64 = lim.addr64;
  mem->has_max = lim.has_max;
  mem->min = lim.min;
  mem->max = lim.max;
  mem->imported = (uint8_t)imported;
  return 1;
}

static int add_global(Reader * r, int imported) {
  GLWASM_ValType ty;
  uint8_t mut;
  GLWASM_Global * g;

  if (!read_valtype(r, &ty) || !read_mut(r, &mut)) {
    return 0;
  }
  GLWASM_RESERVE(r->mod->globals, r->mod->nglobals, 1, r->limits->globals,
      sizeof(GLWASM_Global), "globals");
  g = &r->mod->globals[r->mod->nglobals++];
  memset(g, 0, sizeof(*g));
  g->type = ty;
  g->mut = mut;
  g->imported = (uint8_t)imported;
  if (!imported && !read_expr(r, &g->init, 1)) {
    return 0;
  }
  return 1;
}

static int add_tag(Reader * r, int imported) {
  uint8_t attr;
  uint32_t type_index;
  GLWASM_Tag * tag;

  if (!read_byte(r, &attr)) {
    return 0;
  }
  if (attr != 0) {
    return fail(r, GLWASM_ERR_CORRUPT);
  }
  if (!read_u32(r, &type_index)) {
    return 0;
  }
  GLWASM_RESERVE(r->mod->tags, r->mod->ntags, 1, r->limits->tags,
      sizeof(GLWASM_Tag), "tags");
  tag = &r->mod->tags[r->mod->ntags++];
  tag->type_index = type_index;
  tag->imported = (uint8_t)imported;
  return 1;
}

static int parse_imports(Reader * r) {
  uint32_t n;
  uint32_t i;

  if (!read_u32(r, &n)) {
    return 0;
  }
  if (n > r->limits->imports) {
    return fail_limit(r, "imports");
  }
  r->mod->imports = alloc_array(r, n, sizeof(GLWASM_Import),
      r->limits->imports, "imports");
  if (n != 0 && !r->mod->imports) {
    return 0;
  }
  r->mod->nimports = n;
  for (i = 0; i < n; i++) {
    GLWASM_Import * im = &r->mod->imports[i];
    uint8_t kind;

    if (!read_name(r, &im->module_name) || !read_name(r, &im->field) ||
        !read_byte(r, &kind)) {
      return 0;
    }
    im->kind = kind;
    switch (kind) {
      case 0x00: {
        uint32_t ti;

        if (!read_u32(r, &ti) || !add_func_slot(r, ti, 1)) {
          return 0;
        }
        im->index = r->mod->nfuncs - 1u;
        break;
      }
      case 0x01:
        if (!add_table(r, 1)) {
          return 0;
        }
        im->index = r->mod->ntables - 1u;
        break;
      case 0x02:
        if (!add_memory(r, 1)) {
          return 0;
        }
        im->index = r->mod->nmems - 1u;
        break;
      case 0x03:
        if (!add_global(r, 1)) {
          return 0;
        }
        im->index = r->mod->nglobals - 1u;
        break;
      case 0x04:
        if (!add_tag(r, 1)) {
          return 0;
        }
        im->index = r->mod->ntags - 1u;
        break;
      default:
        return fail(r, GLWASM_ERR_CORRUPT);
    }
  }
  return 1;
}

static int parse_funcs(Reader * r) {
  uint32_t n;
  uint32_t i;

  if (!read_u32(r, &n)) {
    return 0;
  }
  for (i = 0; i < n; i++) {
    uint32_t ti;

    if (!read_u32(r, &ti) || !add_func_slot(r, ti, 0)) {
      return 0;
    }
  }
  return 1;
}

static int parse_tables(Reader * r) {
  uint32_t n;
  uint32_t i;

  if (!read_u32(r, &n)) {
    return 0;
  }
  for (i = 0; i < n; i++) {
    if (!add_table(r, 0)) {
      return 0;
    }
    if (r->skip_rest) {
      return 1;
    }
  }
  return 1;
}

static int parse_mems(Reader * r) {
  uint32_t n;
  uint32_t i;

  if (!read_u32(r, &n)) {
    return 0;
  }
  for (i = 0; i < n; i++) {
    if (!add_memory(r, 0)) {
      return 0;
    }
  }
  return 1;
}

static int parse_globals(Reader * r) {
  uint32_t n;
  uint32_t i;

  if (!read_u32(r, &n)) {
    return 0;
  }
  for (i = 0; i < n; i++) {
    if (!add_global(r, 0)) {
      return 0;
    }
    if (r->skip_rest) {
      return 1;
    }
  }
  return 1;
}

static int parse_exports(Reader * r) {
  uint32_t n;
  uint32_t i;

  if (!read_u32(r, &n)) {
    return 0;
  }
  if (n > r->limits->exports) {
    return fail_limit(r, "exports");
  }
  r->mod->exports = alloc_array(r, n, sizeof(GLWASM_Export),
      r->limits->exports, "exports");
  if (n != 0 && !r->mod->exports) {
    return 0;
  }
  r->mod->nexports = n;
  for (i = 0; i < n; i++) {
    GLWASM_Export * ex = &r->mod->exports[i];
    uint8_t kind;

    if (!read_name(r, &ex->name) || !read_byte(r, &kind) ||
        !read_u32(r, &ex->index)) {
      return 0;
    }
    if (kind > 4) {
      return fail(r, GLWASM_ERR_CORRUPT);
    }
    ex->kind = kind;
  }
  return 1;
}

static int parse_start(Reader * r) {
  if (!read_u32(r, &r->mod->start)) {
    return 0;
  }
  r->mod->has_start = 1;
  return 1;
}

static int parse_elems(Reader * r) {
  uint32_t n;
  uint32_t i;

  if (!read_u32(r, &n)) {
    return 0;
  }
  if (n > r->limits->element_segments) {
    return fail_limit(r, "element_segments");
  }
  r->mod->elems = alloc_array(r, n, sizeof(GLWASM_Elem),
      r->limits->element_segments, "element_segments");
  if (n != 0 && !r->mod->elems) {
    return 0;
  }
  r->mod->nelems = n;
  for (i = 0; i < n; i++) {
    GLWASM_Elem * el = &r->mod->elems[i];
    uint32_t flags;
    int active;
    int use_exprs;

    if (!read_u32(r, &flags)) {
      return 0;
    }
    if (flags > 7u) {
      return fail(r, GLWASM_ERR_CORRUPT);
    }
    active = (flags & 3u) == 0 || (flags & 3u) == 2;
    use_exprs = (flags & 4u) != 0;
    el->mode = active ? GLWASM_ELEM_ACTIVE :
        ((flags & 3u) == 1 ? GLWASM_ELEM_PASSIVE : GLWASM_ELEM_DECLARATIVE);
    el->use_exprs = (uint8_t)use_exprs;
    el->reftype.kind = GLWASM_VK_REF;
    el->reftype.nullable = 1;
    el->reftype.heap = GLWASM_HT_FUNC;
    if (active && (flags & 2u) && !read_u32(r, &el->table)) {
      return 0;
    }
    if (active && !read_expr(r, &el->offset, 1)) {
      return 0;
    }
    if (r->skip_rest) {
      return 1;
    }
    if (!use_exprs) {
      uint8_t kind;
      uint32_t count;
      uint32_t k;

      /* Flag 0 is an active segment of function indices with no elemkind. */
      if (flags != 0) {
        if (!read_byte(r, &kind)) {
          return 0;
        }
        if (kind != 0) {
          return fail(r, GLWASM_ERR_CORRUPT);
        }
      }
      if (!read_u32(r, &count)) {
        return 0;
      }
      if (count > r->limits->functions) {
        return fail_limit(r, "functions");
      }
      el->funcs = alloc_array(r, count, sizeof(uint32_t), r->limits->functions,
          "functions");
      if (count != 0 && !el->funcs) {
        return 0;
      }
      for (k = 0; k < count; k++) {
        if (!read_u32(r, &el->funcs[k])) {
          return 0;
        }
      }
      el->nfuncs = count;
    } else {
      uint32_t count;
      uint32_t k;

      if (flags == 5u || flags == 6u || flags == 7u) {
        if (!read_valtype(r, &el->reftype)) {
          return 0;
        }
        if (el->reftype.kind != GLWASM_VK_REF) {
          return fail(r, GLWASM_ERR_CORRUPT);
        }
      }
      if (!read_u32(r, &count)) {
        return 0;
      }
      if (count > r->limits->functions) {
        return fail_limit(r, "functions");
      }
      el->exprs = alloc_array(r, count, sizeof(GLWASM_Func),
          r->limits->functions, "functions");
      if (count != 0 && !el->exprs) {
        return 0;
      }
      el->nexprs = count;
      for (k = 0; k < count; k++) {
        if (!read_expr(r, &el->exprs[k], 1)) {
          return 0;
        }
        if (r->skip_rest) {
          return 1;
        }
      }
    }
  }
  return 1;
}

static int parse_codes(Reader * r) {
  uint32_t n;
  uint32_t i;
  uint32_t defined;

  if (!read_u32(r, &n)) {
    return 0;
  }
  defined = r->mod->nfuncs - r->mod->nimported_funcs;
  if (n != defined) {
    return fail(r, GLWASM_ERR_CORRUPT);
  }
  for (i = 0; i < n; i++) {
    uint32_t size;
    const uint8_t * body_end;
    GLWASM_Func * fn = &r->mod->funcs[r->mod->nimported_funcs + i];

    if (!read_u32(r, &size)) {
      return 0;
    }
    if ((size_t)(r->end - r->p) < size) {
      return fail(r, GLWASM_ERR_CORRUPT);
    }
    body_end = r->p + size;
    if (!read_code_entry(r, fn, body_end)) {
      return 0;
    }
  }
  return 1;
}

static int parse_datas(Reader * r) {
  uint32_t n;
  uint32_t i;

  if (!read_u32(r, &n)) {
    return 0;
  }
  if (r->mod->has_data_count && n != r->mod->data_count) {
    return fail(r, GLWASM_ERR_CORRUPT);
  }
  if (n > r->limits->data_segments) {
    return fail_limit(r, "data_segments");
  }
  r->mod->datas = alloc_array(r, n, sizeof(GLWASM_Data),
      r->limits->data_segments, "data_segments");
  if (n != 0 && !r->mod->datas) {
    return 0;
  }
  r->mod->ndatas = n;
  for (i = 0; i < n; i++) {
    GLWASM_Data * d = &r->mod->datas[i];
    uint32_t flags;
    uint32_t nbytes;

    if (!read_u32(r, &flags)) {
      return 0;
    }
    if (flags > 2u) {
      return fail(r, GLWASM_ERR_CORRUPT);
    }
    d->passive = (uint8_t)(flags == 1u);
    if (flags == 2u && !read_u32(r, &d->mem)) {
      return 0;
    }
    if (flags != 1u && !read_expr(r, &d->offset, 1)) {
      return 0;
    }
    if (r->skip_rest) {
      return 1;
    }
    if (!read_u32(r, &nbytes)) {
      return 0;
    }
    if ((size_t)(r->end - r->p) < nbytes) {
      return fail(r, GLWASM_ERR_CORRUPT);
    }
    if (nbytes != 0) {
      d->bytes = alloc_bytes(r, nbytes);
      if (!d->bytes) {
        return 0;
      }
      memcpy(d->bytes, r->p, nbytes);
      r->p += nbytes;
    }
    d->nbytes = nbytes;
  }
  return 1;
}

static int parse_data_count(Reader * r) {
  uint32_t n;

  if (!read_u32(r, &n)) {
    return 0;
  }
  /* The data section comes later. The counts are compared once it is read,
   * or at the end of the module when the section is absent (an empty list). */
  r->mod->has_data_count = 1;
  r->mod->data_count = n;
  return 1;
}

static int parse_tags(Reader * r) {
  uint32_t n;
  uint32_t i;

  if (!read_u32(r, &n)) {
    return 0;
  }
  for (i = 0; i < n; i++) {
    if (!add_tag(r, 0)) {
      return 0;
    }
  }
  return 1;
}

static int parse_custom(Reader * r) {
  GLWASM_Name name;

  memset(&name, 0, sizeof(name));
  if (!read_name(r, &name)) {
    return 0;
  }
  mod_free(r->mod, name.bytes);
  r->p = r->end;
  return 1;
}

static int with_section(Reader * r, uint32_t size, int (*fn)(Reader *)) {
  const uint8_t * saved = r->end;
  const uint8_t * sec_end;
  int ok;

  if ((size_t)(r->end - r->p) < size) {
    return fail(r, GLWASM_ERR_CORRUPT);
  }
  sec_end = r->p + size;
  r->end = sec_end;
  ok = fn(r);
  if (r->skip_rest) {
    r->p = sec_end;
    r->skip_rest = 0;
    if (r->err == GLWASM_OK || r->deferred) {
      ok = 1;
    }
  }
  if (ok && r->p != sec_end) {
    ok = fail(r, GLWASM_ERR_CORRUPT);
  }
  r->end = saved;
  return ok;
}

static int parse_module(Reader * r) {
  uint8_t magic[4];
  uint8_t version[4];
  int last_rank = 0;
  int i;

  for (i = 0; i < 4; i++) {
    if (!read_byte(r, &magic[i])) {
      return 0;
    }
  }
  if (magic[0] != 0x00 || magic[1] != 0x61 || magic[2] != 0x73 ||
      magic[3] != 0x6d) {
    return fail(r, GLWASM_ERR_CORRUPT);
  }
  for (i = 0; i < 4; i++) {
    if (!read_byte(r, &version[i])) {
      return 0;
    }
  }
  if (version[0] != 0x01 || version[1] != 0 || version[2] != 0 ||
      version[3] != 0) {
    return fail(r, GLWASM_ERR_CORRUPT);
  }
  while (r->p < r->end) {
    uint8_t id;
    uint32_t size;
    int (*fn)(Reader *) = NULL;

    if (!read_byte(r, &id) || !read_u32(r, &size)) {
      return 0;
    }
    if (id != 0) {
      /* Ids are not the file order: the tag section (13) sits between
       * memory and global, and the data count section (12) sits between
       * element and code. */
      static const int k_rank[14] = {
        0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13, 11, 6
      };
      int rank;

      if (id > 13) {
        return fail(r, GLWASM_ERR_CORRUPT);
      }
      rank = k_rank[id];
      if (rank <= last_rank) {
        return fail(r, GLWASM_ERR_CORRUPT);
      }
      last_rank = rank;
    }
    switch (id) {
      case 0:
        fn = parse_custom;
        break;
      case 1:
        fn = parse_types;
        break;
      case 2:
        fn = parse_imports;
        break;
      case 3:
        fn = parse_funcs;
        break;
      case 4:
        fn = parse_tables;
        break;
      case 5:
        fn = parse_mems;
        break;
      case 6:
        fn = parse_globals;
        break;
      case 7:
        fn = parse_exports;
        break;
      case 8:
        fn = parse_start;
        break;
      case 9:
        fn = parse_elems;
        break;
      case 10:
        fn = parse_codes;
        break;
      case 11:
        fn = parse_datas;
        break;
      case 12:
        fn = parse_data_count;
        break;
      case 13:
        fn = parse_tags;
        break;
      default:
        return fail(r, GLWASM_ERR_CORRUPT);
    }
    if (!with_section(r, size, fn)) {
      return 0;
    }
  }
  if (r->mod->has_data_count && r->mod->ndatas != r->mod->data_count) {
    return fail(r, GLWASM_ERR_CORRUPT);
  }
  if (r->mod->nfuncs > r->mod->nimported_funcs) {
    uint32_t f;

    for (f = r->mod->nimported_funcs; f < r->mod->nfuncs; f++) {
      if (r->mod->funcs[f].nins == 0 && !r->deferred) {
        return fail(r, GLWASM_ERR_CORRUPT);
      }
    }
  }
  return 1;
}

static void write_failure(GLWASM_Failure * failure, GLWASM_Result result,
    const char * limit) {
  if (!failure) {
    return;
  }
  failure->result = result;
  failure->limit = result == GLWASM_ERR_LIMIT ? limit : NULL;
}

GLWASM_Result glwasm_module_decode(
    const uint8_t * bytes,
    size_t length,
    const GLWASM_Limits * limits,
    const GLWASM_Allocator * allocator,
    GLWASM_Failure * failure,
    GLWASM_Module * * out_module) {
  GLWASM_Limits defaults;
  Reader r;
  GLWASM_Module * module;
  const GLWASM_Allocator * alloc;

  if (!out_module || (!bytes && length != 0)) {
    write_failure(failure, GLWASM_ERR_INVALID, NULL);
    return GLWASM_ERR_INVALID;
  }
  if (!limits) {
    glwasm_limits_default(&defaults);
    limits = &defaults;
  }
  if ((uint64_t)length > limits->module_bytes) {
    write_failure(failure, GLWASM_ERR_LIMIT, "module_bytes");
    return GLWASM_ERR_LIMIT;
  }
  alloc = allocator ? allocator : glwasm_allocator_default();
  module = gcu_allocator_calloc(alloc, 1, sizeof(*module));
  if (!module) {
    write_failure(failure, GLWASM_ERR_OOM, NULL);
    return GLWASM_ERR_OOM;
  }
  module->alloc = *alloc;
  module->limits = *limits;
  memset(&r, 0, sizeof(r));
  r.p = bytes;
  r.end = bytes + length;
  r.limits = limits;
  r.alloc = alloc;
  r.mod = module;
  r.err = GLWASM_OK;
  if (length < 8 || !parse_module(&r)) {
    GLWASM_Result err = r.err == GLWASM_OK ? GLWASM_ERR_CORRUPT : r.err;

    glwasm_module_destroy(module);
    write_failure(failure, err, r.limit);
    return err;
  }
  if (r.deferred) {
    glwasm_module_destroy(module);
    write_failure(failure, GLWASM_ERR_UNSUPPORTED, NULL);
    return GLWASM_ERR_UNSUPPORTED;
  }
  {
    GLWASM_Result verr = GLWASM_OK;
    const char * vlimit = NULL;

    if (!glwasm_module_validate(module, &verr, &vlimit)) {
      glwasm_module_destroy(module);
      write_failure(failure, verr, vlimit);
      return verr;
    }
  }
  *out_module = module;
  write_failure(failure, GLWASM_OK, NULL);
  return GLWASM_OK;
}

GLWASM_Result glwasm_module_dump(const GLWASM_Module * module, FILE * out) {
  if (!module || !out) {
    return GLWASM_ERR_INVALID;
  }
  fprintf(out,
      "module types=%u funcs=%u tables=%u mems=%u globals=%u tags=%u\n"
      "  imports=%u exports=%u elems=%u datas=%u\n",
      module->ntypes, module->nfuncs, module->ntables, module->nmems,
      module->nglobals, module->ntags, module->nimports, module->nexports,
      module->nelems, module->ndatas);
  return GLWASM_OK;
}
