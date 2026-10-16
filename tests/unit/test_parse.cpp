/* SPDX-License-Identifier: LGPL-3.0-only */
#include <ghoti.io/lang-wasm/parse.h>
#include <ghoti.io/lang-wasm/decode.h>
#include <ghoti.io/cutil/allocator.h>
#include <gtest/gtest.h>

#include "module_internal.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace {
struct FailAllocator {
  size_t calls = 0;
  size_t fail_at = 0;
};
void * fail_malloc(void * ctx, size_t n) {
  auto * state = static_cast<FailAllocator *>(ctx);
  return ++state->calls == state->fail_at ? nullptr : std::malloc(n);
}
void * fail_calloc(void * ctx, size_t n, size_t size) {
  auto * state = static_cast<FailAllocator *>(ctx);
  return ++state->calls == state->fail_at ? nullptr : std::calloc(n, size);
}
void * fail_realloc(void * ctx, void * ptr, size_t n) {
  auto * state = static_cast<FailAllocator *>(ctx);
  return ++state->calls == state->fail_at ? nullptr : std::realloc(ptr, n);
}
void fail_free(void *, void * ptr) { std::free(ptr); }
}

TEST(WasmTextParser, ParsesNestedCommentsListsAndEscapedBytes) {
  const std::string input =
      "(; outer (; inner ;) ;) (module (custom \"a\\41\\00\"))";
  GLWASM_Text * tree = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_text_parse(reinterpret_cast<const uint8_t *>(input.data()),
                              input.size(), nullptr, &error, &tree),
            GLWASM_OK)
      << error.message;
  ASSERT_NE(tree, nullptr);
  ASSERT_EQ(glwasm_text_size(tree), 1u);
  const GLWASM_Text * module = glwasm_text_at(tree, 0);
  ASSERT_EQ(glwasm_text_kind(module), GLWASM_TEXT_LIST);
  const GLWASM_Text * field = glwasm_text_at(module, 1);
  ASSERT_EQ(glwasm_text_size(field), 2u);
  size_t length = 0;
  const uint8_t * bytes = glwasm_text_bytes(glwasm_text_at(field, 1), &length);
  ASSERT_EQ(length, 3u);
  EXPECT_EQ(bytes[0], 'a');
  EXPECT_EQ(bytes[1], 'A');
  EXPECT_EQ(bytes[2], 0);
  glwasm_text_destroy(tree);
}

TEST(WasmTextParser, ReportsMalformedInputLocationAndLeavesOutputUntouched) {
  const std::string input = "(module)\n(; never closed";
  GLWASM_Text * tree = reinterpret_cast<GLWASM_Text *>(0x1);
  GLWASM_ParseError error{};
  EXPECT_EQ(glwasm_text_parse(reinterpret_cast<const uint8_t *>(input.data()),
                              input.size(), nullptr, &error, &tree),
            GLWASM_ERR_CORRUPT);
  EXPECT_EQ(error.result, GLWASM_ERR_CORRUPT);
  EXPECT_EQ(tree, reinterpret_cast<GLWASM_Text *>(0x1));
  EXPECT_STREQ(error.message, "unterminated nested comment");
  EXPECT_EQ(error.location.line, 2u);
  EXPECT_EQ(error.location.offset, 9u);
  EXPECT_EQ(error.location.column, 1u);
}

TEST(WasmTextParser, ReportsByteOffsetLineAndColumnForNodes) {
  const std::string input = "  (module\n    (func))";
  GLWASM_Text * tree = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_text_parse(reinterpret_cast<const uint8_t *>(input.data()),
                              input.size(), nullptr, &error, &tree), GLWASM_OK);
  const auto module_loc = glwasm_text_location(glwasm_text_at(tree, 0));
  const auto func_loc = glwasm_text_location(
      glwasm_text_at(glwasm_text_at(tree, 0), 1));
  EXPECT_EQ(module_loc.offset, 2u);
  EXPECT_EQ(module_loc.line, 1u);
  EXPECT_EQ(module_loc.column, 3u);
  EXPECT_EQ(func_loc.offset, 14u);
  EXPECT_EQ(func_loc.line, 2u);
  EXPECT_EQ(func_loc.column, 5u);
  glwasm_text_destroy(tree);
}

