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
 * The second pass. The operand stack holds value-type descriptors, including
 * a v128 kind and a reference that names a type index, so nothing here
 * assumes a value is at most 64 bits. Control is an explicit stack: block
 * nesting does not recurse in C.
 */

#include <ghoti.io/lang-wasm/macros.h>

#include <ghoti.io/cutil/allocator.h>

#include "../module_internal.h"

#include <string.h>

enum {
  GLWASM_V_FUNC = 1,
  GLWASM_V_BLOCK,
  GLWASM_V_LOOP,
  GLWASM_V_IF,
  GLWASM_V_TRY
};

typedef struct VFrame {
  uint8_t kind;
  uint8_t unreachable;
  uint8_t else_seen;
  uint32_t height;
  const GLWASM_ValType * params;
  uint32_t nparams;
  const GLWASM_ValType * results;
  uint32_t nresults;
  uint8_t * local_init_snapshot;
  uint8_t * then_local_init_snapshot;
} VFrame;

typedef struct VState {
  GLWASM_Module * mod;
  GLWASM_Result err;
  const char * limit;
  int is_const;
  /* A constant expression may read global.get x only for x below this. */
  uint32_t const_globals;
  uint8_t * reffed;
  GLWASM_ValType * stack;
  uint32_t nstack;
  uint32_t scap;
  VFrame * frames;
  uint32_t nframes;
  uint32_t fcap;
  uint8_t * local_initialized;
  uint32_t nlocal_initialized;
  GLWASM_Func * function;
  uint32_t max_stack;
  uint32_t max_control_depth;
} VState;

static GLWASM_ValType num_type(uint8_t kind) {
  GLWASM_ValType t;

  memset(&t, 0, sizeof(t));
  t.kind = kind;
  return t;
}

static GLWASM_ValType ref_type(uint8_t heap, int nullable, uint32_t index) {
  GLWASM_ValType t;

  memset(&t, 0, sizeof(t));
  t.kind = GLWASM_VK_REF;
  t.nullable = (uint8_t)nullable;
  t.heap = heap;
  t.index = index;
  return t;
}

static int fail(VState * v, GLWASM_Result err) {
  v->err = err;
  return 0;
}

static int fail_limit(VState * v, const char * field) {
  v->err = GLWASM_ERR_LIMIT;
  v->limit = field;
  return 0;
}

static int val_equal(GLWASM_ValType a, GLWASM_ValType b) {
  if (a.kind != b.kind || a.mut != b.mut) {
    return 0;
  }
  if (a.kind != GLWASM_VK_REF) {
    return 1;
  }
  if (a.nullable != b.nullable || a.heap != b.heap) {
    return 0;
  }
  return a.heap != GLWASM_HT_INDEX || a.index == b.index;
}

static int declared_sub(const GLWASM_Module * m, uint32_t child,
    uint32_t parent, int depth);
static int fields_compatible(const GLWASM_Module * m, uint32_t child,
    uint32_t parent);

static int val_sub(const GLWASM_Module * m, GLWASM_ValType actual,
    GLWASM_ValType expect);

static int field_sub(const GLWASM_Module * m, GLWASM_ValType child,
    GLWASM_ValType parent) {
  if (child.mut || parent.mut) {
    return val_equal(child, parent);
  }
  return val_sub(m, child, parent);
}

static int fields_compatible(const GLWASM_Module * m, uint32_t child,
    uint32_t parent) {
  const GLWASM_TypeDef * c;
  const GLWASM_TypeDef * p;
  uint32_t i;

  if (child >= m->ntypes || parent >= m->ntypes) {
    return 0;
  }
  c = &m->types[child];
  p = &m->types[parent];
  if (p->comp == GLWASM_COMP_STRUCT && c->comp == GLWASM_COMP_STRUCT) {
    if (c->nparams < p->nparams) {
      return 0;
    }
    for (i = 0; i < p->nparams; i++) {
      if (!field_sub(m, c->params[i], p->params[i])) {
        return 0;
      }
    }
    return 1;
  }
  if (p->comp == GLWASM_COMP_ARRAY && c->comp == GLWASM_COMP_ARRAY) {
    if (c->nparams < 1 || p->nparams < 1) {
      return 0;
    }
    return field_sub(m, c->params[0], p->params[0]);
  }
  return 1;
}

static int declared_sub(const GLWASM_Module * m, uint32_t child,
    uint32_t parent, int depth) {
  const GLWASM_TypeDef * td;
  uint32_t i;

  if (child == parent) {
    return 1;
  }
  if (depth > 32 || child >= m->ntypes) {
    return 0;
  }
  td = &m->types[child];
  for (i = 0; i < td->nsupers; i++) {
    uint32_t s = td->supers[i];

    if (s == parent) {
      return fields_compatible(m, child, parent);
    }
    if (declared_sub(m, s, parent, depth + 1)) {
      return 1;
    }
  }
  return 0;
}

static int type_comp(const GLWASM_Module * m, uint32_t idx, uint8_t comp) {
  return idx < m->ntypes && m->types[idx].comp == comp;
}

static int abstract_sub(uint8_t actual, uint8_t expect) {
  if (actual == expect) {
    return 1;
  }
  switch (actual) {
    case GLWASM_HT_NONE:
      return expect == GLWASM_HT_I31 || expect == GLWASM_HT_STRUCT ||
          expect == GLWASM_HT_ARRAY || expect == GLWASM_HT_EQ ||
          expect == GLWASM_HT_ANY;
    case GLWASM_HT_NOFUNC:
      return expect == GLWASM_HT_FUNC;
    case GLWASM_HT_NOEXTERN:
      return expect == GLWASM_HT_EXTERN;
    case GLWASM_HT_NOEXN:
      return expect == GLWASM_HT_EXN;
    case GLWASM_HT_I31:
    case GLWASM_HT_STRUCT:
    case GLWASM_HT_ARRAY:
      return expect == GLWASM_HT_EQ || expect == GLWASM_HT_ANY;
    case GLWASM_HT_EQ:
      return expect == GLWASM_HT_ANY;
    case GLWASM_HT_FUNC:
    case GLWASM_HT_EXTERN:
    case GLWASM_HT_EXN:
      /* The func, extern and exn hierarchies are separate from any. */
      return 0;
    default:
      return 0;
  }
}

