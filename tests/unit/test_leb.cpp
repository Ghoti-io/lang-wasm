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
 * LEB128 integers: padding up to ceil(N/7) bytes is legal at every read site;
 * a longer encoding, or unused final bits that are not zero (unsigned) or a
 * sign extension (signed), is malformed.
 */

#include "hexdecode.h"

#include <gtest/gtest.h>

namespace {

const Case k_padded[] = {
  {"SectionSize",
   "0061736d010000000184800001600000",
   GLWASM_OK},
  {"VecCount",
   "0061736d0100000001058100600000",
   GLWASM_OK},
  {"LimitsMin",
   "0061736d0100000005050100818000",
   GLWASM_OK},
  {"LimitsMax",
   "0061736d0100000005050101018100",
   GLWASM_OK},
  {"FuncTypeIndex",
   "0061736d0100000001040160000003030180000a040102000b",
   GLWASM_OK},
  {"BlockTypeS33",
   "0061736d010000000104016000000302010005030100010a080106000280000b0b",
   GLWASM_OK},
  {"I32Const",
   "0061736d010000000104016000000302010005030100010a09010700418080001a0b",
   GLWASM_OK},
  {"I32ConstNeg",
   "0061736d010000000104016000000302010005030100010a0b01090041ffffffff7f1a0b",
   GLWASM_OK},
  {"I64Const",
   "0061736d010000000104016000000302010005030100010a10010e0042808080808080808080001a0b",
   GLWASM_OK},
  {"MemargAlign",
   "0061736d010000000104016000000302010005030100010a0b0109004100288200001a0b",
   GLWASM_OK},
  {"MemargOffset",
   "0061736d010000000104016000000302010005030100010a0c010a00410028028080001a0b",
   GLWASM_OK},
  {"LocalsCount",
   "0061736d01000000010401600000030201000a0701050181007f0b",
   GLWASM_OK},
  {"FcSubOpcode",
   "0061736d010000000104016000000302010005030100010a0e010c00410041004100fc8b00000b",
   GLWASM_OK},
};

const Case k_malformed[] = {
  {"SectionSizeTooLong",
   "0061736d010000000184808080800001600000",
   GLWASM_ERR_CORRUPT},
  {"VecCountTooLong",
   "0061736d010000000109818080808000600000",
   GLWASM_ERR_CORRUPT},
  {"I32ConstTooLong",
   "0061736d010000000104016000000302010005030100010a0c010a00418080808080001a0b",
   GLWASM_ERR_CORRUPT},
  {"I64ConstTooLong",
   "0061736d010000000104016000000302010005030100010a11010f004280808080808080808080001a0b",
   GLWASM_ERR_CORRUPT},
  {"S33TooLong",
   "0061736d010000000104016000000302010005030100010a0c010a00028080808080000b0b",
   GLWASM_ERR_CORRUPT},
  {"FcSubTooLong",
   "0061736d010000000104016000000302010005030100010a0b010900fc8080808080000b",
   GLWASM_ERR_CORRUPT},
  {"VecCountBadFinal",
   "0061736d010000000108ffffffff1f600000",
   GLWASM_ERR_CORRUPT},
  {"U32BadFinalIndex",
   "0061736d0100000001040160000003060180808080100a040102000b",
   GLWASM_ERR_CORRUPT},
  {"I32ConstBadFinalPos",
   "0061736d010000000104016000000302010005030100010a0b0109004180808080701a0b",
   GLWASM_ERR_CORRUPT},
  {"I32ConstBadFinalNeg",
   "0061736d010000000104016000000302010005030100010a0b01090041ffffffff0f1a0b",
   GLWASM_ERR_CORRUPT},
  {"I64ConstBadFinal",
   "0061736d010000000104016000000302010005030100010a10010e0042808080808080808080021a0b",
   GLWASM_ERR_CORRUPT},
  {"S33BadFinal",
   "0061736d010000000104016000000302010005030100010a0b0109000280808080200b0b",
   GLWASM_ERR_CORRUPT},
  {"LimitsU64BadFinal",
   "0061736d01000000050c010080808080808080808002",
   GLWASM_ERR_CORRUPT},
};

}  // namespace

TEST(Leb, PaddingIsAcceptedAtEverySite) {
  for (const Case & c : k_padded) {
    SCOPED_TRACE(c.name);
    EXPECT_EQ(decode_hex(c.hex), c.expect);
  }
}

TEST(Leb, TooLongOrBadFinalBitsIsCorrupt) {
  for (const Case & c : k_malformed) {
    SCOPED_TRACE(c.name);
    EXPECT_EQ(decode_hex(c.hex), c.expect);
  }
}

int main(int argc, char * * argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