TEST(WasmTextParser, RejectsMalformedRawUtf8) {
  const uint8_t input[] = {'(', 'x', ')', '\n', '"', 0xc0, 0xaf, '"', ')'};
  GLWASM_Text * tree = nullptr;
  GLWASM_ParseError error{};
  EXPECT_EQ(glwasm_text_parse(input, sizeof(input), nullptr, &error, &tree),
            GLWASM_ERR_CORRUPT);
  EXPECT_EQ(error.result, GLWASM_ERR_CORRUPT);
  EXPECT_EQ(tree, nullptr);
  EXPECT_STREQ(error.message, "malformed UTF-8 in text source");
  EXPECT_EQ(error.location.offset, 5u);
  EXPECT_EQ(error.location.line, 2u);
  EXPECT_EQ(error.location.column, 2u);
}

TEST(WasmTextParser, PreservesValidRawUtf8InString) {
  const std::string input = "(module (custom \"é🙂\"))";
  GLWASM_Text * tree = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_text_parse(reinterpret_cast<const uint8_t *>(input.data()),
                              input.size(), nullptr, &error, &tree), GLWASM_OK)
      << error.message;
  size_t length = 0;
  const uint8_t * bytes = glwasm_text_bytes(
      glwasm_text_at(glwasm_text_at(glwasm_text_at(tree, 0), 1), 1), &length);
  ASSERT_EQ(length, 6u);
  EXPECT_EQ(bytes[0], 0xc3u);
  EXPECT_EQ(bytes[1], 0xa9u);
  EXPECT_EQ(bytes[2], 0xf0u);
  EXPECT_EQ(bytes[3], 0x9fu);
  EXPECT_EQ(bytes[4], 0x99u);
  EXPECT_EQ(bytes[5], 0x82u);
  glwasm_text_destroy(tree);
}

TEST(WasmTextParser, RejectsInvalidUtf8ContinuationAndTruncation) {
  const uint8_t invalid_continuation[] = {'(', 'x', ' ', '"', 0xc2, 'A',
                                          '"', ')'};
  const uint8_t truncated[] = {'(', 'x', ' ', '"', 0xe2, 0x82, '"', ')'};
  const struct {
    const uint8_t * bytes;
    size_t length;
    uint32_t error_offset;
  } cases[] = {{invalid_continuation, sizeof(invalid_continuation), 5},
               {truncated, sizeof(truncated), 6}};
  for (const auto & input : cases) {
    GLWASM_Text * tree = nullptr;
    GLWASM_ParseError error{};
    EXPECT_EQ(glwasm_text_parse(input.bytes, input.length, nullptr, &error,
                                &tree), GLWASM_ERR_CORRUPT);
    EXPECT_EQ(error.result, GLWASM_ERR_CORRUPT);
    EXPECT_EQ(error.location.offset, input.error_offset);
    EXPECT_STREQ(error.message, "malformed UTF-8 in text source");
    EXPECT_EQ(tree, nullptr);
  }
}

TEST(WasmTextParser, RejectsStrayAndUnterminatedDelimiters) {
  for (const char * input : {" )", "(module", "(module \"bad\\q\")",
                             "(module \x01)"}) {
    GLWASM_Text * tree = nullptr;
    GLWASM_ParseError error{};
    EXPECT_EQ(glwasm_text_parse(reinterpret_cast<const uint8_t *>(input),
                                std::char_traits<char>::length(input), nullptr,
                                &error, &tree),
              GLWASM_ERR_CORRUPT);
    EXPECT_EQ(tree, nullptr);
  }
}

TEST(WasmTextParser, RejectsNestingBeyondTheDocumentedLimit) {
  for (const size_t depth : {257u, 258u}) {
    const std::string input = std::string(depth, '(') + "0" +
                              std::string(depth, ')');
    GLWASM_Text * tree = nullptr;
    GLWASM_ParseError error{};
    EXPECT_EQ(glwasm_text_parse(reinterpret_cast<const uint8_t *>(input.data()),
                                input.size(), nullptr, &error, &tree),
              GLWASM_ERR_CORRUPT);
    EXPECT_EQ(tree, nullptr);
    EXPECT_STREQ(error.message, "nesting limit exceeded");
  }
}

TEST(WasmTextParser, AcceptsTheDocumentedNestingLimit) {
  const std::string input = std::string(256, '(') + "0" +
                            std::string(256, ')');
  GLWASM_Text * tree = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_text_parse(reinterpret_cast<const uint8_t *>(input.data()),
                              input.size(), nullptr, &error, &tree), GLWASM_OK)
      << error.message;
  glwasm_text_destroy(tree);
}

