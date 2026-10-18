# Story 13 opt-in benchmark target. Included at the end of the main Makefile,
# after TEST_INCLUDE, TEST_WASMLIBRARY and BUILD_DIR have been defined.
ifneq ($(filter benchmark benchmark-metadata benchmark-fixture-hashes check-benchmark-runner test test-quiet test-valgrind test-asan test-tsan test-gc-torture,$(MAKECMDGOALS)),)
ifeq ($(strip $(RDBG_LIBS)),)
$(error make benchmark requires runtime-debug development files in pkg-config)
endif
endif

# The runner hashes fixture bytes in process on every supported host, including
# Win64. Declare security directly instead of relying on runtime-debug's
# transitive link flags.
BENCH_GSEC_PC ?= ghoti.io-security-0
BENCH_GSEC_CFLAGS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --cflags $(BENCH_GSEC_PC) 2>/dev/null)
BENCH_GSEC_LIBS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs $(BENCH_GSEC_PC) 2>/dev/null)
ifneq ($(filter benchmark benchmark-metadata benchmark-fixture-hashes check-benchmark-runner test test-quiet test-valgrind test-asan test-tsan test-gc-torture,$(MAKECMDGOALS)),)
ifeq ($(strip $(BENCH_GSEC_LIBS)),)
$(error make benchmark requires security development files in pkg-config)
endif
endif

BENCH_OBJECT := $(OBJ_DIR)/benchmarks/runner.o
BENCH_APP := $(APP_DIR)/wasm-benchmark$(EXE_EXTENSION)

$(BENCH_OBJECT): benchmarks/runner.cpp $(FLAGS_STAMP) | $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(TEST_INCLUDE) $(BENCH_GSEC_CFLAGS) -c $< -o $@

$(BENCH_APP): $(BENCH_OBJECT) $(APP_DIR)/$(STATIC_TARGET) | $(APP_DIR)/$(TARGET)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS) $(TEST_LDFLAGS) $(TEST_WASMLIBRARY) $(BENCH_GSEC_LIBS) $(TESTFLAGS)

.PHONY: benchmark-build benchmark-metadata benchmark
benchmark-build: $(BENCH_APP) ## Internal forced-build target for Story 13 metadata capture

benchmark-metadata: ## Force-build and record Story 13 metadata without collecting timings
	@test -n "$(GLWASM_BENCH_OUTPUT)" || { printf 'GLWASM_BENCH_OUTPUT is required\n' >&2; exit 1; }
	@test -n "$(GLWASM_BENCH_SOURCE_MANIFEST_FILE)" || { printf 'GLWASM_BENCH_SOURCE_MANIFEST_FILE is required\n' >&2; exit 1; }
	@test -n "$(GLWASM_BENCH_SOURCE_COMMIT)" || { printf 'GLWASM_BENCH_SOURCE_COMMIT is required\n' >&2; exit 1; }
	@test -n "$(GLWASM_BENCH_SOURCE_MANIFEST_SHA256)" || { printf 'GLWASM_BENCH_SOURCE_MANIFEST_SHA256 is required\n' >&2; exit 1; }
	@python3 benchmarks/source_manifest.py --source-commit "$(GLWASM_BENCH_SOURCE_COMMIT)" --write "$(GLWASM_BENCH_SOURCE_MANIFEST_FILE)" --expected-sha256 "$(GLWASM_BENCH_SOURCE_MANIFEST_SHA256)"
	@metadata=$$(python3 benchmarks/record_build.py --make '$(MAKE)' --log '$(GLWASM_BENCH_OUTPUT).build.log') || exit $$?; eval "$$metadata"

benchmark: ## Force-build, record provenance, then write raw Story 13 CSV
	@test -n "$(GLWASM_BENCH_OUTPUT)" || { printf 'GLWASM_BENCH_OUTPUT is required\n' >&2; exit 1; }
	@test -n "$(GLWASM_BENCH_SOURCE_MANIFEST_FILE)" || { \
		printf 'GLWASM_BENCH_SOURCE_MANIFEST_FILE is required\n' >&2; exit 1; }
	@test -n "$(GLWASM_BENCH_SOURCE_COMMIT)" || { \
		printf 'GLWASM_BENCH_SOURCE_COMMIT is required\n' >&2; exit 1; }
	@test -n "$(GLWASM_BENCH_SOURCE_MANIFEST_SHA256)" || { \
		printf 'GLWASM_BENCH_SOURCE_MANIFEST_SHA256 is required\n' >&2; exit 1; }
	@python3 benchmarks/source_manifest.py \
		--source-commit "$(GLWASM_BENCH_SOURCE_COMMIT)" \
		--write "$(GLWASM_BENCH_SOURCE_MANIFEST_FILE)" \
		--expected-sha256 "$(GLWASM_BENCH_SOURCE_MANIFEST_SHA256)"
	@metadata=$$(python3 benchmarks/record_build.py --make '$(MAKE)' --log '$(GLWASM_BENCH_OUTPUT).build.log') || exit $$?; eval "$$metadata"; \
	GLWASM_BENCH_COMMAND='make benchmark BENCH_ITERATIONS=$(or $(BENCH_ITERATIONS),20000) BENCH_SAMPLES=$(or $(BENCH_SAMPLES),30)' \
		LD_LIBRARY_PATH="$(TEST_LD_PATH)" $(BENCH_APP) $(or $(BENCH_ITERATIONS),20000) $(or $(BENCH_SAMPLES),30)

.PHONY: benchmark-fixture-hashes
benchmark-fixture-hashes: $(BENCH_APP) ## Print candidate SHA-256 pins for exact benchmark Wasm binaries
	LD_LIBRARY_PATH="$(TEST_LD_PATH)" $(BENCH_APP) --print-fixture-hashes

.PHONY: check-benchmark-runner
check-benchmark-runner: $(BENCH_APP) ## Verify failed benchmark samples are emitted and flushed
	@LD_LIBRARY_PATH="$(TEST_LD_PATH)" python3 tests/unit/test_benchmark_runner.py "$(BENCH_APP)"
