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
 * Constant expressions and declared function references: where ref.func and
 * global.get are legal outside function bodies.
 */

#include "hexdecode.h"

#include <gtest/gtest.h>

namespace {

const Case k_refs[] = {
  {"RefFuncGlobalInit",
   "0061736d01000000010401600000030201000606017000d2000b0a040102000b",
   GLWASM_OK},
  {"RefFuncExport",
   "0061736d010000000105016000017003020100070501016600000a06010400d2000b",
   GLWASM_OK},
  {"RefFuncElemActive",
   "0061736d01000000010401600000030201000404017000010909010441000b01d2000b0a07010500d2001a0b",
   GLWASM_OK},
  {"RefFuncElemPassive",
   "0061736d0100000001040160000003020100090701057001d2000b0a07010500d2001a0b",
   GLWASM_OK},
  {"RefFuncElemDeclarative",
   "0061736d0100000001040160000003020100090701077001d2000b0a07010500d2001a0b",
   GLWASM_OK},
  {"RefFuncElemFuncIndices",
   "0061736d0100000001040160000003020100090501030001000a07010500d2001a0b",
   GLWASM_OK},
  {"RefFuncTableInit",
   "0061736d01000000010401600000030201000409014000700001d2000b0a07010500d2001a0b",
   GLWASM_OK},
  {"RefFuncUndeclared",
   "0061736d01000000010401600000030201000a07010500d2001a0b",
   GLWASM_ERR_VALIDATE},
  {"RefFuncStartDoesNotDeclare",
   "0061736d01000000010401600000030201000801000a07010500d2001a0b",
   GLWASM_ERR_VALIDATE},
  {"RefFuncOtherIndexNotDeclared",
   "0061736d010000000104016000000303020000070501016600010a0a020500d2001a0b02000b",
   GLWASM_ERR_VALIDATE},
};

const Case k_globals[] = {
  {"GlobalGetEarlierImmutable",
   "0061736d01000000060b027f0041010b7f0023000b",
   GLWASM_OK},
  {"GlobalGetEarlierMutable",
   "0061736d01000000060b027f0141010b7f0023000b",
   GLWASM_ERR_VALIDATE},
  {"GlobalGetForward",
   "0061736d01000000060b027f0023010b7f0041010b",
   GLWASM_ERR_VALIDATE},
  {"GlobalGetSelf",
   "0061736d010000000606017f0023000b",
   GLWASM_ERR_VALIDATE},
  {"DataOffsetDefinedGlobal",
   "0061736d0100000005030100010606017f0041000b0b07010023000b0178",
   GLWASM_OK},
  {"DataOffsetMutableGlobal",
   "0061736d0100000005030100010606017f0141000b0b07010023000b0178",
   GLWASM_ERR_VALIDATE},
  {"ElemOffsetDefinedGlobal",
   "0061736d01000000010401600000030201000404017000010606017f0041000b0907010023000b01000a040102000b",
   GLWASM_OK},
  {"TableInitDefinedGlobalStaysInvalid",
   "0061736d01000000040901400070000123000b0606017000d0700b",
   GLWASM_ERR_VALIDATE},
};

}  // namespace

TEST(ConstExpr, RefFuncIsDeclaredByAnyPlaceOutsideBodies) {
  for (const Case & c : k_refs) {
    SCOPED_TRACE(c.name);
    EXPECT_EQ(decode_hex(c.hex), c.expect);
  }
}

TEST(ConstExpr, GlobalGetReadsAnEarlierImmutableGlobal) {
  for (const Case & c : k_globals) {
    SCOPED_TRACE(c.name);
    EXPECT_EQ(decode_hex(c.hex), c.expect);
  }
}

int main(int argc, char * * argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