TEST(WasmTextParser, HandlesStringContinuationsAndRejectsRawControls) {
  const std::string input = "(module (custom \"a\\\n  \\b\"))";
  GLWASM_Text * tree = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_text_parse(reinterpret_cast<const uint8_t *>(input.data()),
                              input.size(), nullptr, &error, &tree),
            GLWASM_OK)
      << error.message;
  size_t length = 0;
  const uint8_t * bytes = glwasm_text_bytes(
      glwasm_text_at(glwasm_text_at(glwasm_text_at(tree, 0), 1), 1), &length);
  ASSERT_EQ(length, 2u);
  EXPECT_EQ(bytes[0], 'a');
  EXPECT_EQ(bytes[1], 'b');
  glwasm_text_destroy(tree);

  const std::string bad = "(module (custom \"a\nb\"))";
  tree = nullptr;
  EXPECT_EQ(glwasm_text_parse(reinterpret_cast<const uint8_t *>(bad.data()),
                              bad.size(), nullptr, &error, &tree),
            GLWASM_ERR_CORRUPT);
}

TEST(WasmTextParser, DecodesUnicodeCodePointEscapesAsUtf8) {
  const std::string input = "(module (custom \"\\u{1f642}\"))";
  GLWASM_Text * tree = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_text_parse(reinterpret_cast<const uint8_t *>(input.data()),
                              input.size(), nullptr, &error, &tree),
            GLWASM_OK)
      << error.message;
  size_t length = 0;
  const uint8_t * bytes = glwasm_text_bytes(
      glwasm_text_at(glwasm_text_at(glwasm_text_at(tree, 0), 1), 1), &length);
  ASSERT_EQ(length, 4u);
  EXPECT_EQ(bytes[0], 0xf0u);
  EXPECT_EQ(bytes[1], 0x9fu);
  EXPECT_EQ(bytes[2], 0x99u);
  EXPECT_EQ(bytes[3], 0x82u);
  glwasm_text_destroy(tree);

  const std::string invalid = "(module (custom \"\\u{d800}\"))";
  EXPECT_EQ(glwasm_text_parse(reinterpret_cast<const uint8_t *>(invalid.data()),
                              invalid.size(), nullptr, &error, &tree),
            GLWASM_ERR_CORRUPT);
}

TEST(WasmTextParser, AllocationFailuresLeaveOutputUntouched) {
  const std::string input = "(module (func (param i32) (result i32) (local.get 0)))";
  bool reached_success = false;
  for (size_t fail_at = 1; fail_at < 64; ++fail_at) {
    FailAllocator state{0, fail_at};
    GLWASM_Allocator allocator{&state, fail_malloc, fail_calloc, fail_realloc,
                               fail_free};
    GLWASM_Text * tree = reinterpret_cast<GLWASM_Text *>(0x1);
    GLWASM_ParseError error{};
    const auto result = glwasm_text_parse(
        reinterpret_cast<const uint8_t *>(input.data()), input.size(),
        &allocator, &error, &tree);
    if (result == GLWASM_OK) {
      reached_success = true;
      glwasm_text_destroy(tree);
      break;
    }
    EXPECT_EQ(result, GLWASM_ERR_OOM) << "failure point " << fail_at;
    EXPECT_EQ(error.result, GLWASM_ERR_OOM) << "failure point " << fail_at;
    EXPECT_EQ(tree, reinterpret_cast<GLWASM_Text *>(0x1));
  }
  EXPECT_TRUE(reached_success);
}

TEST(WasmWatLowering, LowersFoldedNumericFunctionAndMatchesBinaryModule) {
  const std::string wat = "(module (func (export \"run\") (result i32) (i32.const 7)))";
  const uint8_t wasm[] = {0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
                          0x01,0x05,0x01,0x60,0x00,0x01,0x7f,
                          0x03,0x02,0x01,0x00,
                          0x07,0x07,0x01,0x03,0x72,0x75,0x6e,0x00,0x00,
                          0x0a,0x06,0x01,0x04,0x00,0x41,0x07,0x0b};
  GLWASM_Module *from_text=nullptr,*from_binary=nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&from_text),GLWASM_OK) << error.message;
  ASSERT_EQ(glwasm_module_decode(wasm,sizeof(wasm),nullptr,nullptr,nullptr,
      &from_binary),GLWASM_OK);
  FILE *a=tmpfile(),*b=tmpfile(); ASSERT_NE(a,nullptr); ASSERT_NE(b,nullptr);
  ASSERT_EQ(glwasm_module_dump(from_text,a),GLWASM_OK);
  ASSERT_EQ(glwasm_module_dump(from_binary,b),GLWASM_OK);
  rewind(a);rewind(b);
  int ca,cb; do {ca=fgetc(a);cb=fgetc(b);EXPECT_EQ(ca,cb);} while(ca!=EOF&&cb!=EOF);
  fclose(a);fclose(b);glwasm_module_destroy(from_text);glwasm_module_destroy(from_binary);
}

