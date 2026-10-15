/*
 * SPDX-License-Identifier: LGPL-3.0-only
 * Copyright (C) 2026 Corey Pennycuff
 *
 * Story 2 parse gate for the pinned WebAssembly test suite. This pass
 * recognizes every command syntactically and deliberately does not count
 * executable assertions as passed before Story 3 provides an engine adapter.
 */
#include <ghoti.io/lang-wasm/parse.h>
#include <ghoti.io/lang-wasm/script.h>
#include <ghoti.io/lang-wasm/decode.h>
#include <ghoti.io/lang-wasm/parse.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <limits>
#include <vector>

namespace fs = std::filesystem;

namespace {
std::string read_file(const fs::path & path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("cannot read " + path.string());
  std::ostringstream out;
  out << input.rdbuf();
  return out.str();
}

std::string text_bytes(const GLWASM_Text * node) {
  size_t length = 0;
  const uint8_t * bytes = glwasm_text_bytes(node, &length);
  return bytes ? std::string(reinterpret_cast<const char *>(bytes), length)
               : std::string();
}

std::string source_form_at(const std::string & source, GLWASM_SourceLoc loc) {
  size_t start = 0;
  for (uint32_t line = 1; line < loc.line && start < source.size(); ++line) {
    const size_t next = source.find('\n', start);
    if (next == std::string::npos)
      return {};
    start = next + 1;
  }
  if (loc.column == 0 || start + loc.column - 1 > source.size())
    return {};
  start += loc.column - 1;
  const size_t annotated_start = source.rfind("((@", start);
  if (annotated_start != std::string::npos) {
    const size_t module = source.find("module", annotated_start);
    if (module != std::string::npos && module <= start + 8)
      start = annotated_start;
  }
  if (source[start] != '(' ||
      (start + 1 < source.size() && source[start + 1] == ';')) {
    std::vector<size_t> opens;
    size_t block_comment = 0;
    bool string = false, escape = false, line_comment = false;
    for (size_t i = 0; i < start; ++i) {
      const char c = source[i];
      if (line_comment) {
        if (c == '\n') line_comment = false;
        continue;
      }
      if (block_comment) {
        if (c == '(' && i + 1 < start && source[i + 1] == ';') {
          ++block_comment; ++i;
        }
        else if (c == ';' && i + 1 < start && source[i + 1] == ')') {
          --block_comment; ++i;
        }
        continue;
      }
      if (string) {
        if (escape) escape = false;
        else if (c == '\\') escape = true;
        else if (c == '"') string = false;
        continue;
      }
      if (c == ';' && i + 1 < start && source[i + 1] == ';') {
        line_comment = true; ++i; continue;
      }
      if (c == '(' && i + 1 < start && source[i + 1] == ';') {
        block_comment = 1; ++i; continue;
      }
      if (c == '"') string = true;
      else if (c == '(') opens.push_back(i);
      else if (c == ')' && !opens.empty()) opens.pop_back();
    }
    if (opens.empty()) return {};
    start = opens.back();
  }
  size_t depth = 0, block_comment = 0;
  bool string = false, escape = false, line_comment = false;
  for (size_t i = start; i < source.size(); ++i) {
    const char c = source[i];
    if (line_comment) {
      if (c == '\n')
        line_comment = false;
      continue;
    }
    if (block_comment) {
      if (c == '(' && i + 1 < source.size() && source[i + 1] == ';') {
        ++block_comment;
        ++i;
      }
      else if (c == ';' && i + 1 < source.size() && source[i + 1] == ')') {
        --block_comment;
        ++i;
      }
      continue;
    }
    if (string) {
      if (escape)
        escape = false;
      else if (c == '\\')
        escape = true;
      else if (c == '"')
        string = false;
      continue;
    }
    if (c == ';' && i + 1 < source.size() && source[i + 1] == ';') {
      line_comment = true;
      ++i;
      continue;
    }
    if (c == '(' && i + 1 < source.size() && source[i + 1] == ';') {
      block_comment = 1;
      ++i;
      continue;
    }
    if (c == '"') {
      string = true;
      continue;
    }
    if (c == '(')
      ++depth;
    else if (c == ')' && --depth == 0)
      return source.substr(start, i - start + 1);
  }
  return {};
}

const GLWASM_Text * semantic_child(const GLWASM_Text * node, size_t wanted) {
  size_t i, seen = 0;
  for (i = 0; node && i < glwasm_text_size(node); ++i) {
    const GLWASM_Text * child = glwasm_text_at(node, i);
    if (glwasm_text_kind(child) == GLWASM_TEXT_LIST &&
        glwasm_text_size(child) &&
        text_bytes(glwasm_text_at(child, 0)).rfind("@", 0) == 0)
      continue;
    if (seen++ == wanted)
      return child;
  }
  return nullptr;
}

bool atom_is(const GLWASM_Text * node, const char * value) {
  return glwasm_text_kind(node) == GLWASM_TEXT_ATOM &&
         text_bytes(node) == value;
}

bool check_binary_validation_assertion(const GLWASM_Command & command,
                                       GLWASM_Result expected) {
  const GLWASM_Text * module = semantic_child(command.syntax, 1);
  std::vector<uint8_t> binary;
  bool in_binary = false;
  GLWASM_Module * decoded = nullptr;
  GLWASM_Failure failure{};
  GLWASM_Limits limits{};
  GLWASM_Result result;
  if (!module || glwasm_text_kind(module) != GLWASM_TEXT_LIST)
    return false;
  for (size_t i = 1; i < glwasm_text_size(module); ++i) {
    const GLWASM_Text * part = glwasm_text_at(module, i);
    if (atom_is(part, "binary")) {
      in_binary = true;
      continue;
    }
    if (in_binary && glwasm_text_kind(part) == GLWASM_TEXT_STRING) {
      size_t n = 0;
      const uint8_t * bytes = glwasm_text_bytes(part, &n);
      binary.insert(binary.end(), bytes, bytes + n);
      continue;
    }
    if (in_binary)
      return false;
  }
  if (!in_binary)
    return false;
  limits.module_bytes = std::numeric_limits<uint64_t>::max();
  limits.types = limits.imports = limits.exports = limits.functions =
      limits.globals = limits.tables = limits.memories = limits.tags =
          limits.data_segments = limits.element_segments = limits.locals =
              limits.instructions = limits.nesting = limits.br_table =
                  limits.name_length = std::numeric_limits<uint32_t>::max();
  limits.table_size = limits.memory_maximum = limits.memory_initial =
      std::numeric_limits<uint64_t>::max();
  result = glwasm_module_decode(binary.data(), binary.size(), &limits, nullptr,
                                &failure, &decoded);
  if (decoded)
    glwasm_module_destroy(decoded);
  if (result != expected)
    std::cerr << "binary assertion mismatch at line " << command.location.line
              << ": expected " << expected << " got " << result
              << (failure.limit ? std::string(" limit=") + failure.limit
                                : std::string())
              << '\n';
  return result == expected;
}

std::string text_module_source(const std::string & source,
                              const GLWASM_Text * candidate) {
  if (!candidate || glwasm_text_kind(candidate) != GLWASM_TEXT_LIST)
    return {};
  const GLWASM_Text * mode = semantic_child(candidate, 1);
  if (atom_is(mode, "quote")) {
    std::string quoted;
    for (size_t i = 2; i < glwasm_text_size(candidate); ++i) {
      const GLWASM_Text * fragment = semantic_child(candidate, i);
      if (!fragment || glwasm_text_kind(fragment) != GLWASM_TEXT_STRING)
        return {};
      quoted += text_bytes(fragment);
    }
    if (quoted.rfind("(module", 0) != 0)
      quoted = "(module " + quoted + ")";
    return quoted;
  }
  return source_form_at(source, glwasm_text_location(candidate));
}

std::map<std::string, std::string> classifications(const fs::path & path) {
  std::ifstream input(path);
  if (!input)
    throw std::runtime_error("cannot read classification file " +
                             path.string());
  std::map<std::string, std::string> result;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    const size_t tab = line.find('\t');
    if (tab == std::string::npos)
      throw std::runtime_error("malformed classification row: " + line);
    result.emplace(line.substr(0, tab), line.substr(tab + 1));
  }
  return result;
}

