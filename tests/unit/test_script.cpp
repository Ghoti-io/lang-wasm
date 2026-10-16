/* SPDX-License-Identifier: LGPL-3.0-only */
#include <ghoti.io/lang-wasm/script.h>
#include <ghoti.io/cutil/allocator.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

namespace {
struct FailAllocator { size_t calls = 0; size_t fail_at = 0; };
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

TEST(WasmScriptParser, TypesEveryScriptCommandAndKeepsLocations) {
  const std::string text =
      "(module)\n(func)\n(module binary \"\\00\\61\\73\\6d\\01\\00\\00\\00\")\n"
      "(module quote \"(module)\")\n(register \"spectest\")\n"
      "(invoke \"f\") (get \"g\")\n"
      "(assert_return (invoke \"f\") (i32.const 0))\n"
      "(assert_return_canonical_nan (invoke \"f\"))\n"
      "(assert_return_arithmetic_nan (invoke \"f\"))\n"
      "(assert_trap (invoke \"f\") \"trap\")\n"
      "(assert_exhaustion (invoke \"f\") \"stack\")\n"
      "(assert_exception (invoke \"f\"))\n"
      "(assert_malformed (module) \"bad\")\n"
      "(assert_invalid (module) \"bad\")\n"
      "(assert_unlinkable (module) \"bad\")\n"
      "(assert_uninstantiable (module) \"bad\")\n"
      "(assert_suspension (invoke \"f\"))\n"
      "(assert_malformed_custom (module) \"bad\")\n"
      "(assert_invalid_custom (module) \"bad\")\n"
      "(invoke \"f\" (i32.const -0x1) (f32.const 1.25e-2)"
      " (ref.null) (ref.func))";
  GLWASM_Script * script = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_script_parse(reinterpret_cast<const uint8_t *>(text.data()),
                                text.size(), nullptr, &error, &script),
            GLWASM_OK)
      << error.message;
  ASSERT_EQ(glwasm_script_size(script), 21u);
  GLWASM_Command command{};
  ASSERT_EQ(glwasm_script_at(script, 1, &command), GLWASM_OK);
  EXPECT_EQ(command.kind, GLWASM_COMMAND_MODULE_FIELD);
  ASSERT_EQ(glwasm_script_at(script, 2, &command), GLWASM_OK);
  EXPECT_EQ(command.kind, GLWASM_COMMAND_MODULE_BINARY);
  EXPECT_EQ(command.location.line, 3u);
  ASSERT_EQ(glwasm_script_at(script, 17, &command), GLWASM_OK);
  EXPECT_EQ(command.kind, GLWASM_COMMAND_ASSERT_SUSPENSION);
  ASSERT_EQ(glwasm_script_at(script, 19, &command), GLWASM_OK);
  EXPECT_EQ(command.kind, GLWASM_COMMAND_ASSERT_INVALID_CUSTOM);
  glwasm_script_destroy(script);
}