TEST(WasmWatLowering, PreservesTypeOnlyModuleAndIgnoresQuotedComments) {
  const std::string wat = ";; \\\"x\\\"y\n(; \\\"p\\\"q ;) (module (type (func)))";
  const uint8_t wasm[] = {0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
                          0x01,0x04,0x01,0x60,0x00,0x00};
  GLWASM_Module *from_text=nullptr,*from_binary=nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&from_text),GLWASM_OK) << error.message;
  ASSERT_EQ(glwasm_module_decode(wasm,sizeof(wasm),nullptr,nullptr,nullptr,
      &from_binary),GLWASM_OK);
  FILE *a=tmpfile(),*b=tmpfile();ASSERT_NE(a,nullptr);ASSERT_NE(b,nullptr);
  ASSERT_EQ(glwasm_module_dump(from_text,a),GLWASM_OK);
  ASSERT_EQ(glwasm_module_dump(from_binary,b),GLWASM_OK);
  rewind(a);rewind(b);int ca,cb;
  do {ca=fgetc(a);cb=fgetc(b);EXPECT_EQ(ca,cb);} while(ca!=EOF&&cb!=EOF);
  fclose(a);fclose(b);glwasm_module_destroy(from_text);glwasm_module_destroy(from_binary);
}

TEST(WasmWatLowering, NamedParameterLowersEquivalentToBinaryModule) {
  const std::string wat="(module (func (param $x i32) (result i32) (local.get $x)))";
  const uint8_t wasm[]={0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x01,0x06,0x01,0x60,0x01,0x7f,0x01,0x7f,
      0x03,0x02,0x01,0x00,0x0a,0x06,0x01,0x04,0x00,0x20,0x00,0x0b};
  GLWASM_Module *from_text=nullptr,*from_binary=nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&from_text),GLWASM_OK) << error.message;
  ASSERT_EQ(glwasm_module_decode(wasm,sizeof(wasm),nullptr,nullptr,nullptr,
      &from_binary),GLWASM_OK);
  FILE *a=tmpfile(),*b=tmpfile();ASSERT_NE(a,nullptr);ASSERT_NE(b,nullptr);
  ASSERT_EQ(glwasm_module_dump(from_text,a),GLWASM_OK);
  ASSERT_EQ(glwasm_module_dump(from_binary,b),GLWASM_OK);
  rewind(a);rewind(b);std::string atxt,btxt;int ca,cb;
  while((ca=fgetc(a))!=EOF)atxt.push_back(static_cast<char>(ca));
  while((cb=fgetc(b))!=EOF)btxt.push_back(static_cast<char>(cb));
  EXPECT_EQ(atxt,btxt);
  fclose(a);fclose(b);glwasm_module_destroy(from_text);glwasm_module_destroy(from_binary);
}

TEST(WasmWatLowering, LowersTagAndThrow) {
  const std::string wat="(module (tag $oops (param i32)) (func (param i32) local.get 0 throw $oops))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersInlineExportedTagAndThrow) {
  const std::string wat="(module (tag $e0 (export \"e0\")) (func (export \"throw\") (throw $e0)))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, TagTextMatchesBinaryModuleDump) {
  const std::string wat="(module (tag $e0 (export \"e0\")) (func (export \"throw\") (throw $e0)))";
  const uint8_t wasm[]={0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x01,0x07,0x02,0x60,0x00,0x00,0x60,0x00,0x00,
      0x03,0x02,0x01,0x00,0x0d,0x03,0x01,0x00,0x01,
      0x07,0x0e,0x02,0x02,'e','0',0x04,0x00,0x05,'t','h','r','o','w',0x00,0x00,
      0x0a,0x06,0x01,0x04,0x00,0x08,0x00,0x0b};
  GLWASM_Module *from_text=nullptr,*from_binary=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&from_text),GLWASM_OK) << error.message;
  ASSERT_EQ(glwasm_module_decode(wasm,sizeof(wasm),nullptr,nullptr,nullptr,
      &from_binary),GLWASM_OK);
  FILE *a=tmpfile(),*b=tmpfile();ASSERT_NE(a,nullptr);ASSERT_NE(b,nullptr);
  ASSERT_EQ(glwasm_module_dump(from_text,a),GLWASM_OK);
  ASSERT_EQ(glwasm_module_dump(from_binary,b),GLWASM_OK);
  rewind(a);rewind(b);std::string atxt,btxt;int ca,cb;
  while((ca=fgetc(a))!=EOF)atxt.push_back(static_cast<char>(ca));
  while((cb=fgetc(b))!=EOF)btxt.push_back(static_cast<char>(cb));
  EXPECT_EQ(atxt,btxt);
  fclose(a);fclose(b);glwasm_module_destroy(from_text);glwasm_module_destroy(from_binary);
}

