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
 * Table limits, the bottom heap types of the in-scope reference lattices, and
 * copies between memories or tables of different address types.
 */

#include "hexdecode.h"

#include <gtest/gtest.h>

namespace {

const Case k_limits[] = {
  {"I32TableMin2p32",
   "0061736d0100000004080170008080808010",
   GLWASM_ERR_VALIDATE},
  {"I32TableMax2p32",
   "0061736d010000000409017001008080808010",
   GLWASM_ERR_VALIDATE},
  {"I32TableMin2p32m1",
   "0061736d010000000408017000ffffffff0f",
   GLWASM_OK},
  {"I32TableMaxImport2p32",
   "0061736d01000000020d01016d01740170008080808010",
   GLWASM_ERR_VALIDATE},
};

const Case k_refs[] = {
  {"RefNullNofunc",
   "0061736d01000000010401600000030201000a07010500d0731a0b",
   GLWASM_OK},
  {"RefNullNoextern",
   "0061736d01000000010401600000030201000a07010500d0721a0b",
   GLWASM_OK},
  {"RefNullNoexn",
   "0061736d01000000010401600000030201000a07010500d0741a0b",
   GLWASM_OK},
  {"NofuncFlowsToFuncref",
   "0061736d0100000001050160000170030201000a06010400d0730b",
   GLWASM_OK},
  {"NoexternFlowsToExternref",
   "0061736d010000000105016000016f030201000a06010400d0720b",
   GLWASM_OK},
  {"NoexnFlowsToExnref",
   "0061736d0100000001050160000169030201000a06010400d0740b",
   GLWASM_OK},
  {"NofuncDoesNotFlowToExternref",
   "0061736d010000000105016000016f030201000a06010400d0730b",
   GLWASM_ERR_VALIDATE},
  {"FuncrefDoesNotFlowToNofunc",
   "0061736d010000000106016000016373030201000a06010400d0700b",
   GLWASM_ERR_VALIDATE},
  {"NofuncTable",
   "0061736d0100000004050163730001",
   GLWASM_OK},
  {"RefNullNone",
   "0061736d01000000010401600000030201000a07010500d0711a0b",
   GLWASM_ERR_UNSUPPORTED},
  {"RefNullAny",
   "0061736d01000000010401600000030201000a07010500d06e1a0b",
   GLWASM_ERR_UNSUPPORTED},
  {"RefNullEq",
   "0061736d01000000010401600000030201000a07010500d06d1a0b",
   GLWASM_ERR_UNSUPPORTED},
  {"RefNullI31",
   "0061736d01000000010401600000030201000a07010500d06c1a0b",
   GLWASM_ERR_UNSUPPORTED},
  {"RefNullStruct",
   "0061736d01000000010401600000030201000a07010500d06b1a0b",
   GLWASM_ERR_UNSUPPORTED},
  {"RefNullArray",
   "0061736d01000000010401600000030201000a07010500d06a1a0b",
   GLWASM_ERR_UNSUPPORTED},
};

const Case k_copy[] = {
  {"MemoryCopyI64DstI32Src",
   "0061736d010000000104016000000302010005060200010501010a0e010c00420041004100fc0a01000b",
   GLWASM_OK},
  {"TableCopyI64DstI32Src",
   "0061736d0100000001040160000003020100040802700001700501010a0e010c00420041004100fc0e01000b",
   GLWASM_OK},
  {"MemoryCopyI32DstI64Src",
   "0061736d010000000104016000000302010005060200010501010a0e010c00410042004100fc0a00010b",
   GLWASM_OK},
  {"MemoryCopyMixedWithI64LengthIsInvalid",
   "0061736d010000000104016000000302010005060200010501010a0e010c00420041004200fc0a01000b",
   GLWASM_ERR_VALIDATE},
  {"MemoryCopyBothI64",
   "0061736d010000000104016000000302010005060200010501010a0e010c00420042004200fc0a01010b",
   GLWASM_OK},
  {"TableCopyMixedWithI64LengthIsInvalid",
   "0061736d0100000001040160000003020100040802700001700501010a0e010c00420041004200fc0e01000b",
   GLWASM_ERR_VALIDATE},
  {"TableCopySourceSubtypeOfDestination",
   "0061736d0100000001040160000003020100040802700001637300010a0e010c00410041004100fc0e00010b",
   GLWASM_OK},
  {"TableCopySourceNotSubtypeOfDestination",
   "0061736d0100000001040160000003020100040802700001637300010a0e010c00410041004100fc0e01000b",
   GLWASM_ERR_VALIDATE},
};

}  // namespace

TEST(Types, I32TableLimitsStopBelow2To32) {
  for (const Case & c : k_limits) {
    SCOPED_TRACE(c.name);
    EXPECT_EQ(decode_hex(c.hex), c.expect);
  }
}

TEST(Types, BottomHeapTypesAreInScopeAndSubtypeChecked) {
  for (const Case & c : k_refs) {
    SCOPED_TRACE(c.name);
    EXPECT_EQ(decode_hex(c.hex), c.expect);
  }
}

/* Wasm 3.0 allows memory.copy and table.copy between an i32 and an i64
 * memory or table, with the length of the narrower type (i32), and table.copy
 * needs only that the source element type be a subtype of the destination's.
 * V8 12.4 rejects the mixed address types; lang-wasm follows the
 * specification, and this pins that. */
TEST(Types, CopyAcrossAddressTypesIsValid) {
  for (const Case & c : k_copy) {
    SCOPED_TRACE(c.name);
    EXPECT_EQ(decode_hex(c.hex), c.expect);
  }
}

int main(int argc, char * * argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
