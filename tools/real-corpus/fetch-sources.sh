#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$here/../.." && pwd)
output=${GLWASM_REAL_CORPUS_BUILD_DIR:-$repo/build/real-corpus}
(cd "$repo" && sha256sum -c "$repo/tests/real-corpus/tools.sha256")
python3 "$here/fetch_sources.py" \
  --pins "$repo/tests/real-corpus/pins.tsv" \
  --destination "$output/upstream"