TEST(WasmWatLowering, LowersTryTableCatchClauses) {
  const std::string wat="(module (tag $e) (func (block $exit (try_table (catch $e $exit) (unreachable)))))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersConcatenatedDataStrings) {
  const std::string wat="(module (memory 1 1) (data (i32.const 0) \"ab\" \"cd\"))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersMemoryInlineDataInitializer) {
  const std::string wat="(module (memory (data \"initialized\")))";
  const uint8_t wasm[]={0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x05,0x03,0x01,0x00,0x01,0x0b,0x11,0x01,0x00,0x41,0x00,0x0b,0x0b,
      'i','n','i','t','i','a','l','i','z','e','d'};
  GLWASM_Limits limits{};ASSERT_EQ(glwasm_limits_default(&limits),GLWASM_OK);
  limits.memory_maximum=UINT64_MAX;
  GLWASM_Module *binary=nullptr;
  ASSERT_EQ(glwasm_module_decode(wasm,sizeof(wasm),&limits,nullptr,nullptr,&binary),GLWASM_OK);
  glwasm_module_destroy(binary);
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),&limits,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersFoldedBranchWithValueAndCondition) {
  const std::string wat="(module (func (block (drop (i32.ctz (br_if 0 (i32.const 0) (i32.const 1)))))))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersNamedTableElementSegment) {
  const std::string wat="(module (table $t0 2 2 funcref) (func $f) (elem (table $t0) (i32.const 0) func $f))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersSaturatingFloatToIntegerConversion) {
  const std::string wat="(module (func (result i32) (i32.trunc_sat_f32_s (f32.const 1.0))))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersInlineMultiValueBlockType) {
  const std::string wat="(module (func (result i32 i64) (block (result i32 i64) (i32.const 0) (i64.const 1))))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersInlineBlockParameters) {
  const std::string wat="(module (func (param i32) (result i32) (local.get 0) (block (param i32) (result i32) (drop) (local.get 0))))";
  const uint8_t wasm[]={0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x01,0x0b,0x02,0x60,0x01,0x7f,0x01,0x7f,0x60,0x01,0x7f,0x01,0x7f,
      0x03,0x02,0x01,0x00,0x0a,0x0c,0x01,0x0a,0x00,0x20,0x00,0x02,0x01,0x1a,0x20,0x00,0x0b,0x0b};
  GLWASM_Module *binary=nullptr;
  ASSERT_EQ(glwasm_module_decode(wasm,sizeof(wasm),nullptr,nullptr,nullptr,&binary),GLWASM_OK);
  glwasm_module_destroy(binary);
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersMultiValueIfResult) {
  const std::string wat="(module (func (param i32) (result i32 i64) (if (result i32 i64) (local.get 0) (then (i32.const 1) (i64.const 2)) (else (i32.const 3) (i64.const 4)))))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersNamedMemoryAndTableCopySelectors) {
  const std::string wat="(module (memory $m0 1 1) (memory $m1 1 1) (table $t0 2 2 funcref) (table $t1 2 2 funcref) (func (memory.copy $m0 $m1 (i32.const 0) (i32.const 0) (i32.const 0)) (table.copy $t0 $t1 (i32.const 0) (i32.const 0) (i32.const 0))))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersMemoryTableAndExports) {
  const std::string wat="(module (table 1 1 funcref) (memory 1 1) (export \"tab\" (table 0)) (export \"mem\" (memory 0)))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersConstantGlobalsAndGlobalExports) {
  const std::string wat="(module (global (mut i32) (i32.const 3)) (export \"g\" (global 0)))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersGlobalReadsInFunctionBodies) {
  const std::string wat="(module (global (mut i32) (i32.const 3)) (func (result i32) global.get 0))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersLinearMemoryOperations) {
  const std::string wat="(module (memory 1 1) (func (param i32) (result i32) local.get 0 i32.load (offset 4) (align 4)))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersExplicitMultiMemoryAccess) {
  const std::string wat="(module (memory 1 1) (memory 1 1) (func (param i32) (result i32) local.get 0 i32.load (memory 1)))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersActiveElementAndDataSegments) {
  const std::string wat="(module (table 1 1 funcref) (memory 1 1) (func $f) (elem (i32.const 0) func $f) (data (i32.const 0) \"abc\"))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersPassiveDeclarativeAndExpressionElements) {
  const std::string wat="(module (table 1 1 funcref) (func $f) (elem func $f) (elem declare func $f) (elem funcref (ref.func $f)))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersInlineTableElementInitializer) {
  const std::string wat="(module (func $f) (func $g) (table $t funcref (elem (ref.func $f) (ref.null func) (ref.func $g))))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersExceptionReferenceInstructions) {
  const std::string wat="(module (func (param exnref) local.get 0 throw_ref))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersLegacyFlatCallIndirect) {
  const std::string wat="(module (table 1 funcref) "
      "(type $sig (func)) (func unreachable call_indirect (result)) "
      "(func unreachable (return_call_indirect 0 (type $sig))))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, PreservesCommentDelimitedModuleForm) {
  const std::string wat = "(;comment;)((;comment;)\n"
      "(;comment;)module(;comment;)\n(;comment;))(;comment;)";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersInlineAndExplicitTagImports) {
  const std::string wat = "(module "
      "(tag $first (import \"test\" \"one\") (param i32)) "
      "(import \"test\" \"two\" (tag $second (param i32 f32))))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersQuotedIdentifiersAsSingleNames) {
  const std::string wat="(module (func $\"a name\") "
      "(func (call $\"a name\")))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersExplicitTableAndMemorySegmentIndices) {
  const std::string wat="(module (table 1 1 funcref) (table 1 1 funcref) (memory 1 1) (memory 1 1) (func $f) (elem (table 1) (i32.const 0) func $f) (data (memory 1) (i32.const 0) \"x\"))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersPassiveDataAndBulkMemoryInstructions) {
  const std::string wat="(module (memory 1 1) (data \"abc\") (func (memory.init 0 (memory 0) (i32.const 0) (i32.const 0) (i32.const 3)) (data.drop 0)))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersBulkTableInstructions) {
  const std::string wat="(module (table $tab 1 1 funcref) (func $f (table.init $el (table $tab) (i32.const 0) (i32.const 0) (i32.const 1)) (table.copy $tab $tab (i32.const 0) (i32.const 0) (i32.const 1)) (elem.drop $el)) (elem $el (i32.const 0) func $f))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersMultipleTableSegmentsAndIndirectCalls) {
  const std::vector<std::string> modules = {
      R"((module (type (func (result i32)))
        (import "a" "f0" (func (result i32)))
        (table $t0 30 30 funcref) (table $t1 30 30 funcref)))",
      R"((module (type (func (result i32)))
        (import "a" "f0" (func (result i32)))
        (table $t0 30 30 funcref) (table $t1 30 30 funcref)
        (elem (table $t0) (i32.const 2) func 0)
        (elem funcref (ref.func 0))))",
      R"((module (type (func (result i32)))
        (import "a" "f0" (func (result i32)))
        (table $t0 30 30 funcref) (table $t1 30 30 funcref)
        (elem (table $t0) (i32.const 2) func 0)
        (elem funcref (ref.func 0))
        (func (export "check") (param i32) (result i32)
          (call_indirect $t0 (type 0) (local.get 0)))))"};
  for (size_t i = 0; i < modules.size(); ++i) {
  const std::string & wat = modules[i];
  GLWASM_Module * module = nullptr;
  GLWASM_ParseError error{};
  const GLWASM_Result result = glwasm_wat_module_parse(
      reinterpret_cast<const uint8_t *>(wat.data()), wat.size(), nullptr,
      nullptr, &error, &module);
  ASSERT_EQ(result, GLWASM_OK) << "case " << i << ": " << error.message << " (" << result << ")";
  glwasm_module_destroy(module);
  }
}