static int heap_sub(const GLWASM_Module * m, GLWASM_ValType actual,
    GLWASM_ValType expect) {
  if (actual.heap == GLWASM_HT_INDEX && expect.heap == GLWASM_HT_INDEX) {
    return declared_sub(m, actual.index, expect.index, 0);
  }
  if (actual.heap == GLWASM_HT_INDEX) {
    uint8_t comp = actual.index < m->ntypes ? m->types[actual.index].comp : 0;

    if (expect.heap == GLWASM_HT_ANY) {
      return comp == GLWASM_COMP_STRUCT || comp == GLWASM_COMP_ARRAY;
    }
    if (expect.heap == GLWASM_HT_FUNC) {
      return comp == GLWASM_COMP_FUNC;
    }
    if (expect.heap == GLWASM_HT_EQ) {
      return comp == GLWASM_COMP_STRUCT || comp == GLWASM_COMP_ARRAY;
    }
    if (expect.heap == GLWASM_HT_STRUCT) {
      return comp == GLWASM_COMP_STRUCT;
    }
    if (expect.heap == GLWASM_HT_ARRAY) {
      return comp == GLWASM_COMP_ARRAY;
    }
    return 0;
  }
  if (expect.heap == GLWASM_HT_INDEX) {
    if (actual.heap == GLWASM_HT_NOFUNC) {
      return type_comp(m, expect.index, GLWASM_COMP_FUNC);
    }
    if (actual.heap == GLWASM_HT_NONE) {
      return type_comp(m, expect.index, GLWASM_COMP_STRUCT) ||
          type_comp(m, expect.index, GLWASM_COMP_ARRAY);
    }
    return 0;
  }
  return abstract_sub(actual.heap, expect.heap);
}

static int val_sub(const GLWASM_Module * m, GLWASM_ValType actual,
    GLWASM_ValType expect) {
  if (actual.kind == GLWASM_VK_BOT || expect.kind == GLWASM_VK_BOT) {
    return 1;
  }
  if (actual.kind != expect.kind) {
    return 0;
  }
  if (actual.kind != GLWASM_VK_REF) {
    return 1;
  }
  if (!expect.nullable && actual.nullable) {
    return 0;
  }
  return heap_sub(m, actual, expect);
}

static int grow(VState * v, void * old, void * * out, uint32_t * cap,
    uint32_t need, uint32_t limit, size_t elem, const char * field) {
  uint32_t ncap;
  void * p;

  if (need <= *cap) {
    *out = old;
    return 1;
  }
  if (need > limit) {
    return fail_limit(v, field);
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
    return fail_limit(v, field);
  }
  p = gcu_allocator_realloc(&v->mod->alloc, old, (size_t)ncap * elem);
  if (!p) {
    return fail(v, GLWASM_ERR_OOM);
  }
  *out = p;
  *cap = ncap;
  return 1;
}

static VFrame * top(VState * v) {
  if (v->nframes == 0) {
    return NULL;
  }
  return &v->frames[v->nframes - 1u];
}

static int push_val(VState * v, GLWASM_ValType t) {
  void * grown = v->stack;

  if (!grow(v, grown, &grown, &v->scap, v->nstack + 1u,
      v->mod->limits.instructions, sizeof(GLWASM_ValType), "instructions")) {
    return 0;
  }
  v->stack = grown;
  v->stack[v->nstack++] = t;
  if (!v->is_const && v->function && v->nstack > v->max_stack)
    v->max_stack = v->nstack;
  return 1;
}

static int pop_val(VState * v, GLWASM_ValType expect, GLWASM_ValType * got) {
  VFrame * f = top(v);
  GLWASM_ValType actual;

  if (!f) {
    return fail(v, GLWASM_ERR_VALIDATE);
  }
  if (v->nstack == f->height) {
    if (!f->unreachable) {
      return fail(v, GLWASM_ERR_VALIDATE);
    }
    if (got) {
      *got = num_type(GLWASM_VK_BOT);
    }
    return 1;
  }
  actual = v->stack[--v->nstack];
  if (!val_sub(v->mod, actual, expect)) {
    return fail(v, GLWASM_ERR_VALIDATE);
  }
  if (got) {
    *got = actual.kind == GLWASM_VK_BOT ? expect : actual;
  }
  return 1;
}

static int pop_any(VState * v, GLWASM_ValType * got) {
  VFrame * f = top(v);

  if (!f) {
    return fail(v, GLWASM_ERR_VALIDATE);
  }
  if (v->nstack == f->height) {
    if (!f->unreachable) {
      return fail(v, GLWASM_ERR_VALIDATE);
    }
    *got = num_type(GLWASM_VK_BOT);
    return 1;
  }
  *got = v->stack[--v->nstack];
  return 1;
}

static int pop_vals(VState * v, const GLWASM_ValType * types, uint32_t n) {
  uint32_t i;

  for (i = n; i > 0; i--) {
    if (!pop_val(v, types[i - 1u], NULL)) {
      return 0;
    }
  }
  return 1;
}

static int push_vals(VState * v, const GLWASM_ValType * types, uint32_t n) {
  uint32_t i;

  for (i = 0; i < n; i++) {
    if (!push_val(v, types[i])) {
      return 0;
    }
  }
  return 1;
}

static int push_frame(VState * v, VFrame frame) {
  void * grown = v->frames;

  if (!grow(v, grown, &grown, &v->fcap, v->nframes + 1u,
      v->mod->limits.nesting, sizeof(VFrame), "nesting")) {
    return 0;
  }
  v->frames = grown;
  v->frames[v->nframes++] = frame;
  if (!v->is_const && v->function &&
      v->nframes - 1u > v->max_control_depth)
    v->max_control_depth = v->nframes - 1u;
  return 1;
}

static void mark_unreachable(VState * v) {
  VFrame * f = top(v);

  if (!f) {
    return;
  }
  v->nstack = f->height;
  f->unreachable = 1;
}

static int same_seq(const GLWASM_ValType * a, uint32_t na,
    const GLWASM_ValType * b, uint32_t nb) {
  uint32_t i;

  if (na != nb) {
    return 0;
  }
  for (i = 0; i < na; i++) {
    if (!val_equal(a[i], b[i])) {
      return 0;
    }
  }
  return 1;
}

static const GLWASM_ValType * label_types(const VFrame * f, uint32_t * n) {
  if (f->kind == GLWASM_V_LOOP) {
    *n = f->nparams;
    return f->params;
  }
  *n = f->nresults;
  return f->results;
}

static VFrame * label_frame(VState * v, uint32_t label) {
  if (label >= v->nframes) {
    return NULL;
  }
  return &v->frames[v->nframes - 1u - label];
}

