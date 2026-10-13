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
 * Feeds each file named on the command line once through
 * LLVMFuzzerTestOneInput(), the entry point a fuzz harness defines. This is
 * `make fuzz-replay`: the corpus and the seeds as a regression test, in an
 * ordinary build, with no libFuzzer and no sanitizer. A crash is the failure.
 */

#define _DEFAULT_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size);

int main(int argc, char * argv[]) {
  int i;
  size_t replayed = 0;

  if (argc < 2) {
    fprintf(stderr, "usage: replay FILE...\n");
    return 2;
  }
  for (i = 1; i < argc; i++) {
    FILE * file = fopen(argv[i], "rb");
    size_t capacity = 4096;
    size_t length = 0;
    uint8_t * buffer;

    if (!file) {
      fprintf(stderr, "replay: cannot open %s\n", argv[i]);
      return 2;
    }
    buffer = malloc(capacity);
    while (buffer) {
      size_t got;

      if (length == capacity) {
        uint8_t * grown;

        capacity *= 2;
        grown = realloc(buffer, capacity);
        if (!grown) {
          free(buffer);
          buffer = NULL;
          break;
        }
        buffer = grown;
      }
      got = fread(buffer + length, 1, capacity - length, file);
      if (got == 0) {
        if (ferror(file)) {
          fprintf(stderr, "replay: error reading %s\n", argv[i]);
          free(buffer);
          fclose(file);
          return 2;
        }
        break;
      }
      length += got;
    }
    fclose(file);
    if (!buffer) {
      fprintf(stderr, "replay: out of memory reading %s\n", argv[i]);
      return 2;
    }
    LLVMFuzzerTestOneInput(buffer, length);
    free(buffer);
    replayed++;
  }
  printf("replay: %zu inputs, no crash\n", replayed);
  return 0;
}