TEST(WasmWatLowering, LowersReferenceInstructionsAndTableAccess) {
  const std::string wat="(module (table 1 1 funcref) (func $f (drop (ref.func $f)) (drop (ref.null funcref)) (drop (table.get (i32.const 0)))) (elem (i32.const 0) func $f))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersFoldedBranchTable) {
  const std::string wat="(module (func (param i32) (block (br_table 0 0 (local.get 0)))))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersLinearParametersAndLocalReads) {
  const std::string wat="(module (func (param i32 i32) (result i32) (local i32) local.get 0 local.get 1 i32.add local.set 2 local.get 2))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message << " at " << error.location.line << ":" << error.location.column;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersLinearFloatingPointConstant) {
  const std::string wat="(module (func (result f32) f32.const 1.5))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, ResolvesNamedFunctionsAndCalls) {
  const std::string wat="(module (func $source (result i32) i32.const 9) (func $entry (result i32) call $source) (export \"entry\" (func $entry)))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersFunctionImportsAndImportedCallIndices) {
  const std::string wat="(module (import \"env\" \"identity\" (func $id (param i32) (result i32))) (func (export \"call\") (param i32) (result i32) local.get 0 call $id))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersTableMemoryAndGlobalImports) {
  const std::string wat="(module (import \"env\" \"tab\" (table 1 2 funcref)) (import \"env\" \"mem\" (memory 1 1)) (import \"env\" \"g\" (global (mut i32))) (func global.get 0 drop memory.size drop (table.size) drop))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersImportOnlyModuleWithoutFunctionTypes) {
  const std::string wat="(module (import \"env\" \"mem\" (memory 1 1)))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, SkipsOptionalModuleIdentifierDuringLowering) {
  const std::string wat="(module $named (func (export \"f\")))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, RejectsIllegalControlAndUtf8InQuotedSource) {
  for (const std::string & input : {
           std::string("(module (@a ") + char(0x19) + "))",
           std::string("(module (@a ") + char(0x80) + "))"}) {
    GLWASM_Module * module = nullptr;
    GLWASM_ParseError error{};
    EXPECT_EQ(glwasm_wat_module_parse(
                  reinterpret_cast<const uint8_t *>(input.data()),
                  input.size(), nullptr, nullptr, &error, &module),
              GLWASM_ERR_CORRUPT);
    EXPECT_EQ(module, nullptr);
    EXPECT_NE(error.message, nullptr);
  }
}