static int enter_block(VState * v, uint8_t kind, const GLWASM_BlockType * bt,
    GLWASM_Func * fn) {
  VFrame frame;
  const GLWASM_ValType * params;
  const GLWASM_ValType * results;

  if (!bt->resolved) {
    return fail(v, GLWASM_ERR_VALIDATE);
  }
  params = bt->nparams ? fn->vpool + bt->param_at : NULL;
  results = bt->nresults ? fn->vpool + bt->result_at : NULL;
  if (!pop_vals(v, params, bt->nparams)) {
    return 0;
  }
  memset(&frame, 0, sizeof(frame));
  frame.kind = kind;
  frame.height = v->nstack;
  frame.params = params;
  frame.nparams = bt->nparams;
  frame.results = results;
  frame.nresults = bt->nresults;
  if (v->local_initialized && v->nlocal_initialized) {
    frame.local_init_snapshot =
        gcu_allocator_malloc(&v->mod->alloc, v->nlocal_initialized);
    if (!frame.local_init_snapshot)
      return fail(v, GLWASM_ERR_OOM);
    memcpy(frame.local_init_snapshot, v->local_initialized,
           v->nlocal_initialized);
    if (kind == GLWASM_V_IF) {
      frame.then_local_init_snapshot =
          gcu_allocator_malloc(&v->mod->alloc, v->nlocal_initialized);
      if (!frame.then_local_init_snapshot) {
        gcu_allocator_free(&v->mod->alloc, frame.local_init_snapshot);
        return fail(v, GLWASM_ERR_OOM);
      }
    }
  }
  if (!push_frame(v, frame)) {
    gcu_allocator_free(&v->mod->alloc, frame.local_init_snapshot);
    gcu_allocator_free(&v->mod->alloc, frame.then_local_init_snapshot);
    return 0;
  }
  return push_vals(v, params, bt->nparams);
}

static int func_type(const GLWASM_Module *m, uint32_t index,
                     const GLWASM_TypeDef **out) {
  if (index >= m->ntypes || m->types[index].comp != GLWASM_COMP_FUNC) {
    return 0;
  }
  *out = &m->types[index];
  return 1;
}

static GLWASM_ValType addr_type(int addr64) {
  return num_type(addr64 ? GLWASM_VK_I64 : GLWASM_VK_I32);
}

static int mem_at(VState * v, uint32_t index, const GLWASM_Memory * * out) {
  if (index >= v->mod->nmems) {
    return fail(v, GLWASM_ERR_VALIDATE);
  }
  *out = &v->mod->mems[index];
  return 1;
}

static int table_at(VState * v, uint32_t index, const GLWASM_Table * * out) {
  if (index >= v->mod->ntables) {
    return fail(v, GLWASM_ERR_VALIDATE);
  }
  *out = &v->mod->tables[index];
  return 1;
}

static int check_align(VState * v, uint32_t align, uint32_t natural) {
  if (align > natural) {
    return fail(v, GLWASM_ERR_VALIDATE);
  }
  return 1;
}

static int validate_load_store(VState * v, const GLWASM_Instr * ins) {
  static const uint8_t k_kind[] = {
    GLWASM_VK_I32, GLWASM_VK_I64, GLWASM_VK_F32, GLWASM_VK_F64,
    GLWASM_VK_I32, GLWASM_VK_I32, GLWASM_VK_I32, GLWASM_VK_I32,
    GLWASM_VK_I64, GLWASM_VK_I64, GLWASM_VK_I64, GLWASM_VK_I64,
    GLWASM_VK_I64, GLWASM_VK_I64,
    GLWASM_VK_I32, GLWASM_VK_I64, GLWASM_VK_F32, GLWASM_VK_F64,
    GLWASM_VK_I32, GLWASM_VK_I32, GLWASM_VK_I64, GLWASM_VK_I64, GLWASM_VK_I64
  };
  static const uint8_t k_align[] = {
    2, 3, 2, 3, 0, 0, 1, 1, 0, 0, 1, 1, 2, 2,
    2, 3, 2, 3, 0, 1, 0, 1, 2
  };
  uint16_t op = ins->op;
  const GLWASM_Memory * mem;
  GLWASM_ValType value;
  int store;

  if (op < 0x28 || op > 0x3e) {
    return fail(v, GLWASM_ERR_VALIDATE);
  }
  if (!mem_at(v, ins->b, &mem) || !check_align(v, ins->a, k_align[op - 0x28])) {
    return 0;
  }
  if (!mem->addr64 && ins->c > UINT32_MAX) {
    return fail(v, GLWASM_ERR_VALIDATE);
  }
  value = num_type(k_kind[op - 0x28]);
  store = op >= 0x36;
  if (store && !pop_val(v, value, NULL)) {
    return 0;
  }
  if (!pop_val(v, addr_type(mem->addr64), NULL)) {
    return 0;
  }
  if (!store && !push_val(v, value)) {
    return 0;
  }
  return 1;
}

static int validate_numeric(VState * v, uint8_t op) {
  GLWASM_ValType in;
  GLWASM_ValType out;
  int binary = 0;

  if (op == 0x45) {
    return pop_val(v, num_type(GLWASM_VK_I32), NULL) &&
        push_val(v, num_type(GLWASM_VK_I32));
  }
  if (op >= 0x46 && op <= 0x4f) {
    in = num_type(GLWASM_VK_I32);
    binary = 1;
    out = in;
  } else if (op == 0x50) {
    return pop_val(v, num_type(GLWASM_VK_I64), NULL) &&
        push_val(v, num_type(GLWASM_VK_I32));
  } else if (op >= 0x51 && op <= 0x5a) {
    in = num_type(GLWASM_VK_I64);
    binary = 1;
    out = num_type(GLWASM_VK_I32);
  } else if (op >= 0x5b && op <= 0x60) {
    in = num_type(GLWASM_VK_F32);
    binary = 1;
    out = num_type(GLWASM_VK_I32);
  } else if (op >= 0x61 && op <= 0x66) {
    in = num_type(GLWASM_VK_F64);
    binary = 1;
    out = num_type(GLWASM_VK_I32);
  } else if (op >= 0x67 && op <= 0x69) {
    in = num_type(GLWASM_VK_I32);
    out = in;
  } else if (op >= 0x6a && op <= 0x78) {
    in = num_type(GLWASM_VK_I32);
    binary = 1;
    out = in;
  } else if (op >= 0x79 && op <= 0x7b) {
    in = num_type(GLWASM_VK_I64);
    out = in;
  } else if (op >= 0x7c && op <= 0x8a) {
    in = num_type(GLWASM_VK_I64);
    binary = 1;
    out = in;
  } else if (op >= 0x8b && op <= 0x91) {
    in = num_type(GLWASM_VK_F32);
    out = in;
  } else if (op >= 0x92 && op <= 0x98) {
    in = num_type(GLWASM_VK_F32);
    binary = 1;
    out = in;
  } else if (op >= 0x99 && op <= 0x9f) {
    in = num_type(GLWASM_VK_F64);
    out = in;
  } else if (op >= 0xa0 && op <= 0xa6) {
    in = num_type(GLWASM_VK_F64);
    binary = 1;
    out = in;
  } else if (op >= 0xa7 && op <= 0xbf) {
    static const uint8_t k_in[] = {
      GLWASM_VK_I64, GLWASM_VK_F32, GLWASM_VK_F32, GLWASM_VK_F64, GLWASM_VK_F64,
      GLWASM_VK_I32, GLWASM_VK_I32, GLWASM_VK_F32, GLWASM_VK_F32, GLWASM_VK_F64,
      GLWASM_VK_F64, GLWASM_VK_I32, GLWASM_VK_I32, GLWASM_VK_I64, GLWASM_VK_I64,
      GLWASM_VK_F64, GLWASM_VK_I32, GLWASM_VK_I32, GLWASM_VK_I64, GLWASM_VK_I64,
      GLWASM_VK_F32, GLWASM_VK_F32, GLWASM_VK_F64, GLWASM_VK_I32, GLWASM_VK_I64
    };
    static const uint8_t k_out[] = {
      GLWASM_VK_I32, GLWASM_VK_I32, GLWASM_VK_I32, GLWASM_VK_I32, GLWASM_VK_I32,
      GLWASM_VK_I64, GLWASM_VK_I64, GLWASM_VK_I64, GLWASM_VK_I64, GLWASM_VK_I64,
      GLWASM_VK_I64, GLWASM_VK_F32, GLWASM_VK_F32, GLWASM_VK_F32, GLWASM_VK_F32,
      GLWASM_VK_F32, GLWASM_VK_F64, GLWASM_VK_F64, GLWASM_VK_F64, GLWASM_VK_F64,
      GLWASM_VK_F64, GLWASM_VK_I32, GLWASM_VK_I64, GLWASM_VK_F32, GLWASM_VK_F64
    };

    return pop_val(v, num_type(k_in[op - 0xa7]), NULL) &&
        push_val(v, num_type(k_out[op - 0xa7]));
  } else if (op >= 0xc0 && op <= 0xc1) {
    in = num_type(GLWASM_VK_I32);
    out = in;
  } else if (op >= 0xc2 && op <= 0xc4) {
    in = num_type(GLWASM_VK_I64);
    out = in;
  } else {
    return fail(v, GLWASM_ERR_VALIDATE);
  }
  if (binary && !pop_val(v, in, NULL)) {
    return 0;
  }
  if (!pop_val(v, in, NULL) || !push_val(v, out)) {
    return 0;
  }
  return 1;
}

