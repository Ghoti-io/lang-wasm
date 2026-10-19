/*
 * SPDX-License-Identifier: LGPL-3.0-only
 * Copyright (C) 2026 Corey Pennycuff
 *
 * WAST value syntax shared by the spec-suite runner and its unit tests:
 * numeric literals with the script grammar's widths, NaN patterns, and
 * reference forms. Everything here is pure text-to-bits conversion.
 */
#ifndef GHOTI_IO_GLWASM_TESTS_WAST_VALUES_H
#define GHOTI_IO_GLWASM_TESTS_WAST_VALUES_H

#include <ghoti.io/lang-wasm/parse.h>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace wast {

inline std::string text_bytes(const GLWASM_Text *node) {
  size_t length = 0;
  const uint8_t *bytes = glwasm_text_bytes(node, &length);
  return bytes ? std::string(reinterpret_cast<const char *>(bytes), length)
               : std::string();
}

inline bool atom_is(const GLWASM_Text *node, const char *value) {
  return node && glwasm_text_kind(node) == GLWASM_TEXT_ATOM &&
      text_bytes(node) == value;
}

/* The nth child that is not an annotation such as (@name ...). */
inline const GLWASM_Text *semantic_child(const GLWASM_Text *node,
    size_t wanted) {
  size_t seen = 0;
  for (size_t i = 0; node && i < glwasm_text_size(node); ++i) {
    const GLWASM_Text *child = glwasm_text_at(node, i);
    if (glwasm_text_kind(child) == GLWASM_TEXT_LIST &&
        glwasm_text_size(child) &&
        text_bytes(glwasm_text_at(child, 0)).rfind("@", 0) == 0)
      continue;
    if (seen++ == wanted) return child;
  }
  return nullptr;
}

inline size_t semantic_size(const GLWASM_Text *node) {
  size_t count = 0;
  while (semantic_child(node, count)) ++count;
  return count;
}

/* Integer literal of the script grammar: optional sign, decimal or 0x hex,
 * underscores between digits, and either signed or unsigned range. */
inline bool parse_integer(const std::string &raw, unsigned width,
    uint64_t *out) {
  std::string text;
  for (char c : raw) if (c != '_') text.push_back(c);
  if (text.empty() || !out) return false;
  const bool negative = text[0] == '-';
  const size_t sign = (text[0] == '-' || text[0] == '+') ? 1u : 0u;
  const bool hex = text.compare(sign, 2, "0x") == 0 ||
      text.compare(sign, 2, "0X") == 0;
  const std::string digits = text.substr(sign + (hex ? 2u : 0u));
  if (digits.empty()) return false;
  uint64_t magnitude = 0;
  const auto parsed = std::from_chars(digits.data(),
      digits.data() + digits.size(), magnitude, hex ? 16 : 10);
  if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size())
    return false;
  const uint64_t positive_limit = width == 32 ? UINT32_MAX : UINT64_MAX;
  const uint64_t negative_limit = UINT64_C(1) << (width - 1u);
  if ((negative && magnitude > negative_limit) ||
      (!negative && magnitude > positive_limit))
    return false;
  const uint64_t bits = negative ? UINT64_C(0) - magnitude : magnitude;
  *out = width == 32 ? static_cast<uint32_t>(bits) : bits;
  return true;
}

/* Float literal with correct rounding, including values outside long
 * double's range (which round to zero or infinity by their exponent). */