TEST(WasmWatLowering, ParsesNumericSeparatorsAsWatDigitSeparators) {
  const std::string wat =
      "(module (func (result i32) (i32.const 0_123_456)) "
      "(func (drop (f64.const 0_12_3.4_5e+0_2))) "
      "(func (drop (i64.const 0xffff_ffff_ffff_ffff)) "
      "(drop (f32.const nan:0x1))))";
  GLWASM_Module * module = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(
                reinterpret_cast<const uint8_t *>(wat.data()), wat.size(),
                nullptr, nullptr, &error, &module),
            GLWASM_OK)
      << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersNamedStartFunction) {
  const std::string wat="(module (func $init) (start $init))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersExplicitTypeAndIndirectCall) {
  const std::string wat="(module (type $sig (func (param i32) (result i32))) (table 1 1 funcref) (func (type $sig) local.get 0 local.get 0 call_indirect (type $sig)))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, ResolvesNamedFoldedAndLinearLabels) {
  for(const std::string wat : {
      "(module (func (block $outer (block $inner (br $outer)))))",
      "(module (func block $outer block $inner br $outer end end))"}) {
    GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
    ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
        wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << wat << ": " << error.message;
    glwasm_module_destroy(module);
  }
}

TEST(WasmWatLowering, LowersMultiValueFunctionAndTypedBlock) {
  for(const std::string wat : {
      "(module (func (result i32 i64) (i32.const 7) (i64.const 8)))",
      "(module (type $bt (func (result i32))) (func (result i32) (block $b (type $bt) (i32.const 8))))"}) {
    GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
    ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
        wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << wat << ": " << error.message;
    glwasm_module_destroy(module);
  }
}

TEST(WasmWatLowering, LowersMemory64AndTable64DeclarationsAndAddresses) {
  GLWASM_Limits limits{};
  ASSERT_EQ(glwasm_limits_default(&limits),GLWASM_OK);
  limits.memories=4;
  limits.tables=4;
  limits.memory_initial=UINT64_MAX;
  limits.memory_maximum=UINT64_MAX;
  limits.table_size=UINT64_MAX;
  for(const std::string wat : {
      "(module (memory i64 1 1) (func i64.const 0 i32.const 7 i32.store i64.const 0 drop memory.size drop))",
      "(module (memory i64 1 1) (func (i32.store offset=4294967296 (i64.const 0) (i32.const 3))))",
      "(module (memory 1) (memory i64 1 1) (func i64.const 0 i64.const 7 i64.store (memory 1) i64.const 0 drop memory.size 1 drop))",
      "(module (table i64 1 1 funcref) (func (drop (table.size 0))))",
      "(module (import \"env\" \"mem\" (memory i64 1 1)))",
      "(module (table (export \"t\") 1 funcref) (func))"}) {
    GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
    ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
        wat.size(),&limits,nullptr,&error,&module),GLWASM_OK) << wat << ": " << error.message;
    glwasm_module_destroy(module);
  }
  const std::string inline_export =
      "(module (memory (export \"m\") 1 1) (func))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(inline_export.data()),
      inline_export.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersFunctionImportTypeUse) {
  const std::string wat="(module (type $sig (func (param i32) (result i32))) (import \"env\" \"f\" (func $f (type $sig))) (func (type $sig) local.get 0 call $f))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, ResolvesNamedParametersAndLocals) {
  for(const std::string wat : {
      "(module (func (param $n i32) (result i32) (local $x i32) (local.set $x (local.get $n)) (local.get $x)))",
      "(module (type $sig (func (param i32) (result i32))) (func (type $sig) (param $n i32) (result i32) local.get $n))",
      "(module (func (result externref) (ref.null extern)))"}) {
    GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
    ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
        wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
    glwasm_module_destroy(module);
  }
  const std::string deferred =
      "(module (func (param $p (ref extern)) (result (ref extern)) "
      "(local $x (ref extern)) (local.set $x (local.get $p)) "
      "(local.get $x)))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  EXPECT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(deferred.data()),
      deferred.size(),nullptr,nullptr,&error,&module),GLWASM_ERR_UNSUPPORTED);
}