static int validate_call(VState * v, uint32_t type_index, int tail) {
  const GLWASM_TypeDef * td;
  VFrame * func;

  if (!func_type(v->mod, type_index, &td)) {
    return fail(v, GLWASM_ERR_VALIDATE);
  }
  if (!pop_vals(v, td->params, td->nparams)) {
    return 0;
  }
  if (tail) {
    func = v->nframes ? &v->frames[0] : NULL;
    if (!func || !same_seq(td->results, td->nresults, func->results,
        func->nresults)) {
      return fail(v, GLWASM_ERR_VALIDATE);
    }
    mark_unreachable(v);
    return 1;
  }
  return push_vals(v, td->results, td->nresults);
}

static int validate_indirect(VState * v, const GLWASM_Instr * ins, int tail) {
  const GLWASM_Table * table;
  const GLWASM_TypeDef * td;
  GLWASM_ValType ref;

  if (!func_type(v->mod, ins->a, &td) || !table_at(v, ins->b, &table)) {
    return 0;
  }
  ref = ref_type(GLWASM_HT_INDEX, 1, ins->a);
  if (!val_sub(v->mod, ref, table->elem)) {
    return fail(v, GLWASM_ERR_VALIDATE);
  }
  if (!pop_val(v, addr_type(table->addr64), NULL)) {
    return 0;
  }
  return validate_call(v, ins->a, tail);
}

static int validate_fc(VState * v, const GLWASM_Instr * ins) {
  uint32_t sub = (uint32_t)(ins->op & 0xffu);
  static const uint8_t k_in[] = {
    GLWASM_VK_F32, GLWASM_VK_F32, GLWASM_VK_F64, GLWASM_VK_F64,
    GLWASM_VK_F32, GLWASM_VK_F32, GLWASM_VK_F64, GLWASM_VK_F64
  };
  static const uint8_t k_out[] = {
    GLWASM_VK_I32, GLWASM_VK_I32, GLWASM_VK_I32, GLWASM_VK_I32,
    GLWASM_VK_I64, GLWASM_VK_I64, GLWASM_VK_I64, GLWASM_VK_I64
  };

  if (sub <= 7u) {
    return pop_val(v, num_type(k_in[sub]), NULL) &&
        push_val(v, num_type(k_out[sub]));
  }
  if (sub == 8u || sub == 9u) {
    const GLWASM_Memory * mem;

    if (!v->mod->has_data_count || ins->a >= v->mod->ndatas) {
      return fail(v, GLWASM_ERR_VALIDATE);
    }
    if (sub == 9u) {
      return 1;
    }
    if (!mem_at(v, ins->b, &mem)) {
      return 0;
    }
    if (!pop_val(v, num_type(GLWASM_VK_I32), NULL) ||
        !pop_val(v, num_type(GLWASM_VK_I32), NULL) ||
        !pop_val(v, addr_type(mem->addr64), NULL)) {
      return 0;
    }
    return 1;
  }
  if (sub == 10u) {
    const GLWASM_Memory * dst;
    const GLWASM_Memory * src;

    if (!mem_at(v, ins->a, &dst) || !mem_at(v, ins->b, &src)) {
      return 0;
    }
    /* The destination and source may differ in address type; the length
     * has the narrower one (i32 when either is i32). */
    if (!pop_val(v, addr_type(dst->addr64 && src->addr64), NULL) ||
        !pop_val(v, addr_type(src->addr64), NULL) ||
        !pop_val(v, addr_type(dst->addr64), NULL)) {
      return 0;
    }
    return 1;
  }
  if (sub == 11u) {
    const GLWASM_Memory * mem;

    if (!mem_at(v, ins->a, &mem)) {
      return 0;
    }
    if (!pop_val(v, addr_type(mem->addr64), NULL) ||
        !pop_val(v, num_type(GLWASM_VK_I32), NULL) ||
        !pop_val(v, addr_type(mem->addr64), NULL)) {
      return 0;
    }
    return 1;
  }
  if (sub == 12u || sub == 13u) {
    const GLWASM_Table * table;
    const GLWASM_Elem * el;

    if (ins->a >= v->mod->nelems) {
      return fail(v, GLWASM_ERR_VALIDATE);
    }
    if (sub == 13u) {
      return 1;
    }
    el = &v->mod->elems[ins->a];
    if (!table_at(v, ins->b, &table) || !val_sub(v->mod, el->reftype,
        table->elem)) {
      return 0;
    }
    if (!pop_val(v, num_type(GLWASM_VK_I32), NULL) ||
        !pop_val(v, num_type(GLWASM_VK_I32), NULL) ||
        !pop_val(v, addr_type(table->addr64), NULL)) {
      return 0;
    }
    return 1;
  }
  if (sub == 14u) {
    const GLWASM_Table * dst;
    const GLWASM_Table * src;

    if (!table_at(v, ins->a, &dst) || !table_at(v, ins->b, &src)) {
      return 0;
    }
    /* The source element type must be a subtype of the destination's, and
     * the address types may differ as for memory.copy. */
    if (!val_sub(v->mod, src->elem, dst->elem)) {
      return fail(v, GLWASM_ERR_VALIDATE);
    }
    if (!pop_val(v, addr_type(dst->addr64 && src->addr64), NULL) ||
        !pop_val(v, addr_type(src->addr64), NULL) ||
        !pop_val(v, addr_type(dst->addr64), NULL)) {
      return 0;
    }
    return 1;
  }
  if (sub == 15u) {
    const GLWASM_Table * table;

    if (!table_at(v, ins->a, &table)) {
      return 0;
    }
    if (!pop_val(v, addr_type(table->addr64), NULL) ||
        !pop_val(v, table->elem, NULL)) {
      return 0;
    }
    return push_val(v, addr_type(table->addr64));
  }
  if (sub == 16u) {
    const GLWASM_Table * table;

    if (!table_at(v, ins->a, &table)) {
      return 0;
    }
    return push_val(v, addr_type(table->addr64));
  }
  if (sub == 17u) {
    const GLWASM_Table * table;

    if (!table_at(v, ins->a, &table)) {
      return 0;
    }
    if (!pop_val(v, addr_type(table->addr64), NULL) ||
        !pop_val(v, table->elem, NULL) ||
        !pop_val(v, addr_type(table->addr64), NULL)) {
      return 0;
    }
    return 1;
  }
  return fail(v, GLWASM_ERR_VALIDATE);
}

