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
 * The decoded module. A value type is not a 64-bit word: a v128 is its own
 * kind, and a reference can name a type index. The validator's operand stack
 * stores these descriptors, never a host register.
 */

#ifndef GHOTI_IO_GLWASM_SRC_MODULE_INTERNAL_H
#define GHOTI_IO_GLWASM_SRC_MODULE_INTERNAL_H

#include <ghoti.io/lang-wasm/macros.h>

#include <ghoti.io/lang-wasm/decode.h>
#include <ghoti.io/lang-wasm/policy.h>

#include <stdint.h>

enum {
  GLWASM_VK_BOT = 0,
  GLWASM_VK_I32,
  GLWASM_VK_I64,
  GLWASM_VK_F32,
  GLWASM_VK_F64,
  GLWASM_VK_V128,
  GLWASM_VK_REF
};

/* Abstract heap types, plus a concrete type index. `bot` is not stored. */
enum {
  GLWASM_HT_NONE = 0,
  GLWASM_HT_NOFUNC,
  GLWASM_HT_NOEXTERN,
  GLWASM_HT_NOEXN,
  GLWASM_HT_FUNC,
  GLWASM_HT_EXTERN,
  GLWASM_HT_ANY,
  GLWASM_HT_EQ,
  GLWASM_HT_I31,
  GLWASM_HT_STRUCT,
  GLWASM_HT_ARRAY,
  GLWASM_HT_EXN,
  GLWASM_HT_INDEX
};

enum {
  GLWASM_COMP_FUNC = 0,
  GLWASM_COMP_STRUCT,
  GLWASM_COMP_ARRAY
};

typedef struct GLWASM_ValType {
  uint8_t kind;
  uint8_t nullable;
  uint8_t heap;
  uint8_t mut;
  uint32_t index;
} GLWASM_ValType;

typedef struct GLWASM_TypeDef {
  uint8_t comp;
  uint8_t is_final;
  uint32_t nsupers;
  uint32_t * supers;
  uint32_t nparams;
  GLWASM_ValType * params;
  uint32_t nresults;
  GLWASM_ValType * results;
  uint8_t * muts;
} GLWASM_TypeDef;

typedef struct GLWASM_Instr {
  uint16_t op;
  uint16_t pad;
  uint32_t a;
  uint32_t b;
  uint32_t d;
  uint64_t c;
  /* Absolute byte offset in the original module (canonical lowered binary
   * for WAT input). */
  uint64_t source_offset;
} GLWASM_Instr;

typedef struct GLWASM_BlockType {
  uint32_t type_index; /* UINT32_MAX when the block type is inline. */
  uint32_t param_at;
  uint32_t nparams;
  uint32_t result_at;
  uint32_t nresults;
  uint8_t resolved;
} GLWASM_BlockType;

typedef struct GLWASM_Catch {
  uint8_t kind; /* 0 catch, 1 catch_ref, 2 catch_all, 3 catch_all_ref */
  uint32_t tag;
  uint32_t label;
  /* Resolved by translate.c. The function label has no opener. */
  uint32_t target_pc;
  uint32_t target_opener;
  uint8_t resolved;
} GLWASM_Catch;

typedef struct GLWASM_Func {
  uint32_t type_index;
  uint8_t imported;
  uint8_t is_const;
  uint32_t nparams;
  uint32_t nlocals;
  GLWASM_ValType * locals;
  GLWASM_Instr * ins;
  uint32_t nins;
  uint32_t ins_cap;
  uint32_t * u32s;
  uint32_t nu32;
  uint32_t u32_cap;
  GLWASM_ValType * vpool;
  uint32_t nv;
  uint32_t vcap;
  GLWASM_BlockType * blocks;
  uint32_t nblocks;
  uint32_t block_cap;
  GLWASM_Catch * catches;
  uint32_t ncatches;
  uint32_t catch_cap;
  struct GLWASM_TranslatedFunc * translated;
} GLWASM_Func;

typedef struct GLWASM_Table {
  GLWASM_ValType elem;
  uint8_t addr64;
  uint8_t has_max;
  uint8_t imported;
  uint8_t has_init;
  uint64_t min;
  uint64_t max;
  GLWASM_Func init;
} GLWASM_Table;

