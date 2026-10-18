/* SPDX-License-Identifier: LGPL-3.0-only */
#include <stdint.h>
#include <string.h>
#include <zlib.h>
#include "corpus_result.h"

uint64_t corpus_run(void) {
  corpus_result_reset();
  static const unsigned char prefix[] = "ghoti-wasm-corpus-v1:";
  unsigned char input[4096], compressed[8192], output[4096];
  for (size_t i = 0; i < sizeof(input); ++i)
    input[i] = (unsigned char)(prefix[i % (sizeof(prefix) - 1)] ^
        (unsigned char)((i * 37u + i / 11u) & 0xffu));
  uLongf compressed_size = sizeof(compressed);
  if (compress2(compressed, &compressed_size, input, sizeof(input), 6) != Z_OK)
    return 0;
  uLongf output_size = sizeof(output);
  if (uncompress(output, &output_size, compressed, compressed_size) != Z_OK ||
      output_size != sizeof(input) || memcmp(input, output, sizeof(input)) != 0 ||
      !corpus_result_append(output, (uint32_t)output_size)) {
    corpus_result_reset();
    return 0;
  }
  corpus_result_valid = 1;
  return 1;
}