static uint32_t fn_type_index(VState * v, uint32_t func_index);

static int validate_instr(VState * v, GLWASM_Func * fn, const GLWASM_Instr * ins) {
  if ((ins->op & 0xff00u) == 0xfc00u) {
    return validate_fc(v, ins);
  }
  switch (ins->op) {
    case 0x00:
      mark_unreachable(v);
      return 1;
    case 0x01:
      return 1;
    case 0x02:
    case 0x03:
    case 0x04: {
      uint8_t kind = ins->op == 0x02 ? GLWASM_V_BLOCK :
          ins->op == 0x03 ? GLWASM_V_LOOP : GLWASM_V_IF;

      if (ins->op == 0x04 && !pop_val(v, num_type(GLWASM_VK_I32), NULL)) {
        return 0;
      }
      if (ins->a >= fn->nblocks) {
        return fail(v, GLWASM_ERR_VALIDATE);
      }
      return enter_block(v, kind, &fn->blocks[ins->a], fn);
    }
    case 0x05: {
      VFrame * f = top(v);

      if (!f || f->kind != GLWASM_V_IF || f->else_seen) {
        return fail(v, GLWASM_ERR_VALIDATE);
      }
      if (!pop_vals(v, f->results, f->nresults)) {
        return 0;
      }
      if (v->nstack != f->height) {
        return fail(v, GLWASM_ERR_VALIDATE);
      }
      f->else_seen = 1;
      f->unreachable = 0;
      if (f->then_local_init_snapshot)
        memcpy(f->then_local_init_snapshot, v->local_initialized,
               v->nlocal_initialized);
      if (f->local_init_snapshot)
        memcpy(v->local_initialized, f->local_init_snapshot,
               v->nlocal_initialized);
      return push_vals(v, f->params, f->nparams);
    }
    case 0x0b: {
      VFrame *f = top(v);

      if (!f) {
        return fail(v, GLWASM_ERR_VALIDATE);
      }
      if (f->kind == GLWASM_V_IF && !f->else_seen &&
          !same_seq(f->params, f->nparams, f->results, f->nresults)) {
        return fail(v, GLWASM_ERR_VALIDATE);
      }
      if (!pop_vals(v, f->results, f->nresults)) {
        return 0;
      }
      if (v->nstack != f->height) {
        return fail(v, GLWASM_ERR_VALIDATE);
      }
      {
        const GLWASM_ValType *results = f->results;
        uint32_t nresults = f->nresults;
        if (f->local_init_snapshot) {
          if (f->kind == GLWASM_V_IF) {
            if (f->else_seen && f->then_local_init_snapshot) {
              for (uint32_t i = 0; i < v->nlocal_initialized; i++)
                v->local_initialized[i] &= f->then_local_init_snapshot[i];
            } else {
              memcpy(v->local_initialized, f->local_init_snapshot,
                     v->nlocal_initialized);
            }
          } else if (f->kind != GLWASM_V_BLOCK) {
            memcpy(v->local_initialized, f->local_init_snapshot,
                   v->nlocal_initialized);
          }
          gcu_allocator_free(&v->mod->alloc, f->local_init_snapshot);
          f->local_init_snapshot = NULL;
          gcu_allocator_free(&v->mod->alloc, f->then_local_init_snapshot);
          f->then_local_init_snapshot = NULL;
        }
        v->nframes--;
        return push_vals(v, results, nresults);
      }
    }
    case 0x08: {
      const GLWASM_TypeDef *td;
      const GLWASM_Tag *tag;

      if (ins->a >= v->mod->ntags) {
        return fail(v, GLWASM_ERR_VALIDATE);
      }
      tag = &v->mod->tags[ins->a];
      if (!func_type(v->mod, tag->type_index, &td) || td->nresults != 0) {
        return fail(v, GLWASM_ERR_VALIDATE);
      }
      if (!pop_vals(v, td->params, td->nparams)) {
        return 0;
      }
      mark_unreachable(v);
      return 1;
    }
    case 0x0a:
      if (!pop_val(v, ref_type(GLWASM_HT_EXN, 1, 0), NULL)) {
        return 0;
      }
      mark_unreachable(v);
      return 1;
    case 0x0c:
    case 0x0d: {
      VFrame * lab = label_frame(v, ins->a);
      const GLWASM_ValType * types;
      uint32_t n;

      if (!lab) {
        return fail(v, GLWASM_ERR_VALIDATE);
      }
      types = label_types(lab, &n);
      if (ins->op == 0x0d && !pop_val(v, num_type(GLWASM_VK_I32), NULL)) {
        return 0;
      }
      if (!pop_vals(v, types, n)) {
        return 0;
      }
      if (ins->op == 0x0d) {
        return push_vals(v, types, n);
      }
      mark_unreachable(v);
      return 1;
    }
    case 0x0e: {
      VFrame * def;
      const GLWASM_ValType * types;
      uint32_t n;
      uint32_t i;

      if (ins->b == 0 || ins->a + ins->b > fn->nu32) {
        return fail(v, GLWASM_ERR_VALIDATE);
      }
      def = label_frame(v, fn->u32s[ins->a + ins->b - 1u]);
      if (!def) {
        return fail(v, GLWASM_ERR_VALIDATE);
      }
      types = label_types(def, &n);
      for (i = 0; i + 1u < ins->b; i++) {
        VFrame * lab = label_frame(v, fn->u32s[ins->a + i]);
        const GLWASM_ValType * lt;
        uint32_t ln;

        if (!lab) {
          return fail(v, GLWASM_ERR_VALIDATE);
        }
        lt = label_types(lab, &ln);
        if ((top(v)->unreachable && ln != n) ||
            (!top(v)->unreachable && !same_seq(lt, ln, types, n))) {
          return fail(v, GLWASM_ERR_VALIDATE);
        }
      }
      if (!pop_val(v, num_type(GLWASM_VK_I32), NULL) || !pop_vals(v, types, n)) {
        return 0;
      }
      mark_unreachable(v);
      return 1;
    }
    case 0x0f: {
      VFrame * func = v->nframes ? &v->frames[0] : NULL;

      if (!func || !pop_vals(v, func->results, func->nresults)) {
        return 0;
      }
      mark_unreachable(v);
      return 1;
    }
    case 0x10:
      return validate_call(v, fn_type_index(v, ins->a), 0);
    case 0x12:
      return validate_call(v, fn_type_index(v, ins->a), 1);
    case 0x11:
      return validate_indirect(v, ins, 0);
    case 0x13:
      return validate_indirect(v, ins, 1);
    case 0x1a: {
      GLWASM_ValType t;

      return pop_any(v, &t);
    }
    case 0x1b: {
      GLWASM_ValType a;
      GLWASM_ValType b;

      if (!pop_val(v, num_type(GLWASM_VK_I32), NULL) || !pop_any(v, &a) ||
          !pop_any(v, &b)) {
        return 0;
      }
      if (a.kind == GLWASM_VK_REF || b.kind == GLWASM_VK_REF) {
        return fail(v, GLWASM_ERR_VALIDATE);
      }
      if (a.kind != GLWASM_VK_BOT && b.kind != GLWASM_VK_BOT &&
          !val_equal(a, b)) {
        return fail(v, GLWASM_ERR_VALIDATE);
      }
      return push_val(v, a.kind == GLWASM_VK_BOT ? b : a);
    }
    case 0x1c: {
      GLWASM_ValType t;
      GLWASM_ValType a;
      GLWASM_ValType b;

      if (ins->d != 1u || ins->a >= fn->nv) {
        return fail(v, GLWASM_ERR_VALIDATE);
      }
      t = fn->vpool[ins->a];
      if (!pop_val(v, num_type(GLWASM_VK_I32), NULL) || !pop_val(v, t, &a) ||
          !pop_val(v, t, &b)) {
        return 0;
      }
      (void)a;
      (void)b;
      return push_val(v, t);
    }
    case 0x1f: {
      uint32_t i;

      if (ins->a >= fn->nblocks) {
        return fail(v, GLWASM_ERR_VALIDATE);
      }
      for (i = 0; i < ins->d; i++) {
        GLWASM_Catch c;
        VFrame * lab;
        const GLWASM_ValType * lt;
        uint32_t ln;
        const GLWASM_TypeDef * td = NULL;

        if (ins->b + i >= fn->ncatches) {
          return fail(v, GLWASM_ERR_VALIDATE);
        }
        c = fn->catches[ins->b + i];
        lab = label_frame(v, c.label);
        if (!lab) {
          return fail(v, GLWASM_ERR_VALIDATE);
        }
        lt = label_types(lab, &ln);
        if (c.kind < 2) {
          if (c.tag >= v->mod->ntags || !func_type(v->mod,
              v->mod->tags[c.tag].type_index, &td) || td->nresults != 0) {
            return fail(v, GLWASM_ERR_VALIDATE);
          }
        }
        if (c.kind == 0 && !same_seq(lt, ln, td->params, td->nparams)) {
          return fail(v, GLWASM_ERR_VALIDATE);
        }
        if (c.kind == 1) {
          if (ln != td->nparams + 1u || !same_seq(lt, td->nparams, td->params,
              td->nparams) || !val_equal(lt[td->nparams],
              ref_type(GLWASM_HT_EXN, 1, 0))) {
            return fail(v, GLWASM_ERR_VALIDATE);
          }
        }
        if (c.kind == 2 && ln != 0) {
          return fail(v, GLWASM_ERR_VALIDATE);
        }
        if (c.kind == 3 && (ln != 1 || !val_equal(lt[0],
            ref_type(GLWASM_HT_EXN, 1, 0)))) {
          return fail(v, GLWASM_ERR_VALIDATE);
        }
      }
      return enter_block(v, GLWASM_V_TRY, &fn->blocks[ins->a], fn);
    }
    case 0x20:
    case 0x21:
    case 0x22:
      if (ins->a >= fn->nlocals || !fn->locals) {
        return fail(v, GLWASM_ERR_VALIDATE);
      }
      if (ins->op == 0x20) {
        if (v->local_initialized && ins->a < v->nlocal_initialized &&
            fn->locals[ins->a].kind == GLWASM_VK_REF &&
            !fn->locals[ins->a].nullable && !v->local_initialized[ins->a]) {
          return fail(v, GLWASM_ERR_VALIDATE);
        }
        return push_val(v, fn->locals[ins->a]);
      }
      if (!pop_val(v, fn->locals[ins->a], NULL)) {
        return 0;
      }
      if (ins->op == 0x22) {
        if (v->local_initialized && ins->a < v->nlocal_initialized)
          v->local_initialized[ins->a] = 1;
        return push_val(v, fn->locals[ins->a]);
      }
      if (v->local_initialized && ins->a < v->nlocal_initialized)
        v->local_initialized[ins->a] = 1;
      return 1;
    case 0x23:
    case 0x24: {
      const GLWASM_Global *g;

      if (ins->a >= v->mod->nglobals) {
        return fail(v, GLWASM_ERR_VALIDATE);
      }
      g = &v->mod->globals[ins->a];
      if (ins->op == 0x23) {
        if (v->is_const && (ins->a >= v->const_globals || g->mut)) {
          return fail(v, GLWASM_ERR_VALIDATE);
        }
        return push_val(v, g->type);
      }
      if (!g->mut) {
        return fail(v, GLWASM_ERR_VALIDATE);
      }
      return pop_val(v, g->type, NULL);
    }
    case 0x25:
    case 0x26: {
      const GLWASM_Table * table;

      if (!table_at(v, ins->a, &table)) {
        return 0;
      }
      if (ins->op == 0x26 && !pop_val(v, table->elem, NULL)) {
        return 0;
      }
      if (!pop_val(v, addr_type(table->addr64), NULL)) {
        return 0;
      }
      if (ins->op == 0x25) {
        return push_val(v, table->elem);
      }
      return 1;
    }
    case 0x28: case 0x29: case 0x2a: case 0x2b:
    case 0x2c: case 0x2d: case 0x2e: case 0x2f:
    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x34: case 0x35: case 0x36: case 0x37:
    case 0x38: case 0x39: case 0x3a: case 0x3b:
    case 0x3c: case 0x3d: case 0x3e:
      return validate_load_store(v, ins);
    case 0x3f:
    case 0x40: {
      const GLWASM_Memory * mem;

      if (!mem_at(v, ins->a, &mem)) {
        return 0;
      }
      if (ins->op == 0x40 && !pop_val(v, addr_type(mem->addr64), NULL)) {
        return 0;
      }
      return push_val(v, addr_type(mem->addr64));
    }
    case 0x41:
      return push_val(v, num_type(GLWASM_VK_I32));
    case 0x42:
      return push_val(v, num_type(GLWASM_VK_I64));
    case 0x43:
      return push_val(v, num_type(GLWASM_VK_F32));
    case 0x44:
      return push_val(v, num_type(GLWASM_VK_F64));
    case 0xd0:
      if (ins->a >= fn->nv) {
        return fail(v, GLWASM_ERR_VALIDATE);
      }
      return push_val(v, fn->vpool[ins->a]);
    case 0xd1: {
      GLWASM_ValType t;

      if (!pop_any(v, &t)) {
        return 0;
      }
      if (t.kind != GLWASM_VK_BOT && t.kind != GLWASM_VK_REF) {
        return fail(v, GLWASM_ERR_VALIDATE);
      }
      return push_val(v, num_type(GLWASM_VK_I32));
    }
    case 0xd2:
      if (ins->a >= v->mod->nfuncs || !v->reffed || !v->reffed[ins->a]) {
        return fail(v, GLWASM_ERR_VALIDATE);
      }
      return push_val(v, ref_type(GLWASM_HT_FUNC, 0, 0));
    default:
      if (ins->op >= 0x45 && ins->op <= 0xc4) {
        return validate_numeric(v, (uint8_t)ins->op);
      }
      return fail(v, GLWASM_ERR_VALIDATE);
  }
}