TEST(WasmScriptParser, RejectsUnknownAndMalformedCommands) {
  for (const char * source : {
           "(ignored 1)", "(assert_future (invoke))", "(register)",
           "(invoke 1)", "(assert_invalid (module))",
           "(module binary \"bytes\" (not-a-string))", "(get \"g\" 1)",
           "(invoke \"f\" 1)", "(assert_trap (bogus) \"trap\")",
           "(invoke \"f\" (i32.const nope))",
           "(invoke \"f\" (i64.const 0x))",
           "(invoke \"f\" (i32.const 0X1))",
           "(invoke \"f\" (f32.const 1e))",
           "(invoke \"f\" (f32.const 0X1p0))",
           "(invoke \"f\" (ref.func nope))",
           "(invoke \"f\" (ref.extern func))",
           "(assert_return_canonical_nan (module))",
           "(invoke \"f\" (i32.add))",
           "(assert_return (invoke \"f\") (bogus))", "(start)",
           "(start banana)", "(export)", "(export \"x\" ())",
           "(module definition)", "(global banana)",
           "(global i32 (bogus))", "(type (func (param)))",
           "(tag banana)", "(data nope)", "(elem nope)",
           "(elem $f)", "(elem 0)", "(memory 1 2 3)",
           "(data (i32.const) \"x\")",
           "(data \"x\" (i32.const 0))", "(memory 1 shared 2)",
           "(elem (item (ref.func)))",
           "(elem func $f func $g)",
           "(func (param banana))",
           "(func (import \"m\" \"f\") (i32.const 0))",
           "(func (i32.const 0) (import \"m\" \"f\"))",
           "(import \"m\" \"mem\" (memory 1 shared))",
           "(memory 1 2 shared shared)",
           "(import \"m\" \"n\" ())",
           "(import \"m\" \"n\" (memory))",
           "(import \"m\" \"n\" (memory nope))",
           "(import \"m\" \"n\" (table))",
           "(import \"m\" \"n\" (table nope nope))",
           "(import \"m\" \"n\" (func nonsense))",
           "(import \"m\" \"n\" (func (param banana)))",
           "(import \"m\" \"n\" (func (param)))",
           "(import \"m\" \"n\" (func (result)))",
           "(import \"m\" \"n\" (func (param (ref banana))))",
           "(import \"m\" \"n\" (global banana))",
           "(import \"m\" \"n\" (tag (param banana)))",
           "(export \"x\" (func nope))", "(memory nope)",
           "(type banana)"}) {
    GLWASM_Script * script = nullptr;
    GLWASM_ParseError error{};
    EXPECT_EQ(glwasm_script_parse(reinterpret_cast<const uint8_t *>(source),
                                  std::char_traits<char>::length(source),
                                  nullptr, &error, &script),
              GLWASM_ERR_CORRUPT) << source;
    EXPECT_EQ(error.result, GLWASM_ERR_CORRUPT) << source;
    EXPECT_EQ(script, nullptr);
    EXPECT_STREQ(error.message, "unknown or malformed WAST command");
  }
}

TEST(WasmScriptParser, ReportsCommandColumnAfterLeadingText) {
  const std::string source = "(module)  (invoke \"f\")";
  GLWASM_Script * script = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_script_parse(reinterpret_cast<const uint8_t *>(source.data()),
                                source.size(), nullptr, &error, &script),
            GLWASM_OK) << error.message;
  GLWASM_Command command{};
  ASSERT_EQ(glwasm_script_at(script, 1, &command), GLWASM_OK);
  EXPECT_EQ(command.location.offset, 10u);
  EXPECT_EQ(command.location.line, 1u);
  EXPECT_EQ(command.location.column, 11u);
  glwasm_script_destroy(script);
}

TEST(WasmScriptParser, ReportsMalformedCommandLocation) {
  const std::string source = "(module)\n\n (invoke \"f\" 1)";
  GLWASM_Script * script = nullptr;
  GLWASM_ParseError error{};
  EXPECT_EQ(glwasm_script_parse(reinterpret_cast<const uint8_t *>(source.data()),
                                source.size(), nullptr, &error, &script),
            GLWASM_ERR_CORRUPT);
  EXPECT_EQ(script, nullptr);
  EXPECT_EQ(error.location.offset, 11u);
  EXPECT_EQ(error.location.line, 3u);
  EXPECT_EQ(error.location.column, 2u);
}

TEST(WasmScriptParser, IgnoresScriptAnnotationsWhenFindingCommandKind) {
  const std::string source = "((@a) module (@a) $m (@a))";
  GLWASM_Script * script = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(
      glwasm_script_parse(reinterpret_cast<const uint8_t *>(source.data()),
                          source.size(), nullptr, &error, &script),
      GLWASM_OK)
      << error.message;
  GLWASM_Command command{};
  ASSERT_EQ(glwasm_script_at(script, 0, &command), GLWASM_OK);
  EXPECT_EQ(command.kind, GLWASM_COMMAND_MODULE_TEXT);
  glwasm_script_destroy(script);
}