template <typename Float>
bool parse_float_text(const std::string &raw, Float *out) {
  std::string normalized;
  for (char c : raw) if (c != '_') normalized.push_back(c);
  if (normalized.empty() || !out) return false;
  if (normalized[0] == '+') normalized.erase(0, 1);
  if (normalized.empty()) return false;
  const bool negative = normalized[0] == '-';
  const size_t sign = negative ? 1u : 0u;
  if (normalized.compare(sign, std::string::npos, "inf") == 0) {
    *out = negative ? -std::numeric_limits<Float>::infinity()
                    : std::numeric_limits<Float>::infinity();
    return true;
  }
  auto format = std::chars_format::general;
  if (normalized.compare(sign, 2, "0x") == 0 ||
      normalized.compare(sign, 2, "0X") == 0) {
    normalized.erase(sign, 2);
    format = std::chars_format::hex;
  }
  Float value{};
  const char *begin = normalized.data();
  const char *end = begin + normalized.size();
  const auto parsed = std::from_chars(begin, end, value, format);
  if (parsed.ptr != end) return false;
  if (parsed.ec == std::errc::result_out_of_range) {
    /* from_chars reports range errors without a value; decide by the
     * decimal or binary exponent of the first significant digit. */
    const bool hexadecimal = format == std::chars_format::hex;
    const size_t exponent_at = normalized.find_first_of(
        hexadecimal ? "pP" : "eE", sign);
    const std::string mantissa = normalized.substr(sign,
        exponent_at == std::string::npos ? std::string::npos
                                         : exponent_at - sign);
    long long before = 0, first = -1, position = 0;
    bool after_point = false;
    unsigned leading = 0;
    for (char c : mantissa) {
      if (c == '.') { after_point = true; continue; }
      if (!after_point) ++before;
      unsigned digit;
      if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
      else if (hexadecimal && c >= 'a' && c <= 'f')
        digit = 10u + static_cast<unsigned>(c - 'a');
      else if (hexadecimal && c >= 'A' && c <= 'F')
        digit = 10u + static_cast<unsigned>(c - 'A');
      else return false;
      if (first < 0 && digit != 0) { first = position; leading = digit; }
      ++position;
    }
    if (first < 0) { *out = negative ? -Float(0) : Float(0); return true; }
    long long exponent = 0;
    if (exponent_at != std::string::npos) {
      size_t i = exponent_at + 1u;
      const bool exponent_negative = i < normalized.size() &&
          normalized[i] == '-';
      if (i < normalized.size() &&
          (normalized[i] == '-' || normalized[i] == '+')) ++i;
      if (i == normalized.size()) return false;
      for (; i < normalized.size(); ++i) {
        if (normalized[i] < '0' || normalized[i] > '9') return false;
        exponent = std::min(1000000000LL, exponent * 10 + (normalized[i] - '0'));
      }
      if (exponent_negative) exponent = -exponent;
    }
    long long order = exponent + before - first - 1;
    if (hexadecimal) {
      unsigned top = 0;
      while (leading >>= 1u) ++top;
      order = exponent + 4 * (before - first - 1) + top;
    }
    if (order < 0) *out = negative ? -Float(0) : Float(0);
    else *out = negative ? -std::numeric_limits<Float>::infinity()
                         : std::numeric_limits<Float>::infinity();
    return true;
  }
  if (parsed.ec != std::errc{}) return false;
  *out = value;
  return true;
}

enum class NanPattern { None, Canonical, Arithmetic };

/* Bits of an f32/f64 literal; `nan` and `nan:0x...` included. A pattern
 * (nan:canonical, nan:arithmetic) is reported in *pattern with no bits. */
inline bool parse_float_bits(const std::string &raw, bool f32,
    uint64_t *out, NanPattern *pattern) {
  if (pattern) *pattern = NanPattern::None;
  std::string text;
  for (char c : raw) if (c != '_') text.push_back(c);
  const bool negative = !text.empty() && text[0] == '-';
  const std::string magnitude = (!text.empty() &&
      (text[0] == '-' || text[0] == '+')) ? text.substr(1) : text;
  const uint64_t sign_bit = f32 ? UINT64_C(0x80000000)
                                : UINT64_C(0x8000000000000000);
  const uint64_t exponent_mask = f32 ? UINT64_C(0x7f800000)
                                     : UINT64_C(0x7ff0000000000000);
  const uint64_t quiet = f32 ? UINT64_C(0x00400000)
                             : UINT64_C(0x0008000000000000);
  const uint64_t payload_mask = f32 ? UINT64_C(0x007fffff)
                                    : UINT64_C(0x000fffffffffffff);
  if (magnitude == "nan:canonical" || magnitude == "nan:arithmetic") {
    if (!pattern || negative) return false;
    *pattern = magnitude == "nan:canonical" ? NanPattern::Canonical
                                            : NanPattern::Arithmetic;
    *out = exponent_mask | quiet;
    return true;
  }
  if (magnitude == "nan") {
    *out = (negative ? sign_bit : 0) | exponent_mask | quiet;
    return true;
  }
  if (magnitude.rfind("nan:0x", 0) == 0) {
    const std::string digits = magnitude.substr(6);
    uint64_t payload = 0;
    const auto parsed = std::from_chars(digits.data(),
        digits.data() + digits.size(), payload, 16);
    if (digits.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != digits.data() + digits.size() || payload == 0 ||
        payload > payload_mask)
      return false;
    *out = (negative ? sign_bit : 0) | exponent_mask | payload;
    return true;
  }
  if (f32) {
    float value;
    if (!parse_float_text(text, &value)) return false;
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    *out = bits;
  } else {
    double value;
    if (!parse_float_text(text, &value)) return false;
    std::memcpy(out, &value, sizeof(*out));
  }
  return true;
}

inline bool nan_matches(uint64_t actual, bool f32, NanPattern pattern) {
  const uint64_t exponent_mask = f32 ? UINT64_C(0x7f800000)
                                     : UINT64_C(0x7ff0000000000000);
  const uint64_t payload_mask = f32 ? UINT64_C(0x007fffff)
                                    : UINT64_C(0x000fffffffffffff);
  const uint64_t quiet = f32 ? UINT64_C(0x00400000)
                             : UINT64_C(0x0008000000000000);
  if (f32) actual &= UINT32_MAX;
  if ((actual & exponent_mask) != exponent_mask ||
      (actual & payload_mask) == 0)
    return false;
  if (pattern == NanPattern::Canonical)
    return (actual & payload_mask) == quiet;
  return (actual & quiet) != 0;
}

} // namespace wast

#endif