/* call's type index is the callee's, not a helper. Defined below the switch
 * would be too late; the cases above call this. */
static uint32_t fn_type_index(VState * v, uint32_t func_index) {
  if (func_index >= v->mod->nfuncs) {
    v->err = GLWASM_ERR_VALIDATE;
    return UINT32_MAX;
  }
  return v->mod->funcs[func_index].type_index;
}

static int const_opcode(uint16_t op) {
  return op == 0x0b || op == 0x41 || op == 0x42 || op == 0x43 || op == 0x44 ||
      op == 0xd0 || op == 0xd2 || op == 0x23 || op == 0x6a || op == 0x6b ||
      op == 0x6c || op == 0x7c || op == 0x7d || op == 0x7e;
}

static int validate_expr_body(VState * v, GLWASM_Func * fn, int is_const,
    const GLWASM_ValType * params, uint32_t nparams,
    const GLWASM_ValType * results, uint32_t nresults) {
  VFrame frame;
  uint32_t i;

  v->nstack = 0;
  v->nframes = 0;
  v->is_const = is_const;
  v->function = is_const ? NULL : fn;
  v->max_stack = 0;
  v->max_control_depth = 0;
  memset(&frame, 0, sizeof(frame));
  frame.kind = GLWASM_V_FUNC;
  frame.params = params;
  frame.nparams = nparams;
  frame.results = results;
  frame.nresults = nresults;
  if (!push_frame(v, frame)) {
    return 0;
  }
  for (i = 0; i < fn->nins; i++) {
    if (is_const && !const_opcode(fn->ins[i].op)) {
      return fail(v, GLWASM_ERR_VALIDATE);
    }
    if (!validate_instr(v, fn, &fn->ins[i])) {
      return 0;
    }
  }
  if (v->nframes != 0 || v->nstack != nresults) {
    return fail(v, GLWASM_ERR_VALIDATE);
  }
  for (i = 0; i < nresults; i++) {
    if (!val_sub(v->mod, v->stack[i], results[i])) {
      return fail(v, GLWASM_ERR_VALIDATE);
    }
  }
  if (!is_const) {
    fn->max_stack = v->max_stack;
    fn->max_control_depth = v->max_control_depth;
  }
  return 1;
}

