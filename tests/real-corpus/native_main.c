/* SPDX-License-Identifier: LGPL-3.0-only */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

uint64_t corpus_run(void);
uint32_t corpus_output_size(void);
uint64_t corpus_output_chunk(uint32_t index);

int main(void) {
  if (corpus_run() == 0) {
    fputs("corpus driver failed\n", stderr);
    return 1;
  }
  const uint32_t length = corpus_output_size();
  printf("1\t%" PRIu32 "\t", length);
  for (uint32_t offset = 0; offset < length; offset += 7) {
    const uint64_t chunk = corpus_output_chunk(offset / 7) & UINT64_C(0x00ffffffffffffff);
    for (uint32_t byte = 0; byte < 7 && offset + byte < length; ++byte)
      printf("%02" PRIx32, (uint32_t)((chunk >> (byte * 8)) & 0xffu));
  }
  putchar('\n');
  return ferror(stdout) ? 1 : 0;
}
