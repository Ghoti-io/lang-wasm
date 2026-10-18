/* SPDX-License-Identifier: LGPL-3.0-only */
#ifndef GHOTI_LANG_WASM_CORPUS_RESULT_H
#define GHOTI_LANG_WASM_CORPUS_RESULT_H

#include <stdint.h>
#include <string.h>

#define CORPUS_RESULT_CAPACITY 8192u

uint64_t corpus_run(void);

static uint8_t corpus_result[CORPUS_RESULT_CAPACITY];
static uint32_t corpus_result_length;
static uint32_t corpus_result_valid;

static void corpus_result_reset(void) {
  corpus_result_length = 0;
  corpus_result_valid = 0;
}

static int corpus_result_append(const void *data, uint32_t length) {
  if (length > CORPUS_RESULT_CAPACITY - corpus_result_length) return 0;
  if (length != 0) memcpy(corpus_result + corpus_result_length, data, length);
  corpus_result_length += length;
  return 1;
}

static int corpus_result_append_u32(uint32_t value) {
  uint8_t bytes[4];
  for (uint32_t i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (i * 8));
  return corpus_result_append(bytes, sizeof(bytes));
}

static int corpus_result_append_u64(uint64_t value) {
  uint8_t bytes[8];
  for (uint32_t i = 0; i < 8; ++i) bytes[i] = (uint8_t)(value >> (i * 8));
  return corpus_result_append(bytes, sizeof(bytes));
}

uint32_t corpus_output_size(void) {
  if (!corpus_result_valid && corpus_run() == 0) return 0;
  return corpus_result_length;
}

uint64_t corpus_output_chunk(uint32_t index) {
  if (!corpus_result_valid && corpus_run() == 0) return 0;
  if (index > UINT32_MAX / 7u) return 0;
  uint64_t value = 0;
  const uint32_t offset = index * 7u;
  if (offset >= corpus_result_length) return 0;
  for (uint32_t i = 0; i < 7; ++i) {
    if (i < corpus_result_length - offset)
      value |= (uint64_t)corpus_result[offset + i] << (i * 8);
  }
  return (UINT64_C(1) << 56) | value;
}

#endif