static int validate_expr(VState *v, GLWASM_Func *fn, int is_const,
                         const GLWASM_ValType *params, uint32_t nparams,
                         const GLWASM_ValType *results, uint32_t nresults) {
  uint8_t *initialized = NULL;
  uint32_t i;
  int ok;

  if (!is_const && fn->nlocals) {
    initialized =
        gcu_allocator_calloc(&v->mod->alloc, fn->nlocals, sizeof(*initialized));
    if (!initialized)
      return fail(v, GLWASM_ERR_OOM);
    for (i = 0; i < fn->nparams && i < fn->nlocals; i++)
      initialized[i] = 1;
  }
  v->local_initialized = initialized;
  v->nlocal_initialized = initialized ? fn->nlocals : 0;
  ok = validate_expr_body(v, fn, is_const, params, nparams, results, nresults);
  for (i = 0; i < v->nframes; i++) {
    gcu_allocator_free(&v->mod->alloc, v->frames[i].local_init_snapshot);
    v->frames[i].local_init_snapshot = NULL;
    gcu_allocator_free(&v->mod->alloc, v->frames[i].then_local_init_snapshot);
    v->frames[i].then_local_init_snapshot = NULL;
  }
  gcu_allocator_free(&v->mod->alloc, initialized);
  v->local_initialized = NULL;
  v->nlocal_initialized = 0;
  return ok;
}

static int note_ref(VState *v, uint32_t func) {
  if (func >= v->mod->nfuncs) {
    return fail(v, GLWASM_ERR_VALIDATE);
  }
  v->reffed[func] = 1;
  return 1;
}

/* C.refs: every function index that occurs outside function bodies and the
 * start function. */
static int scan_refs(VState *v, GLWASM_Func *fn) {
  uint32_t i;

  for (i = 0; i < fn->nins; i++) {
    if (fn->ins[i].op == 0xd2 && !note_ref(v, fn->ins[i].a)) {
      return 0;
    }
  }
  return 1;
}

static int names_equal(const GLWASM_Name * a, const GLWASM_Name * b) {
  if (a->length != b->length) {
    return 0;
  }
  if (a->length == 0) {
    return 1;
  }
  return memcmp(a->bytes, b->bytes, a->length) == 0;
}