TEST(WasmScriptParser, DistinguishesModuleDefinitionsAndInstances) {
  const std::string source =
      "(module definition $M (func))\n(module instance $I $M)";
  GLWASM_Script * script = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_script_parse(reinterpret_cast<const uint8_t *>(source.data()),
                                source.size(), nullptr, &error, &script),
            GLWASM_OK)
      << error.message;
  ASSERT_EQ(glwasm_script_size(script), 2u);
  GLWASM_Command command{};
  ASSERT_EQ(glwasm_script_at(script, 0, &command), GLWASM_OK);
  EXPECT_EQ(command.kind, GLWASM_COMMAND_MODULE_DEFINITION);
  ASSERT_EQ(glwasm_script_at(script, 1, &command), GLWASM_OK);
  EXPECT_EQ(command.kind, GLWASM_COMMAND_MODULE_INSTANCE);
  glwasm_script_destroy(script);
}

TEST(WasmScriptParser, AcceptsValidFieldDescriptors) {
  const std::string source =
      "(export \"f\" (func $f))"
      "(export \"t\" (table 0))"
      "(export \"m\" (memory 0))"
      "(export \"g\" (global 0))"
      "(export \"e\" (tag 0))"
      "(import \"m\" \"f\" (func $f (param $x i32)"
      "(param (ref null func)) (result i64)))"
      "(import \"m\" \"mem\" (memory $mem 1 2))"
      "(import \"m\" \"table\" (table $t 1 funcref))"
      "(import \"m\" \"memory64\" (memory i64 1 2))"
      "(import \"m\" \"shared\" (memory 1 2 shared))"
      "(import \"m\" \"table64\" (table i64 1 2 (ref null func)))"
      "(import \"m\" \"typed\" (table 1 2 (ref null extern)))"
      "(import \"m\" \"typed-function\" (func (type 0)))"
      "(import \"m\" \"typed-tag\" (tag (type 0)))"
      "(type (func $inner))"
      "(import \"m\" \"g\" (global i32))"
      "(import \"m\" \"gm\" (global $g (mut i32)))"
      "(import \"m\" \"tag\" (tag (param i32)))";
  const std::string script_source = source + "(global i32 (i32.const 0))"
                                             "(func $valid (param $x i32)"
                                             " (result i32) (i32.const 0))"
                                             "(func $inline-import (import \"m\" \"f\")"
                                             " (param i32) (result i32))"
                                             "(data \"x\")"
                                             "(elem declare func $f)"
                                             "(elem (i32.const 0) func $f)"
                                             "(memory 1 2)"
                                             "(data (i32.const 0) \"x\")"
                                             "(data (global.get $g) \"x\")";
  GLWASM_Script * script = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_script_parse(
                reinterpret_cast<const uint8_t *>(script_source.data()),
                script_source.size(), nullptr, &error, &script),
            GLWASM_OK) << error.message;
  ASSERT_EQ(glwasm_script_size(script), 27u);
  GLWASM_Command command{};
  for (size_t i = 0; i < glwasm_script_size(script); ++i) {
    ASSERT_EQ(glwasm_script_at(script, i, &command), GLWASM_OK);
    EXPECT_EQ(command.kind, GLWASM_COMMAND_MODULE_FIELD);
  }
  glwasm_script_destroy(script);
}

TEST(WasmScriptParser, AllocationFailuresLeaveOutputUntouched) {
  const std::string input = "(module (func)) (invoke \"f\")";
  bool reached_success = false;
  for (size_t fail_at = 1; fail_at < 64; ++fail_at) {
    FailAllocator state{0, fail_at};
    GLWASM_Allocator allocator{&state, fail_malloc, fail_calloc, fail_realloc,
                               fail_free};
    GLWASM_Script * script = reinterpret_cast<GLWASM_Script *>(0x1);
    GLWASM_ParseError error{};
    const auto result = glwasm_script_parse(
        reinterpret_cast<const uint8_t *>(input.data()), input.size(),
        &allocator, &error, &script);
    if (result == GLWASM_OK) {
      reached_success = true;
      glwasm_script_destroy(script);
      break;
    }
    EXPECT_EQ(result, GLWASM_ERR_OOM) << "failure point " << fail_at;
    EXPECT_EQ(error.result, GLWASM_ERR_OOM) << "failure point " << fail_at;
    EXPECT_EQ(script, reinterpret_cast<GLWASM_Script *>(0x1));
  }
  EXPECT_TRUE(reached_success);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