TEST(WasmWatLowering, LowersImportedGlobalDataOffsets) {
  const std::string wat="(module (import \"env\" \"off\" (global $g i32)) (memory 1 1) (data (global.get $g) \"a\"))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, AcceptsBrTableAfterUnreachableAcrossResultTypes) {
  const std::string wat="(module (func (block (result f64) (block (result f32) (unreachable) (br_table 0 1 1 (i32.const 1))) (drop) (f64.const 0)) (drop)))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
      wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message;
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, LowersFoldedAndLinearStructuredControl) {
  for(const std::string wat : {
      "(module (func (result i32) (block (result i32) (i32.const 3))))",
      "(module (func (result i32) block (result i32) i32.const 4 end))",
      "(module (func block br 0 end))"}) {
    GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
    ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
        wat.size(),nullptr,nullptr,&error,&module),GLWASM_OK) << error.message << "\nWAT: " << wat;
    glwasm_module_destroy(module);
  }
}

TEST(WasmWatLowering, RejectsMalformedInlineTypeUsesBeforeValidation) {
  for(const std::string wat : {
      "(module (type $sig (func (param i32) (result i32))) (func (i32.const 0) (block (type $sig) (result i32) (param i32))))",
      "(module (type $sig (func)) (table 1 funcref) (func (call_indirect (type $sig) (result i32) (i32.const 0))))",
      "(module (type $sig (func (param i32) (result i32))) (func (type 2) (param i32)))",
      "(module (func (i32.const 0) (block (param $x i32) (drop))))"}) {
    GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
    EXPECT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
        wat.size(),nullptr,nullptr,&error,&module),GLWASM_ERR_CORRUPT)
        << error.message << "\nWAT: " << wat;
    if(module)glwasm_module_destroy(module);
  }
}

TEST(WasmWatLowering, RejectsMalformedFloatAndHexPrefixes) {
  for(const std::string literal : {"1_e1","1e_1","1.0_e1","1.0e_1",
      ".0",".0e0","0X1p0"}) {
    const std::string wat="(module (global f64 (f64.const "+literal+")))";
    GLWASM_Module *module=nullptr;GLWASM_ParseError error{};
    EXPECT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t*>(wat.data()),
        wat.size(),nullptr,nullptr,&error,&module),GLWASM_ERR_CORRUPT)
        << error.message << "\nWAT: " << wat;
    if(module)glwasm_module_destroy(module);
  }
}

TEST(WasmWatLowering, AcceptsPinnedCorpusHexFloatsWithoutExponent) {
  const std::string wat =
      "(module (global f32 (f32.const 0xa0f0099)))";
  GLWASM_Module *module = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(wat.data()),
      wat.size(), nullptr, nullptr, &error, &module), GLWASM_OK)
      << error.message;
  ASSERT_NE(module, nullptr);
  ASSERT_EQ(module->nglobals, 1u);
  ASSERT_GE(module->globals[0].init.nins, 1u);
  EXPECT_EQ(module->globals[0].init.ins[0].op, 0x43u);
  EXPECT_EQ(module->globals[0].init.ins[0].a, UINT32_C(0x4d20f00a));
  glwasm_module_destroy(module);
}

TEST(WasmWatLowering, RejectsUppercaseHexIntegerPrefix) {
  const std::string wat = "(module (global i32 (i32.const 0X1)))";
  GLWASM_Module *module = nullptr;
  GLWASM_ParseError error{};
  EXPECT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(wat.data()),
      wat.size(), nullptr, nullptr, &error, &module), GLWASM_ERR_CORRUPT)
      << error.message;
  if (module) glwasm_module_destroy(module);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