static int validate_module(VState * v) {
  GLWASM_Module * m = v->mod;
  uint32_t i;

  if (m->nfuncs != 0) {
    v->reffed = gcu_allocator_calloc(&m->alloc, m->nfuncs, 1);
    if (!v->reffed) {
      return fail(v, GLWASM_ERR_OOM);
    }
  }
  for (i = 0; i < m->nelems; i++) {
    GLWASM_Elem * el = &m->elems[i];
    uint32_t k;

    for (k = 0; k < el->nfuncs; k++) {
      if (!note_ref(v, el->funcs[k])) {
        return 0;
      }
    }
    for (k = 0; k < el->nexprs; k++) {
      if (!scan_refs(v, &el->exprs[k])) {
        return 0;
      }
    }
    if (!scan_refs(v, &el->offset)) {
      return 0;
    }
  }
  for (i = 0; i < m->nglobals; i++) {
    if (!scan_refs(v, &m->globals[i].init)) {
      return 0;
    }
  }
  for (i = 0; i < m->ntables; i++) {
    if (!scan_refs(v, &m->tables[i].init)) {
      return 0;
    }
  }
  for (i = 0; i < m->ndatas; i++) {
    if (!scan_refs(v, &m->datas[i].offset)) {
      return 0;
    }
  }
  for (i = 0; i < m->nexports; i++) {
    if (m->exports[i].kind == GLWASM_EXT_FUNC &&
        !note_ref(v, m->exports[i].index)) {
      return 0;
    }
  }
  for (i = 0; i < m->nfuncs; i++) {
    const GLWASM_TypeDef * td;

    if (!func_type(m, m->funcs[i].type_index, &td)) {
      return fail(v, GLWASM_ERR_VALIDATE);
    }
    if (!m->funcs[i].imported) {
      if (m->funcs[i].nlocals != 0 && !m->funcs[i].locals) {
        return fail(v, GLWASM_ERR_VALIDATE);
      }
      if (!validate_expr(v, &m->funcs[i], 0, td->params, td->nparams,
          td->results, td->nresults)) {
        return 0;
      }
    }
  }
  for (i = 0; i < m->nglobals; i++) {
    GLWASM_Global * g = &m->globals[i];

    v->const_globals = i;
    if (!g->imported && !validate_expr(v, &g->init, 1, NULL, 0, &g->type, 1)) {
      return 0;
    }
  }
  /* Tables are declared before any global is defined, so only the imported
   * globals are visible to a table initializer. */
  for (i = 0; i < m->nglobals && m->globals[i].imported; i++) {
  }
  v->const_globals = i;
  for (i = 0; i < m->ntables; i++) {
    GLWASM_Table * t = &m->tables[i];

    if (t->elem.kind != GLWASM_VK_REF) {
      return fail(v, GLWASM_ERR_VALIDATE);
    }
    if (!t->has_init && !t->elem.nullable) {
      return fail(v, GLWASM_ERR_VALIDATE);
    }
    if (t->has_init && !validate_expr(v, &t->init, 1, NULL, 0, &t->elem, 1)) {
      return 0;
    }
  }
  /* An element or data offset runs after every global is initialized. */
  v->const_globals = m->nglobals;
  for (i = 0; i < m->nelems; i++) {
    GLWASM_Elem * el = &m->elems[i];
    uint32_t k;

    if (el->mode == GLWASM_ELEM_ACTIVE) {
      const GLWASM_Table * table;
      GLWASM_ValType addr;

      if (!table_at(v, el->table, &table)) {
        return 0;
      }
      if (!val_sub(m, el->reftype, table->elem)) {
        return fail(v, GLWASM_ERR_VALIDATE);
      }
      addr = addr_type(table->addr64);
      if (!validate_expr(v, &el->offset, 1, NULL, 0, &addr, 1)) {
        return 0;
      }
    }
    for (k = 0; k < el->nexprs; k++) {
      if (!validate_expr(v, &el->exprs[k], 1, NULL, 0, &el->reftype, 1)) {
        return 0;
      }
    }
  }
  for (i = 0; i < m->ndatas; i++) {
    GLWASM_Data * d = &m->datas[i];

    if (!d->passive) {
      const GLWASM_Memory * mem;
      GLWASM_ValType addr;

      if (!mem_at(v, d->mem, &mem)) {
        return 0;
      }
      addr = addr_type(mem->addr64);
      if (!validate_expr(v, &d->offset, 1, NULL, 0, &addr, 1)) {
        return 0;
      }
    }
  }
  for (i = 0; i < m->nexports; i++) {
    GLWASM_Export * ex = &m->exports[i];
    uint32_t n = 0;
    uint32_t j;

    switch (ex->kind) {
      case GLWASM_EXT_FUNC:
        n = m->nfuncs;
        break;
      case GLWASM_EXT_TABLE:
        n = m->ntables;
        break;
      case GLWASM_EXT_MEM:
        n = m->nmems;
        break;
      case GLWASM_EXT_GLOBAL:
        n = m->nglobals;
        break;
      case GLWASM_EXT_TAG:
        n = m->ntags;
        break;
      default:
        return fail(v, GLWASM_ERR_VALIDATE);
    }
    if (ex->index >= n) {
      return fail(v, GLWASM_ERR_VALIDATE);
    }
    for (j = 0; j < i; j++) {
      if (names_equal(&ex->name, &m->exports[j].name)) {
        return fail(v, GLWASM_ERR_VALIDATE);
      }
    }
  }
  if (m->has_start) {
    const GLWASM_TypeDef * td;

    if (m->start >= m->nfuncs || !func_type(m, m->funcs[m->start].type_index,
        &td) || td->nparams != 0 || td->nresults != 0) {
      return fail(v, GLWASM_ERR_VALIDATE);
    }
  }
  for (i = 0; i < m->ntags; i++) {
    const GLWASM_TypeDef * td;

    if (!func_type(m, m->tags[i].type_index, &td) || td->nresults != 0) {
      return fail(v, GLWASM_ERR_VALIDATE);
    }
  }
  return 1;
}

int glwasm_module_validate(GLWASM_Module * module, GLWASM_Result * err,
    const char * * limit) {
  VState v;
  int ok;

  if (!module || !err) {
    return 0;
  }
  memset(&v, 0, sizeof(v));
  v.mod = module;
  v.err = GLWASM_OK;
  ok = validate_module(&v);
  gcu_allocator_free(&module->alloc, v.stack);
  gcu_allocator_free(&module->alloc, v.frames);
  gcu_allocator_free(&module->alloc, v.reffed);
  if (!ok) {
    *err = v.err == GLWASM_OK ? GLWASM_ERR_VALIDATE : v.err;
    if (limit) {
      *limit = *err == GLWASM_ERR_LIMIT ? v.limit : NULL;
    }
    return 0;
  }
  *err = GLWASM_OK;
  if (limit) {
    *limit = NULL;
  }
  return 1;
}