std::map<std::string, std::map<uint32_t, std::string>>
assertion_skips(const fs::path & path) {
  std::ifstream input(path);
  if (!input)
    throw std::runtime_error("cannot read assertion skip file " +
                             path.string());
  std::map<std::string, std::map<uint32_t, std::string>> result;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    std::istringstream fields(line);
    std::string file, line_text, reason, extra;
    if (!std::getline(fields, file, '\t') ||
        !std::getline(fields, line_text, '\t') ||
        !std::getline(fields, reason, '\t') ||
        std::getline(fields, extra, '\t'))
      throw std::runtime_error("malformed assertion skip row: " + line);
    const unsigned long parsed = std::stoul(line_text);
    if (parsed == 0 || parsed > UINT32_MAX ||
        !result[file].emplace(static_cast<uint32_t>(parsed), reason).second)
      throw std::runtime_error("duplicate or invalid assertion skip row: " +
                               line);
  }
  return result;
}
} // namespace

TEST(WastCorpus, EveryPinnedFileParsesAndEveryCommandIsRecognized) {
  const char * root_env = std::getenv("GLWASM_WAST_CORPUS_ROOT");
  const char * classification_env = std::getenv("GLWASM_WAST_CLASSIFICATION");
  const char * skips_env = std::getenv("GLWASM_WAST_ASSERTION_SKIPS");
  ASSERT_NE(root_env, nullptr) << "Makefile must set GLWASM_WAST_CORPUS_ROOT";
  ASSERT_NE(classification_env, nullptr)
      << "Makefile must set GLWASM_WAST_CLASSIFICATION";
  ASSERT_NE(skips_env, nullptr)
      << "Makefile must set GLWASM_WAST_ASSERTION_SKIPS";
  const fs::path root(root_env);
  GLWASM_Limits wat_limits{};
  ASSERT_EQ(glwasm_limits_default(&wat_limits), GLWASM_OK);
  wat_limits.memory_maximum = wat_limits.memory_initial =
      std::numeric_limits<uint64_t>::max();
  wat_limits.table_size = std::numeric_limits<uint64_t>::max();
  const auto classes = classifications(classification_env);
  const auto per_assertion = assertion_skips(skips_env);
  size_t parsed_files = 0, run_files = 0, pending = 0, commands = 0;
  size_t binary_validation_checked = 0, binary_validation_passed = 0;
  size_t text_malformed_checked = 0, text_malformed_passed = 0;
  size_t text_invalid_checked = 0, text_invalid_passed = 0;
  size_t wat_modules_checked = 0, wat_modules_passed = 0,
         deferred_wat_modules = 0;
  size_t wat_modules_in_scope = 0, wat_in_scope_passed = 0;
  std::vector<std::string> wat_module_failures;
  std::map<std::string, size_t> skips;
  std::map<std::string, size_t> assertion_skip_counts;
  std::set<std::pair<std::string, uint32_t>> consumed_assertion_skips;
  std::vector<std::string> failures;
  std::vector<fs::path> files;
  for (const auto & entry : fs::recursive_directory_iterator(root)) {
    if (entry.is_regular_file() && entry.path().extension() == ".wast")
      files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());

  for (const fs::path & path : files) {
    const std::string relative =
        "test/" + fs::relative(path, root).generic_string();
    const auto classification = classes.find(relative);
    if (classification == classes.end()) {
      failures.push_back(relative + ": missing classification");
      continue;
    }
    if (classification->second == "run")
      ++run_files;
    else if (classification->second.rfind("skip(", 0) == 0 &&
             classification->second.back() == ')')
      ++skips[classification->second.substr(5,
                                            classification->second.size() - 6)];
    else {
      failures.push_back(relative + ": invalid classification " +
                         classification->second);
      continue;
    }

    const std::string source = read_file(path);
    GLWASM_ParseError error{};
    GLWASM_Script * script = nullptr;
    const auto * bytes = reinterpret_cast<const uint8_t *>(source.data());
    GLWASM_Result result =
        glwasm_script_parse(bytes, source.size(), nullptr, &error, &script);
    // The three non-core custom-annotation files contain runner extensions
    // outside CAP-2. They remain syntax-checked, but are not fed to the strict
    // core command parser.
    GLWASM_Text * syntax = nullptr;
    const bool non_core = classification->second == "skip(non-core)";
    if (result != GLWASM_OK && non_core) {
      result =
          glwasm_text_parse(bytes, source.size(), nullptr, &error, &syntax);
    }
    if (result != GLWASM_OK) {
      failures.push_back(relative + ":" + std::to_string(error.location.line) +
                         ":" + std::to_string(error.location.column) + ": " +
                         (error.message ? error.message : "parse failed"));
      continue;
    }
    ++parsed_files;
    if (script) {
      for (size_t i = 0; i < glwasm_script_size(script); ++i) {
        GLWASM_Command command{};
        ASSERT_EQ(glwasm_script_at(script, i, &command), GLWASM_OK);
        ++commands;
        if (command.kind == GLWASM_COMMAND_MODULE_TEXT) {
          const auto file_skips = per_assertion.find(relative);
          const auto line_skip = file_skips == per_assertion.end()
              ? std::map<uint32_t, std::string>::const_iterator{}
              : file_skips->second.find(command.location.line);
          if (file_skips != per_assertion.end() &&
              line_skip != file_skips->second.end()) {
            const std::string & reason = line_skip->second;
            if (reason.rfind("skip(", 0) != 0 || reason.back() != ')') {
              failures.push_back(relative + ":" +
                  std::to_string(command.location.line) +
                  ": invalid deferred module skip reason");
            }
            else {
              ++deferred_wat_modules;
              ++assertion_skip_counts[reason.substr(5, reason.size() - 6)];
              consumed_assertion_skips.emplace(relative, command.location.line);
            }
            continue;
          }
          const bool in_scope = classification->second == "run";
          const std::string form = source_form_at(source, command.location);
          if (form.empty()) {
            const std::string message =
                relative + ":" + std::to_string(command.location.line) +
                ": could not recover module source form";
            wat_module_failures.push_back(in_scope ? "in-scope " + message
                : classification->second + " " + message);
            if (in_scope)
              failures.push_back("in-scope WAT: " + message);
          }
          else {
            GLWASM_ParseError wat_error{};
            GLWASM_Module * lowered = nullptr;
            ++wat_modules_checked;
            if (in_scope)
              ++wat_modules_in_scope;
            const GLWASM_Result wat_result = glwasm_wat_module_parse(
                reinterpret_cast<const uint8_t *>(form.data()), form.size(),
                &wat_limits, nullptr, &wat_error, &lowered);
            if (lowered)
              glwasm_module_destroy(lowered);
            if (wat_result == GLWASM_OK) {
              ++wat_modules_passed;
              if (in_scope)
                ++wat_in_scope_passed;
            }
            else {
              const std::string message =
                  relative + ":" + std::to_string(command.location.line) +
                  ":wat-" + std::to_string(wat_error.location.line) + ":" +
                  std::to_string(wat_error.location.column) + ": " +
                  (wat_error.message ? wat_error.message
                                     : "WAT module lowering failed") +
                  " [result " + std::to_string(wat_result) + "]";
              wat_module_failures.push_back(in_scope ? "in-scope " + message
                  : classification->second + " " + message);
              if (in_scope)
                failures.push_back("in-scope WAT: " + message);
            }
          }
        }
        if (command.kind >= GLWASM_COMMAND_ASSERT_RETURN) {
          const std::string & file_class = classification->second;
          const auto file_skips = per_assertion.find(relative);
          const auto line_skip =
              file_skips == per_assertion.end()
                  ? std::map<uint32_t, std::string>::const_iterator{}
                  : file_skips->second.find(command.location.line);
          if (file_class.rfind("skip(", 0) == 0 && file_class.back() == ')') {
            ++assertion_skip_counts[file_class.substr(
                5, file_class.size() - 6)];
          }
          else if (file_skips != per_assertion.end() &&
              line_skip != file_skips->second.end()) {
            ++assertion_skip_counts[line_skip->second.substr(
                5, line_skip->second.size() - 6)];
            consumed_assertion_skips.emplace(relative, command.location.line);
          }
          else if (command.kind == GLWASM_COMMAND_ASSERT_MALFORMED ||
                   command.kind == GLWASM_COMMAND_ASSERT_INVALID) {
            const GLWASM_Result expected =
                command.kind == GLWASM_COMMAND_ASSERT_MALFORMED
                    ? GLWASM_ERR_CORRUPT
                    : GLWASM_ERR_VALIDATE;
            const bool checked =
                check_binary_validation_assertion(command, expected);
            // Text forms require the text grammar/lowering work in Story 2;
            // binary forms exercise the existing decode+validate pipeline.
            if (checked) {
              ++binary_validation_checked;
              ++binary_validation_passed;
            }
            else if (command.kind == GLWASM_COMMAND_ASSERT_MALFORMED ||
                     command.kind == GLWASM_COMMAND_ASSERT_INVALID) {
              const GLWASM_Text * candidate =
                  semantic_child(command.syntax, 1);
              bool has_binary = false;
              for (size_t j = 0; candidate &&
                                 j < glwasm_text_size(candidate); ++j)
                has_binary |= atom_is(glwasm_text_at(candidate, j), "binary");
              if (has_binary) {
                failures.push_back(relative + ":" +
                                   std::to_string(command.location.line) +
                                   ": binary validation assertion produced an unexpected result");
              }
              else {
                const std::string form = text_module_source(source, candidate);
                GLWASM_ParseError text_error{};
                GLWASM_Module * lowered = nullptr;
                const bool quoted = atom_is(semantic_child(candidate, 1), "quote");
                const GLWASM_Result text_result = form.empty()
                    ? GLWASM_ERR_INVALID
                    : (quoted ? glwasm_wat_module_parse_verbatim
                              : glwasm_wat_module_parse)(
                          reinterpret_cast<const uint8_t *>(form.data()),
                          form.size(), &wat_limits, nullptr, &text_error,
                          &lowered);
                if (lowered)
                  glwasm_module_destroy(lowered);
                const bool expected_text_result =
                    command.kind == GLWASM_COMMAND_ASSERT_MALFORMED
                        ? text_result == GLWASM_ERR_CORRUPT
                        : text_result == GLWASM_ERR_VALIDATE;
                if (command.kind == GLWASM_COMMAND_ASSERT_MALFORMED)
                  ++text_malformed_checked;
                else
                  ++text_invalid_checked;
                if (expected_text_result) {
                  if (command.kind == GLWASM_COMMAND_ASSERT_MALFORMED)
                    ++text_malformed_passed;
                  else
                    ++text_invalid_passed;
                }
                else {
                  failures.push_back(
                      relative + ":" + std::to_string(command.location.line) +
                      ": text assertion expected " +
                      (command.kind == GLWASM_COMMAND_ASSERT_MALFORMED
                           ? "malformed"
                           : "invalid") +
                      ", got result " + std::to_string(text_result) +
                      (text_error.message ? ": " +
                           std::string(text_error.message)
                                         : std::string()));
                }
              }
            }
          }
          else
            ++pending;
        }
      }
      glwasm_script_destroy(script);
    }
    else {
      for (size_t i = 0; i < glwasm_text_size(syntax); ++i) {
        const GLWASM_Text * command = glwasm_text_at(syntax, i);
        ++commands;
        if (glwasm_text_kind(command) == GLWASM_TEXT_LIST &&
            glwasm_text_size(command) != 0 &&
            text_bytes(glwasm_text_at(command, 0)).rfind("assert_", 0) == 0)
          ++pending;
      }
      glwasm_text_destroy(syntax);
    }
  }

  std::cout << "wg-3.0 WAST parse gate: parsed=" << parsed_files << "/"
            << files.size() << " commands=" << commands
            << " run-files=" << run_files
            << " skip-files=" << (files.size() - run_files)
            << " binary-validation-checked=" << binary_validation_checked
            << " binary-validation-passed=" << binary_validation_passed
            << " text-malformed-status=" << text_malformed_passed << "/"
            << text_malformed_checked
            << " text-invalid-status=" << text_invalid_passed << "/"
            << text_invalid_checked
            << " wat-modules-lowered=" << wat_modules_passed << "/"
            << wat_modules_checked
            << " in-scope-wat-modules-lowered=" << wat_in_scope_passed << "/"
            << wat_modules_in_scope
            << " deferred-wat-modules=" << deferred_wat_modules
            << " assertions-pending=" << pending << '\n';
  for (const auto & [reason, count] : skips)
    std::cout << "  skip(" << reason << ")=" << count << '\n';
  for (const auto & [reason, count] : assertion_skip_counts)
    std::cout << "  assertion-skip(" << reason << ")=" << count << '\n';
  for (const auto & failure : wat_module_failures)
    std::cout << "  wat-module-failure " << failure << '\n';
  size_t expected_assertion_skips = 0;
  for (const auto & [file, lines] : per_assertion) {
    (void)file;
    expected_assertion_skips += lines.size();
  }
  EXPECT_EQ(consumed_assertion_skips.size(), expected_assertion_skips)
      << "each assertion skip must match a source-located assertion command";
  std::cout << "execution assertions pending; passed=0 (Story 3 adapter not "
               "connected)\n";
  for (const auto & failure : failures)
    ADD_FAILURE() << failure;
  EXPECT_EQ(parsed_files, files.size()) << "all pinned WAST files must parse";
  EXPECT_EQ(classes.size(), files.size())
      << "classification and fetched file sets must agree";
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