typedef struct GLWASM_Memory {
  uint8_t addr64;
  uint8_t has_max;
  uint8_t imported;
  uint64_t min;
  uint64_t max;
} GLWASM_Memory;

typedef struct GLWASM_Global {
  GLWASM_ValType type;
  uint8_t mut;
  uint8_t imported;
  GLWASM_Func init;
} GLWASM_Global;

typedef struct GLWASM_Tag {
  uint32_t type_index;
  uint8_t imported;
} GLWASM_Tag;

typedef struct GLWASM_Name {
  char * bytes;
  uint32_t length;
} GLWASM_Name;

enum {
  GLWASM_EXT_FUNC = 0,
  GLWASM_EXT_TABLE,
  GLWASM_EXT_MEM,
  GLWASM_EXT_GLOBAL,
  GLWASM_EXT_TAG
};

typedef struct GLWASM_Import {
  GLWASM_Name module_name;
  GLWASM_Name field;
  uint8_t kind;
  uint32_t index;
} GLWASM_Import;

typedef struct GLWASM_Export {
  GLWASM_Name name;
  uint8_t kind;
  uint32_t index;
} GLWASM_Export;

enum {
  GLWASM_ELEM_ACTIVE = 0,
  GLWASM_ELEM_PASSIVE,
  GLWASM_ELEM_DECLARATIVE
};

typedef struct GLWASM_Elem {
  uint8_t mode;
  uint8_t use_exprs;
  uint32_t table;
  GLWASM_ValType reftype;
  GLWASM_Func offset;
  uint32_t * funcs;
  uint32_t nfuncs;
  GLWASM_Func * exprs;
  uint32_t nexprs;
} GLWASM_Elem;

typedef struct GLWASM_Data {
  uint8_t passive;
  uint32_t mem;
  GLWASM_Func offset;
  uint8_t * bytes;
  uint32_t nbytes;
} GLWASM_Data;

struct GLWASM_Module {
  GLWASM_Allocator alloc;
  GLWASM_Limits limits;
  GLWASM_Policy policy;
  uint8_t has_policy;
  GLWASM_TypeDef * types;
  uint32_t ntypes;
  GLWASM_Import * imports;
  uint32_t nimports;
  GLWASM_Func * funcs;
  uint32_t nfuncs;
  uint32_t nimported_funcs;
  GLWASM_Table * tables;
  uint32_t ntables;
  GLWASM_Memory * mems;
  uint32_t nmems;
  GLWASM_Global * globals;
  uint32_t nglobals;
  GLWASM_Export * exports;
  uint32_t nexports;
  int has_start;
  uint32_t start;
  GLWASM_Elem * elems;
  uint32_t nelems;
  GLWASM_Data * datas;
  uint32_t ndatas;
  int has_data_count;
  uint32_t data_count;
  GLWASM_Tag * tags;
  uint32_t ntags;
};

typedef struct GLWASM_TranslatedInstr {
  GLWASM_Instr source;
  uint32_t branch_target;
  uint32_t alternate_target;
  uint32_t call_target;
} GLWASM_TranslatedInstr;

typedef struct GLWASM_TranslatedFunc {
  GLWASM_TranslatedInstr * ins;
  uint32_t nins;
  uint64_t * offsets;
  uint32_t offset_count;
  uint32_t * branch_table_targets;
  uint32_t branch_table_count;
} GLWASM_TranslatedFunc;

GLWASM_Result glwasm_translate_module(GLWASM_Module * module);
uint8_t glwasm_opcode_cost(uint16_t opcode);
void glwasm_translate_func_destroy(GLWASM_Module * module,
    GLWASM_TranslatedFunc * translated);

/**
 * @brief Validate a module that has already decoded.
 *
 * On failure @p *err is the result and @p *limit is the field name when
 * that result is a cap. Scratch is freed either way. The module is not.
 */
int glwasm_module_validate(
    GLWASM_Module * module, GLWASM_Result * err, const char * * limit);

#endif /* GHOTI_IO_GLWASM_SRC_MODULE_INTERNAL_H */
