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
 * Validation of a module the decoder has already accepted as Wasm.
 */

#include <ghoti.io/lang-wasm/lang-wasm.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

std::vector<uint8_t> hex(const char * text) {
  std::vector<uint8_t> out;
  int hi = -1;

  for (const char * p = text; *p; p++) {
    char c = *p;
    int v;

    if (c == ' ' || c == '\n') {
      continue;
    }
    if (c >= '0' && c <= '9') {
      v = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      v = c - 'a' + 10;
    } else {
      continue;
    }
    if (hi < 0) {
      hi = v;
    } else {
      out.push_back(static_cast<uint8_t>((hi << 4) | v));
      hi = -1;
    }
  }
  return out;
}

GLWASM_Result decode(const char * text, GLWASM_Module * * module) {
  std::vector<uint8_t> bytes = hex(text);

  return glwasm_module_decode(bytes.data(), bytes.size(), NULL, NULL, NULL,
      module);
}

}  // namespace

TEST(Validate, BodyTypeMismatch) {
  GLWASM_Module * module = reinterpret_cast<GLWASM_Module *>(1);

  EXPECT_EQ(decode(
      "00 61 73 6d 01 00 00 00 "
      "01 05 01 60 00 01 7f "
      "03 02 01 00 "
      "0a 05 01 03 00 01 0b",
      &module), GLWASM_ERR_VALIDATE);
  EXPECT_EQ(module, reinterpret_cast<GLWASM_Module *>(1));
}

TEST(Validate, UnreachableProducesTheResult) {
  GLWASM_Module * module = NULL;

  EXPECT_EQ(decode(
      "00 61 73 6d 01 00 00 00 "
      "01 05 01 60 00 01 7f "
      "03 02 01 00 "
      "0a 05 01 03 00 00 0b",
      &module), GLWASM_OK);
  glwasm_module_destroy(module);
}

TEST(Validate, IfElse) {
  GLWASM_Module * module = NULL;

  EXPECT_EQ(decode(
      "00 61 73 6d 01 00 00 00 "
      "01 05 01 60 00 01 7f "
      "03 02 01 00 "
      "0a 0e 01 0c 00 41 01 04 7f 41 02 05 41 03 0b 0b",
      &module), GLWASM_OK);
  glwasm_module_destroy(module);
}

TEST(Validate, CallIndirectThroughFuncref) {
  GLWASM_Module * module = NULL;

  EXPECT_EQ(decode(
      "00 61 73 6d 01 00 00 00 "
      "01 04 01 60 00 00 "
      "03 02 01 00 "
      "04 05 01 70 01 00 01 "
      "0a 09 01 07 00 41 00 11 00 00 0b",
      &module), GLWASM_OK);
  glwasm_module_destroy(module);
}

TEST(Validate, LoadAlignment) {
  GLWASM_Module * module = NULL;

  EXPECT_EQ(decode(
      "00 61 73 6d 01 00 00 00 "
      "01 05 01 60 00 01 7f "
      "03 02 01 00 "
      "05 04 01 01 01 01 "
      "0a 09 01 07 00 41 00 28 02 00 0b",
      &module), GLWASM_OK);
  glwasm_module_destroy(module);
  module = reinterpret_cast<GLWASM_Module *>(1);
  EXPECT_EQ(decode(
      "00 61 73 6d 01 00 00 00 "
      "01 05 01 60 00 01 7f "
      "03 02 01 00 "
      "05 04 01 01 01 01 "
      "0a 09 01 07 00 41 00 28 03 00 0b",
      &module), GLWASM_ERR_VALIDATE);
  EXPECT_EQ(module, reinterpret_cast<GLWASM_Module *>(1));
}

TEST(Validate, LoadOffsetPastI32) {
  GLWASM_Module * module = reinterpret_cast<GLWASM_Module *>(1);

  EXPECT_EQ(decode(
      "00 61 73 6d 01 00 00 00 "
      "01 05 01 60 00 01 7f "
      "03 02 01 00 "
      "05 04 01 01 01 01 "
      "0a 0d 01 0b 00 41 00 28 02 80 80 80 80 10 0b",
      &module), GLWASM_ERR_VALIDATE);
  EXPECT_EQ(module, reinterpret_cast<GLWASM_Module *>(1));
}

TEST(Validate, CallIndexOutOfRange) {
  GLWASM_Module * module = reinterpret_cast<GLWASM_Module *>(1);

  EXPECT_EQ(decode(
      "00 61 73 6d 01 00 00 00 "
      "01 04 01 60 00 00 "
      "03 02 01 00 "
      "0a 06 01 04 00 10 01 0b",
      &module), GLWASM_ERR_VALIDATE);
  EXPECT_EQ(module, reinterpret_cast<GLWASM_Module *>(1));
}

int main(int argc, char * * argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
