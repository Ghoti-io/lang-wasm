# Copied from color's Makefile, which came from model's, per CONVENTIONS.md
# section 12. SUITE, PROJECT and the object discovery are all that changed.
# Color's security dependency and its oracle gates are not this library's.

SUITE := ghoti.io
PROJECT := lang-wasm

BUILD ?= release
MAJOR_VERSION := 0
MINOR_VERSION := 0.0
VERSION_MINOR_ONLY := $(word 1,$(subst ., ,$(MINOR_VERSION)))
VERSION_PATCH_ONLY := $(or $(word 2,$(subst ., ,$(MINOR_VERSION))),0)
VERSION := $(MAJOR_VERSION).$(MINOR_VERSION)

# Names this build everywhere: the .pc file, the install directory, the soname
# and the symbol token. Defaults to the major version.
BRANCH ?= -$(MAJOR_VERSION)

ifeq ($(BRANCH),-$(MAJOR_VERSION))
VERSION_STRING := $(VERSION)
else
VERSION_STRING := $(VERSION)$(BRANCH)
endif

ifeq ($(BUILD),debug)
    override BRANCH := $(BRANCH)-debug
    override VERSION_STRING := $(VERSION_STRING)-debug
endif

ifeq ($(BUILD),debug)
OPT_CFLAGS := -O0
else
OPT_CFLAGS := -O2
endif

BASE_NAME := lib$(SUITE)-$(PROJECT)$(BRANCH).so
LIBVER_SYMBOL := $(shell echo "ghotiio_$(PROJECT)$(BRANCH)" | sed 's/[.-]/_/g')
BASE_NAME_PREFIX := lib$(SUITE)-$(PROJECT)$(BRANCH)
SO_NAME := $(BASE_NAME).$(MAJOR_VERSION)
STATIC_TARGET := $(BASE_NAME_PREFIX).a

PKG_CONFIG_PATH_ENV := $(PKG_CONFIG_PATH)

# The test corpus is fetched and pinned by runtime-jit's tools/wasm. These
# paths configure only the cross-library spec-suite test gate.
WASM_SPEC_COMMIT := $(shell cat ../runtime-jit/tools/wasm/WASM_SPEC_COMMIT)
WASM_SPEC_DIR ?= ../runtime-jit/third_party/wasm-spec/$(WASM_SPEC_COMMIT)
export GLWASM_WAST_CORPUS_ROOT := $(abspath $(WASM_SPEC_DIR)/test)
export GLWASM_WAST_CLASSIFICATION := $(abspath tests/data-wast-classification.tsv)
export GLWASM_WAST_ASSERTION_SKIPS := $(abspath tests/data-wast-assertion-skips.tsv)
export GLWASM_WAST_JIT_RULES := $(abspath tests/data-wast-jit-rules.tsv)
export GLWASM_WAST_SPEC_COMMIT := $(WASM_SPEC_COMMIT)

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S), Linux)
	OS_NAME := Linux
	LIB_EXTENSION := so
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG := -Wl,-soname,$(SO_NAME)
	TARGET := $(SO_NAME).$(MINOR_VERSION)
	EXE_EXTENSION :=
	PC_INSTALL_PATH := /usr/local/share/pkgconfig
	INCLUDE_INSTALL_PATH := /usr/local/include
	LIB_INSTALL_PATH := /usr/local/lib
	PC_INCLUDE_DIR := $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	PC_LIB_DIR := $(LIB_INSTALL_PATH)/$(SUITE)
	override BUILD := linux/$(BUILD)

else ifeq ($(UNAME_S), Darwin)
	OS_NAME := Mac
	LIB_EXTENSION := dylib
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG := -Wl,-install_name,$(BASE_NAME_PREFIX).dylib
	TARGET := $(BASE_NAME_PREFIX).dylib
	EXE_EXTENSION :=
	PC_INSTALL_PATH := /usr/local/share/pkgconfig
	INCLUDE_INSTALL_PATH := /usr/local/include
	LIB_INSTALL_PATH := /usr/local/lib
	PC_INCLUDE_DIR := $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	PC_LIB_DIR := $(LIB_INSTALL_PATH)/$(SUITE)
	override BUILD := mac/$(BUILD)

# TODO(windows): the Windows branches in this file were adapted from color's
# and have never been run, nor has GLWASM_API's dllexport/dllimport switching.
# See notes/suite/WINDOWS-TODO.md.
else ifeq ($(findstring MINGW32_NT,$(UNAME_S)),MINGW32_NT)
	OS_NAME := Windows
	LIB_EXTENSION := dll
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG = -Wl,--out-implib,$(APP_DIR)/$(BASE_NAME_PREFIX).dll.a
	TARGET := $(BASE_NAME_PREFIX).dll
	EXE_EXTENSION := .exe
	PC_INSTALL_PATH := /mingw32/lib/pkgconfig
	INCLUDE_INSTALL_PATH := /mingw32/include
	LIB_INSTALL_PATH := /mingw32/lib
	BIN_INSTALL_PATH := /mingw32/bin
	PC_INCLUDE_DIR = $(shell cygpath -m $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH))
	PC_LIB_DIR = $(shell cygpath -m $(LIB_INSTALL_PATH)/$(SUITE))
	override BUILD := win32/$(BUILD)

else ifeq ($(findstring MINGW64_NT,$(UNAME_S)),MINGW64_NT)
	OS_NAME := Windows
	LIB_EXTENSION := dll
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG = -Wl,--out-implib,$(APP_DIR)/$(BASE_NAME_PREFIX).dll.a
	TARGET := $(BASE_NAME_PREFIX).dll
	EXE_EXTENSION := .exe
	PC_INSTALL_PATH := /mingw64/lib/pkgconfig
	INCLUDE_INSTALL_PATH := /mingw64/include
	LIB_INSTALL_PATH := /mingw64/lib
	BIN_INSTALL_PATH := /mingw64/bin
	PC_INCLUDE_DIR = $(shell cygpath -m $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH))
	PC_LIB_DIR = $(shell cygpath -m $(LIB_INSTALL_PATH)/$(SUITE))
	override BUILD := win64/$(BUILD)

else
    $(error Unsupported OS: $(UNAME_S))
endif

ifdef PREFIX
INCLUDE_INSTALL_PATH := $(PREFIX)/include
LIB_INSTALL_PATH := $(PREFIX)/lib
BIN_INSTALL_PATH := $(PREFIX)/bin
PC_INSTALL_PATH := $(PREFIX)/share/pkgconfig
ifeq ($(OS_NAME), Windows)
PC_INCLUDE_DIR = $(shell cygpath -m $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH))
PC_LIB_DIR = $(shell cygpath -m $(LIB_INSTALL_PATH)/$(SUITE))
else
PC_INCLUDE_DIR := $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
PC_LIB_DIR := $(LIB_INSTALL_PATH)/$(SUITE)
endif
LDCONF_INSTALL_PATH :=
endif

PKG_CONFIG_LOOKUP_PATH := $(if $(PKG_CONFIG_PATH_ENV),$(PKG_CONFIG_PATH_ENV):)$(PC_INSTALL_PATH)

CXX := g++
CXXFLAGS := -pedantic-errors -Wall -Wextra -Werror -Wno-error=unused-function -Wfatal-errors -std=c++20 -O1 -g -ffp-contract=off $(EXTRA_CXXFLAGS)
CC := cc
CFLAGS := -pedantic-errors -Wall -Wextra -Werror -Wfloat-conversion -fstrict-aliasing -Wstrict-aliasing=1 -Wno-error=unused-function -Wfatal-errors -std=c17 $(OPT_CFLAGS) -g -ffp-contract=off $(EXTRA_CFLAGS)
ifeq ($(OS_NAME), Windows)
CFLAGS += -DGLWASM_STATIC
CXXFLAGS += -DGLWASM_STATIC
endif
LIB_CFLAGS := $(CFLAGS) -fvisibility=hidden -DGLWASM_BUILD $(EXTRA_CFLAGS)
LDFLAGS := -L /usr/lib -lstdc++ -lm $(EXTRA_LDFLAGS)
ifdef PREFIX
LDFLAGS += -Wl,-rpath,$(LIB_INSTALL_PATH)/$(SUITE)
ifeq ($(OS_NAME), Windows)
export PATH := $(BIN_INSTALL_PATH):$(PATH)
endif
endif

BUILD_DIR := $(if $(GLWASM_REAL_CORPUS_BUILD_DIR),$(abspath $(GLWASM_REAL_CORPUS_BUILD_DIR))/lang-wasm-build/$(BUILD),./build/$(BUILD))
OBJ_DIR := $(BUILD_DIR)/objects
FLAGS_STAMP := $(OBJ_DIR)/.flags
GEN_DIR := $(BUILD_DIR)/generated
APP_DIR := $(BUILD_DIR)/apps

ifeq ($(UNAME_S), Linux)
	LIB_CFLAGS += -fPIC
endif

INCLUDE := -I include/ -I $(GEN_DIR)/ -I src/

DEPLESS_GOALS := docs docs-pdf clean fuzz-clean cloc help check-wast-corpus
ifeq ($(filter-out $(DEPLESS_GOALS),$(or $(MAKECMDGOALS),all)),)
SKIP_DEP_CHECK := 1
endif

# Dependencies remain on their current major while this library's branch may
# advance independently. Preserve the debug suffix and allow explicit overrides.
DEPENDENCY_RELEASE_BRANCH := $(patsubst %-debug,%,$(BRANCH))
DEPENDENCY_BUILD_SUFFIX := $(if $(filter %-debug,$(BRANCH)),-debug,)
CUTIL_BRANCH ?= $(if $(filter -dev,$(DEPENDENCY_RELEASE_BRANCH)),-dev,-0)$(DEPENDENCY_BUILD_SUFFIX)
RCORE_BRANCH ?= $(if $(filter -dev,$(DEPENDENCY_RELEASE_BRANCH)),-dev,-0)$(DEPENDENCY_BUILD_SUFFIX)
CUTIL_PC ?= ghoti.io-cutil$(CUTIL_BRANCH)
RCORE_PC ?= ghoti.io-runtime-core$(RCORE_BRANCH)
CUTIL_CFLAGS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --cflags $(CUTIL_PC) 2>/dev/null)
CUTIL_LIBS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs $(CUTIL_PC) 2>/dev/null)
CUTIL_LIBDIR := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs-only-L $(CUTIL_PC) 2>/dev/null | awk '{print $$1}' | sed 's/^-L//')
ifeq ($(strip $(CUTIL_CFLAGS)),)
ifndef SKIP_DEP_CHECK
$(error ghoti.io-cutil was not found by pkg-config. Run ./bootstrap.sh at the root of the workspace - two levels up, the directory holding libs/ - to build and install the suite into a local prefix, then pass the same PREFIX here - or point PKG_CONFIG_PATH at the directory holding its .pc file. There is deliberately no sibling-checkout fallback.)
endif
endif
INCLUDE += $(CUTIL_CFLAGS)
RCORE_CFLAGS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --cflags $(RCORE_PC) 2>/dev/null)
RCORE_LIBS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs $(RCORE_PC) 2>/dev/null)
RCORE_EXISTS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --exists $(RCORE_PC) 2>/dev/null && echo yes)
RCORE_LIBDIR := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs-only-L $(RCORE_PC) 2>/dev/null | awk '{print $$1}' | sed 's/^-L//')
ifneq ($(RCORE_EXISTS),yes)
ifndef SKIP_DEP_CHECK
$(error ghoti.io-runtime-core was not found by pkg-config. Install runtime-core into the same PREFIX and set PKG_CONFIG_PATH. There is deliberately no sibling-checkout fallback.)
endif
endif
INCLUDE += $(RCORE_CFLAGS)

SOURCES := $(shell find src -type f -name '*.c')
LIBOBJECTS := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SOURCES))

TESTFLAGS := `PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs --cflags gtest`
RJIT_BRANCH ?= $(if $(filter -dev,$(DEPENDENCY_RELEASE_BRANCH)),-dev,-0)$(DEPENDENCY_BUILD_SUFFIX)
RJIT_PC ?= ghoti.io-runtime-jit$(RJIT_BRANCH)
RJIT_CFLAGS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --cflags $(RJIT_PC) 2>/dev/null)
RJIT_LIBS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs $(RJIT_PC) 2>/dev/null)
RJIT_LIBDIR := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs-only-L $(RJIT_PC) 2>/dev/null | awk '{print $$1}' | sed 's/^-L//')
RJIT_EXISTS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --exists $(RJIT_PC) 2>/dev/null && echo yes)
ifeq ($(RJIT_EXISTS),yes)
INCLUDE += $(RJIT_CFLAGS)
else
ifndef SKIP_DEP_CHECK
$(error ghoti.io-runtime-jit was not found by pkg-config. Story 10 requires it for module-shared compiled code.)
endif
endif
NEED_RJIT_FIXTURE := $(filter test test-quiet test-valgrind test-asan test-tsan test-gc-torture testBoundary_wasm $(APP_DIR)/testBoundary_wasm$(EXE_EXTENSION),$(MAKECMDGOALS))
ifneq ($(RJIT_EXISTS),yes)
ifneq ($(NEED_RJIT_FIXTURE),)
ifndef SKIP_DEP_CHECK
$(error ghoti.io-runtime-jit was not found by pkg-config; the module JIT and compiled boundary acceptance tests require it)
endif
endif
endif
RDBG_BRANCH ?= $(if $(filter -dev,$(DEPENDENCY_RELEASE_BRANCH)),-dev,-0)$(DEPENDENCY_BUILD_SUFFIX)
RDBG_PC ?= ghoti.io-runtime-debug$(RDBG_BRANCH)
RDBG_CFLAGS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --cflags $(RDBG_PC) 2>/dev/null)
RDBG_LIBS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs $(RDBG_PC) 2>/dev/null)
RDBG_EXISTS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --exists $(RDBG_PC) 2>/dev/null && echo yes)
NEED_RDBG_FIXTURE := $(filter test test-quiet test-valgrind test-asan test-tsan test-gc-torture,$(MAKECMDGOALS))
ifneq ($(RDBG_EXISTS),yes)
ifneq ($(NEED_RDBG_FIXTURE),)
ifndef SKIP_DEP_CHECK
$(error ghoti.io-runtime-debug was not found by pkg-config; the Story 7 debugger contract tests require it)
endif
endif
endif
RHEAP_BRANCH ?= $(if $(filter -dev,$(DEPENDENCY_RELEASE_BRANCH)),-dev,-0)$(DEPENDENCY_BUILD_SUFFIX)
RHEAP_PC ?= ghoti.io-runtime-heap$(RHEAP_BRANCH)
RHEAP_CFLAGS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --cflags $(RHEAP_PC) 2>/dev/null)
RHEAP_LIBS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs $(RHEAP_PC) 2>/dev/null)
RHEAP_LIBDIR := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs-only-L $(RHEAP_PC) 2>/dev/null | awk '{print $$1}' | sed 's/^-L//')
RHEAP_EXISTS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --exists $(RHEAP_PC) 2>/dev/null && echo yes)
# GNU ld searches every -L directory in command-line order, even when the
# matching -l appears later. Put the selected heap prefix first for relocation
# fixtures so an earlier base-prefix -L cannot silently select the non-moving
# runtime-heap with the same soname.
RHEAP_RELOCATE_LDFLAGS := $(if $(RHEAP_LIBDIR),-L$(RHEAP_LIBDIR))
NEED_RHEAP_BOUNDARY := $(filter test test-quiet test-valgrind test-asan test-tsan test-gc-torture testBoundary_wasm test-boundary-gc test-boundary-relocate test-debug-relocate,$(MAKECMDGOALS))
ifneq ($(RHEAP_EXISTS),yes)
ifneq ($(NEED_RHEAP_BOUNDARY),)
ifndef SKIP_DEP_CHECK
$(error ghoti.io-runtime-heap was not found by pkg-config; the Story 12 boundary acceptance fixture requires it)
endif
endif
endif
TEST_INCLUDE := $(INCLUDE) $(RJIT_CFLAGS) $(RDBG_CFLAGS) $(RHEAP_CFLAGS)

TEST_GATES ?= check-symbols check-aliasing check-stamps check-fp-contract check-labels check-edges check-wast-corpus check-benchmark-summarizer check-benchmark-runner fuzz-replay

VALGRIND_FLAGS := --leak-check=full --show-leak-kinds=definite,indirect,possible --track-origins=yes --error-exitcode=1 --suppressions=tests/valgrind.supp

WASMLIBRARY := -Wl,--whole-archive $(APP_DIR)/$(STATIC_TARGET) -Wl,--no-whole-archive $(RJIT_LIBS) $(RCORE_LIBS) $(CUTIL_LIBS)
TEST_WASMLIBRARY := $(WASMLIBRARY) $(RDBG_LIBS) $(RHEAP_LIBS)

ifeq ($(OS_NAME), Windows)
# The presets' native-stack budget (4 MiB) is more than a MinGW executable's
# default stack reserve (2 MB), so a compiled recursion would fault before it
# reached the budget. 16 MB is reserved address space, committed as used.
LDFLAGS += -Wl,--stack,16777216
TEST_LDFLAGS := -Wl,--wrap=__mingw_fprintf
else
TEST_LDFLAGS :=
endif

TEST_PAIRS := $(shell find tests -type f -name 'test_*.cpp' 2>/dev/null | sort | while read f; do \
	echo "$$f|$$(basename "$$f" .cpp | sed 's/test_/test/; s/^test\([a-z]\)/test\U\1/')"; done)
TEST_SOURCES := $(foreach pair,$(TEST_PAIRS),$(word 1,$(subst |, ,$(pair))))
TEST_NAMES := $(foreach pair,$(TEST_PAIRS),$(word 2,$(subst |, ,$(pair))))
TEST_EXECUTABLES := $(addprefix $(APP_DIR)/,$(addsuffix $(EXE_EXTENSION),$(TEST_NAMES)))
SAMPLE_HOST := $(APP_DIR)/sample_host$(EXE_EXTENSION)
SAMPLE_HOST_OBJECT := $(OBJ_DIR)/examples/sample_host.o

RDBG_LIBDIR := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs-only-L $(RDBG_PC) 2>/dev/null | awk '{print $$1}' | sed 's/^-L//')
TEST_LD_PATH := $(APP_DIR):$(RJIT_LIBDIR):$(RCORE_LIBDIR):$(CUTIL_LIBDIR):$(RDBG_LIBDIR):$(RHEAP_LIBDIR):$(LIB_INSTALL_PATH)/$(SUITE)
RHEAP_RELOCATE_LD_PATH := $(if $(RHEAP_LIBDIR),$(RHEAP_LIBDIR):)$(TEST_LD_PATH)

all: $(APP_DIR)/$(TARGET) $(APP_DIR)/$(STATIC_TARGET) ## Build shared + static libraries

TEST_DEPFILES := $(foreach pair,$(TEST_PAIRS),$(OBJ_DIR)/tests/$(basename $(notdir $(word 1,$(subst |, ,$(pair))))).d)
DEPFILES := $(LIBOBJECTS:.o=.d) $(TEST_DEPFILES)
-include $(DEPFILES)

LIBVER_GEN := $(GEN_DIR)/ghoti.io/$(PROJECT)/libver_gen.h

.PHONY: force-libver
force-libver:

$(LIBVER_GEN): force-libver
	@if [ -z "$(LIBVER_SYMBOL)" ]; then \
		printf "### LIBVER_SYMBOL is empty ###\n" >&2; \
		printf "Every exported symbol would lose its version namespace.\n" >&2; \
		exit 1; \
	fi
	@mkdir -p $(@D)
	@printf '%s\n' \
		'// Generated by the Makefile. Do not edit; see CONVENTIONS.md section 4.' \
		'#ifndef GHOTI_IO_GLWASM_LIBVER_GEN_H' \
		'#define GHOTI_IO_GLWASM_LIBVER_GEN_H' \
		'' \
		'/** The symbol namespace for this build, from the Makefile'"'"'s BRANCH. */' \
		'#define GHOTIIO_LANG_WASM_NAME $(LIBVER_SYMBOL)' \
		'' \
		'/** Human-readable version of this build. */' \
		'#define GHOTIIO_LANG_WASM_VERSION "$(VERSION_STRING)"' \
		'' \
		'/** The same version as three integers. */' \
		'#define GHOTIIO_LANG_WASM_VERSION_MAJOR $(MAJOR_VERSION)' \
		'#define GHOTIIO_LANG_WASM_VERSION_MINOR $(VERSION_MINOR_ONLY)' \
		'#define GHOTIIO_LANG_WASM_VERSION_PATCH $(VERSION_PATCH_ONLY)' \
		'' \
		'#endif // GHOTI_IO_GLWASM_LIBVER_GEN_H' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(OBJ_DIR)/%.o: src/%.c $(FLAGS_STAMP) | $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CC) $(LIB_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(APP_DIR)/$(TARGET): $(LIBOBJECTS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(LDFLAGS) $(RJIT_LIBS) $(RCORE_LIBS) $(CUTIL_LIBS) $(OS_SPECIFIC_LIBRARY_NAME_FLAG)
ifeq ($(OS_NAME), Linux)
	@ln -f -s $(TARGET) $(APP_DIR)/$(SO_NAME)
	@ln -f -s $(SO_NAME) $(APP_DIR)/$(BASE_NAME)
endif

$(APP_DIR)/$(STATIC_TARGET): $(LIBOBJECTS)
	@mkdir -p $(@D)
	@rm -f $@
	ar rcs $@ $^

$(SAMPLE_HOST_OBJECT): examples/sample_host.c $(FLAGS_STAMP) | $(LIBVER_GEN)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDE) $(CUTIL_CFLAGS) $(RCORE_CFLAGS) -c $< -o $@

$(SAMPLE_HOST): $(SAMPLE_HOST_OBJECT) $(APP_DIR)/$(STATIC_TARGET) | $(APP_DIR)/$(TARGET)
	$(CXX) $(CXXFLAGS) -o $@ $(SAMPLE_HOST_OBJECT) $(LDFLAGS) $(WASMLIBRARY)

sample: $(SAMPLE_HOST) ## Run the policy-bounded sample plugin host
	LD_LIBRARY_PATH="$(TEST_LD_PATH)" $(SAMPLE_HOST) examples/runaway.wasm

$(OBJ_DIR)/tests/%.o: tests/unit/%.cpp $(FLAGS_STAMP) | $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(TEST_INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

define test-executable-rule
TEST_OBJ_$1 := $(OBJ_DIR)/tests/$(basename $(notdir $1)).o

$(APP_DIR)/$2$(EXE_EXTENSION): $$(TEST_OBJ_$1) $(APP_DIR)/$(STATIC_TARGET) | $(APP_DIR)/$(TARGET)
	@mkdir -p $$(@D)
	$(CXX) $(CXXFLAGS) -o $$@ $$(TEST_OBJ_$1) $(LDFLAGS) $(TEST_LDFLAGS) $(TEST_WASMLIBRARY) $(TESTFLAGS)
endef

$(foreach pair,$(TEST_PAIRS),\
	$(eval $(call test-executable-rule,$(word 1,$(subst |, ,$(pair))),$(word 2,$(subst |, ,$(pair))))))

BOUNDARY_RELOCATE_OBJECT := $(OBJ_DIR)/tests/test_boundary_wasm_relocate.o
BOUNDARY_RELOCATE_APP := $(APP_DIR)/testBoundary_wasm_relocate$(EXE_EXTENSION)
$(BOUNDARY_RELOCATE_OBJECT): tests/unit/test_boundary_wasm.cpp $(FLAGS_STAMP) | $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(TEST_INCLUDE) -DGLWASM_BOUNDARY_RELOCATE -c $< -MMD -MP -MF $(@:.o=.d) -o $@
$(BOUNDARY_RELOCATE_APP): $(BOUNDARY_RELOCATE_OBJECT) $(APP_DIR)/$(STATIC_TARGET) $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -o $@ $< $(RHEAP_RELOCATE_LDFLAGS) $(LDFLAGS) $(TEST_LDFLAGS) $(TEST_WASMLIBRARY) $(TESTFLAGS)
.PHONY: test-boundary-gc test-boundary-relocate
test-boundary-gc: $(APP_DIR)/testBoundary_wasm$(EXE_EXTENSION) ## Run Story 12 boundary tests with runtime-heap GC torture
	LD_LIBRARY_PATH="$(TEST_LD_PATH):$(RHEAP_LIBDIR)" tools/run-gtest-full.sh $<
test-boundary-relocate: $(BOUNDARY_RELOCATE_APP) ## Require runtime-heap relocation mode for the Story 12 boundary tests
	LD_LIBRARY_PATH="$(RHEAP_RELOCATE_LD_PATH)" tools/run-gtest-full.sh $<

DEBUG_RELOCATE_OBJECT := $(OBJ_DIR)/tests/test_debug_relocate.o
DEBUG_RELOCATE_APP := $(APP_DIR)/testDebug_relocate$(EXE_EXTENSION)
.PHONY: check-debug-relocate-header test-debug-relocate
check-debug-relocate-header:
	@test -f "$(patsubst -I%,%,$(firstword $(RHEAP_CFLAGS)))/ghoti.io/runtime-heap/relocate.h" || { \
		printf 'test-debug-relocate requires a RELOCATE=yes runtime-heap install on PKG_CONFIG_PATH\n' >&2; \
		exit 1; \
	}
$(DEBUG_RELOCATE_OBJECT): tests/unit/test_debug.cpp $(FLAGS_STAMP) | check-debug-relocate-header $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(TEST_INCLUDE) -DGLWASM_DEBUG_RELOCATE -c $< -MMD -MP -MF $(@:.o=.d) -o $@
$(DEBUG_RELOCATE_APP): $(DEBUG_RELOCATE_OBJECT) $(APP_DIR)/$(STATIC_TARGET) $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -o $@ $< $(RHEAP_RELOCATE_LDFLAGS) $(LDFLAGS) $(TEST_LDFLAGS) $(TEST_WASMLIBRARY) $(TESTFLAGS)
test-debug-relocate: $(DEBUG_RELOCATE_APP) ## Compare compiled debugger frames under GC torture, stack movement and relocation
	@output=$$(env -u GTEST_FILTER -u GTEST_TOTAL_SHARDS -u GTEST_SHARD_INDEX \
		-u GTEST_SHARD_STATUS_FILE -u GTEST_REPEAT -u GTEST_ALSO_RUN_DISABLED_TESTS \
		LD_LIBRARY_PATH="$(RHEAP_RELOCATE_LD_PATH)" $< \
		--gtest_filter='WasmDebug.CompiledFramesMatchInterpreterAtInstructionPolls:WasmDebug.NestedCompiledFrameIdentitiesMatchAtEveryInstructionPoll' \
		--gtest_brief=0 --gtest_color=no 2>&1); \
		rc=$$?; printf '%s\n' "$$output"; \
		[ $$rc -eq 0 ] && [ "$$(printf '%s\n' "$$output" | grep -c '^\[ RUN      \] WasmDebug\.')" -eq 2 ] && \
		[ "$$(printf '%s\n' "$$output" | grep -c '^\[==========\] 2 tests from .* ran\.')" -eq 1 ] && \
		printf '%s\n' "$$output" | grep -q '^\[  PASSED  \] 2 tests\.' || exit 1

SPEC_RELOCATE_OBJECT := $(OBJ_DIR)/tests/test_spec_suite_relocate.o
SPEC_RELOCATE_APP := $(APP_DIR)/testSpec_suite_relocate$(EXE_EXTENSION)
.PHONY: test-spec-relocate
$(SPEC_RELOCATE_OBJECT): tests/unit/test_spec_suite.cpp $(FLAGS_STAMP) | check-debug-relocate-header $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(TEST_INCLUDE) -DGLWASM_SPEC_RELOCATE -c $< -MMD -MP -MF $(@:.o=.d) -o $@
$(SPEC_RELOCATE_APP): $(SPEC_RELOCATE_OBJECT) $(APP_DIR)/$(STATIC_TARGET) $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -o $@ $< $(RHEAP_RELOCATE_LDFLAGS) $(LDFLAGS) $(TEST_LDFLAGS) $(TEST_WASMLIBRARY) $(TESTFLAGS)
test-spec-relocate: $(SPEC_RELOCATE_APP) ## Run the spec-suite frame differential with relocation required
	LD_LIBRARY_PATH="$(RHEAP_RELOCATE_LD_PATH)" $< \
		--gtest_filter='SpecSuite.FramesMatchAcrossTiersWhereEveryInvocationStops' \
		--gtest_brief=0 --gtest_color=no

.PHONY: clean cloc docs docs-pdf coverage check-symbols check-stamps check-fp-contract check-aliasing check-labels check-edges check-wast-corpus check-install test-tsan test-gc-torture
.PHONY: all install test test-quiet test-asan test-valgrind uninstall
.PHONY: fuzz fuzz-clean fuzz-replay help

check-aliasing: ## Fail if -Wstrict-aliasing is not armed at level 1
	@mkdir -p $(BUILD_DIR)
	@printf 'int glwasm_alias_probe(float * f);\nint glwasm_alias_probe(float * f) { int * i = (int *)f; *i = 7; return *i; }\n' > $(BUILD_DIR)/alias_probe.c
	@printf 'int glwasm_alias_clean(int * i);\nint glwasm_alias_clean(int * i) { *i = 7; return *i; }\n' > $(BUILD_DIR)/alias_clean.c
	@probe=$$($(CC) $(CFLAGS) -Wno-error -c $(BUILD_DIR)/alias_probe.c -o $(BUILD_DIR)/alias_probe.o 2>&1); \
	ctl=$$($(CC) $(CFLAGS) -Wno-error -c $(BUILD_DIR)/alias_clean.c -o $(BUILD_DIR)/alias_clean.o 2>&1); ctlrc=$$?; \
	if [ $$ctlrc -ne 0 ]; then \
		printf 'check-aliasing: the control file did not compile, so this gate is measuring nothing:\n%s\n' "$$ctl" >&2; \
		exit 1; \
	fi; \
	if printf '%s' "$$ctl" | grep -q 'strict-aliasing'; then \
		printf 'check-aliasing: the control file drew a strict-aliasing diagnostic, so the probe proves nothing:\n%s\n' "$$ctl" >&2; \
		exit 1; \
	fi; \
	if printf '%s' "$$probe" | grep -q 'strict-aliasing'; then \
		printf 'check-aliasing: the planted violation is reported\n'; \
		exit 0; \
	fi; \
	qout=$$($(CC) -Q --help=warnings $(CFLAGS) 2>/dev/null); qrc=$$?; \
	level=$$(printf '%s' "$$qout" | awk '/-Wstrict-aliasing=</{print $$2}'); \
	if [ $$qrc -ne 0 ] || [ -z "$$level" ]; then \
		printf 'check-aliasing: no diagnostic, and %s reports no -Wstrict-aliasing level. That is a compiler which accepts the option and implements nothing.\n' "$(CC)" >&2; \
	elif [ "$$level" = 1 ]; then \
		printf 'check-aliasing: level 1 is set and the planted store still drew no diagnostic.\n' >&2; \
	else \
		printf 'check-aliasing: CFLAGS resolves to -Wstrict-aliasing=%s; only level 1 reports this probe.\n' "$$level" >&2; \
	fi; \
	exit 1

check-stamps: ## Fail if a compile rule names no flags stamp, or a stamp omits a variable
	@python3 tools/check-stamps.py

check-fp-contract: ## Fail if any guest-float compile flags permit contraction
	@tools/check-fp-contract.sh

check-symbols: $(APP_DIR)/$(TARGET) ## Fail if any exported symbol lacks the version namespace
	@tools/check-symbols.sh $(APP_DIR)/$(TARGET) $(LIBVER_SYMBOL) .
ifeq ($(OS_NAME), Windows)
	@tools/check-symbols-pe-controls.sh $(LIBVER_SYMBOL) .
endif

check-labels: ## Fail if a public header has no @stability free label
	@tools/check-labels.sh .

check-edges: $(APP_DIR)/$(TARGET) ## Fail on a dependency outside cutil, runtime-core, and runtime-jit
	@tools/check-edges.sh --includes .
	@tools/check-edges.sh --links $(APP_DIR)

check-wast-corpus: ## Fail on corpus drift or an incomplete file classification
	@python3 tools/test-check-wast-corpus.py
	@python3 tools/check-wast-corpus.py --metadata ../runtime-jit/tools/wasm \
		--corpus ../runtime-jit/third_party/wasm-spec \
		--classification tests/data-wast-classification.tsv \
		--assertion-skips tests/data-wast-assertion-skips.tsv

.PHONY: check-benchmark-summarizer
check-benchmark-summarizer: ## Validate Story 13 raw-sample completeness and pairing rules
	@python3 tests/unit/test_benchmark_summarize.py

.PHONY: check-gtest-full
check-gtest-full: ## Prove full-suite selection and zero-test rejection
	@tools/run-gtest-full.sh --selftest

test: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES) $(TEST_GATES) check-gtest-full ## Build and run the tests
	@for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		printf '\n### Running %s ###\n\n' "$$test_name"; \
		LD_LIBRARY_PATH="$(TEST_LD_PATH)" tools/run-gtest-full.sh $$test_exe || exit 1; \
	done

SPEC_SUITE_APP := $(APP_DIR)/testSpec_suite$(EXE_EXTENSION)
.PHONY: test-spec-suite
test-spec-suite: $(APP_DIR)/$(TARGET) $(SPEC_SUITE_APP) ## Run only the pinned spec suite, in both tiers
	LD_LIBRARY_PATH="$(TEST_LD_PATH)" tools/run-gtest-full.sh $(SPEC_SUITE_APP)

test-quiet: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES) ## Run tests, one line per suite
	@for test_exe in $(TEST_EXECUTABLES); do \
		output=$$(LD_LIBRARY_PATH="$(TEST_LD_PATH)" tools/run-gtest-full.sh $$test_exe 2>&1); \
		rc=$$?; \
		if [ $$rc -ne 0 ]; then printf '%s\n' "$$output"; exit $$rc; fi; \
		summary=$$(printf '%s\n' "$$output" | grep '^run-gtest-full: completed ' | tail -1); \
		if [ -z "$$summary" ]; then printf '%s\n' "$$output"; exit 2; fi; \
		printf '%s: %s\n' "$$(basename $$test_exe $(EXE_EXTENSION))" "$$summary"; \
	done

test-gc-torture: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES) $(TEST_GATES) ## Run the full suite with runtime-heap torture enabled
	@for test_exe in $(TEST_EXECUTABLES); do \
		printf '\n### GC torture %s ###\n\n' "$$(basename $$test_exe)"; \
		GRHEAP_TORTURE=1 LD_LIBRARY_PATH="$(TEST_LD_PATH)" \
			tools/run-gtest-full.sh $$test_exe || exit 1; \
	done

test-valgrind: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES) ## Run the tests under Valgrind
ifeq ($(OS_NAME), Linux)
	@for test_exe in $(TEST_EXECUTABLES); do \
		printf '\n### Valgrind %s ###\n\n' "$$(basename $$test_exe)"; \
		LD_LIBRARY_PATH="$(TEST_LD_PATH)" tools/run-gtest-full.sh valgrind $(VALGRIND_FLAGS) $$test_exe || exit 1; \
	done
else
	@printf 'Valgrind is only available on Linux\n' >&2; exit 1
endif

####################################################################
# ASan + UBSan, in their own tree
####################################################################

UBSAN_CHECKS := undefined,float-cast-overflow
ASAN_UBSAN_FLAGS := -fsanitize=address,$(UBSAN_CHECKS) -fno-sanitize-recover=$(UBSAN_CHECKS) -fno-omit-frame-pointer -g -O1
COV_BUILD_DIR := ./build/$(BUILD)-cov
ASAN_BUILD_DIR := ./build/$(BUILD)-asan
ASAN_OBJ_DIR := $(ASAN_BUILD_DIR)/objects
ASAN_FLAGS_STAMP := $(ASAN_OBJ_DIR)/.flags
ASAN_APP_DIR := $(ASAN_BUILD_DIR)/apps
ASAN_LIBOBJECTS := $(patsubst src/%.c,$(ASAN_OBJ_DIR)/%.o,$(SOURCES))
ASAN_DEPFILES := $(ASAN_LIBOBJECTS:.o=.d) \
    $(foreach pair,$(TEST_PAIRS),$(ASAN_OBJ_DIR)/tests/$(basename $(notdir $(word 1,$(subst |, ,$(pair))))).d)
-include $(ASAN_DEPFILES)
ASAN_ARCHIVE := $(ASAN_APP_DIR)/$(STATIC_TARGET)
ASAN_WASMLIBRARY := -Wl,--whole-archive $(ASAN_ARCHIVE) -Wl,--no-whole-archive $(RCORE_LIBS) $(CUTIL_LIBS) $(RHEAP_LIBS)
ASAN_CFLAGS := $(CFLAGS) $(ASAN_UBSAN_FLAGS) -DGLWASM_BUILD
ASAN_CXXFLAGS := $(CXXFLAGS) $(ASAN_UBSAN_FLAGS)
ASAN_LDFLAGS := $(LDFLAGS) $(ASAN_UBSAN_FLAGS)
ifeq ($(UNAME_S), Linux)
	ASAN_CFLAGS += -fPIC
endif

$(ASAN_OBJ_DIR)/%.o: src/%.c $(ASAN_FLAGS_STAMP) | $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CC) $(ASAN_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(ASAN_ARCHIVE): $(ASAN_LIBOBJECTS)
	@mkdir -p $(@D)
	@rm -f $@
	ar rcs $@ $^

$(ASAN_OBJ_DIR)/tests/%.o: tests/unit/%.cpp $(ASAN_FLAGS_STAMP) | $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) $(TEST_INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

define asan-test-executable-rule
ASAN_TEST_OBJ_$1 := $(ASAN_OBJ_DIR)/tests/$(basename $(notdir $1)).o

$(ASAN_APP_DIR)/$2$(EXE_EXTENSION): $$(ASAN_TEST_OBJ_$1) $(ASAN_ARCHIVE)
	@mkdir -p $$(@D)
	$(CXX) $(ASAN_CXXFLAGS) -o $$@ $$(ASAN_TEST_OBJ_$1) $(ASAN_LDFLAGS) $(ASAN_WASMLIBRARY) $(RJIT_LIBS) $(RDBG_LIBS) $(TESTFLAGS)
endef

$(foreach pair,$(TEST_PAIRS),\
	$(eval $(call asan-test-executable-rule,$(word 1,$(subst |, ,$(pair))),$(word 2,$(subst |, ,$(pair))))))

ASAN_TEST_EXECUTABLES := $(addprefix $(ASAN_APP_DIR)/,$(addsuffix $(EXE_EXTENSION),$(TEST_NAMES)))
ASAN_RUNTIME := $(shell $(CXX) -print-file-name=libasan.so 2>/dev/null)
ifneq ($(findstring clang,$(shell $(CXX) --version 2>/dev/null)),)
ASAN_PRELOAD = GTEST_FULL_TEST_LD_PRELOAD= LD_PRELOAD=
else
ASAN_PRELOAD = GTEST_FULL_TEST_LD_PRELOAD="$(ASAN_RUNTIME)$${LD_PRELOAD:+:$$LD_PRELOAD}" LD_PRELOAD=
endif

test-asan: $(ASAN_TEST_EXECUTABLES) ## Build with ASan+UBSan and run the tests
	@for test_exe in $(ASAN_TEST_EXECUTABLES); do \
		printf '\n### ASan+UBSan %s ###\n\n' "$$(basename $$test_exe)"; \
		$(ASAN_PRELOAD) \
		LD_LIBRARY_PATH="$(ASAN_APP_DIR):$(CUTIL_LIBDIR):$(LIB_INSTALL_PATH)/$(SUITE)" \
			tools/run-gtest-full.sh $$test_exe || exit 1; \
	done
	@printf '\nASan+UBSan suite clean.\n'

####################################################################
# ThreadSanitizer, in its own tree
####################################################################

TSAN_FLAGS := -fsanitize=thread -fno-omit-frame-pointer -g -O1
TSAN_BUILD_DIR := ./build/$(BUILD)-tsan
TSAN_OBJ_DIR := $(TSAN_BUILD_DIR)/objects
TSAN_FLAGS_STAMP := $(TSAN_OBJ_DIR)/.flags
TSAN_APP_DIR := $(TSAN_BUILD_DIR)/apps
TSAN_LIBOBJECTS := $(patsubst src/%.c,$(TSAN_OBJ_DIR)/%.o,$(SOURCES))
TSAN_DEPFILES := $(TSAN_LIBOBJECTS:.o=.d) \
    $(foreach pair,$(TEST_PAIRS),$(TSAN_OBJ_DIR)/tests/$(basename $(notdir $(word 1,$(subst |, ,$(pair))))).d)
-include $(TSAN_DEPFILES)
TSAN_ARCHIVE := $(TSAN_APP_DIR)/$(STATIC_TARGET)
TSAN_WASMLIBRARY := -Wl,--whole-archive $(TSAN_ARCHIVE) -Wl,--no-whole-archive \
	$(RJIT_LIBS) $(RDBG_LIBS) $(RHEAP_LIBS) $(RCORE_LIBS) $(CUTIL_LIBS)
TSAN_CFLAGS := $(CFLAGS) $(TSAN_FLAGS) -DGLWASM_BUILD
TSAN_CXXFLAGS := $(CXXFLAGS) $(TSAN_FLAGS)
TSAN_LDFLAGS := $(LDFLAGS) $(TSAN_FLAGS)
ifeq ($(UNAME_S), Linux)
	TSAN_CFLAGS += -fPIC
endif

$(TSAN_OBJ_DIR)/%.o: src/%.c $(TSAN_FLAGS_STAMP) | $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CC) $(TSAN_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(TSAN_ARCHIVE): $(TSAN_LIBOBJECTS)
	@mkdir -p $(@D)
	@rm -f $@
	ar rcs $@ $^

$(TSAN_OBJ_DIR)/tests/%.o: tests/unit/%.cpp $(TSAN_FLAGS_STAMP) | $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CXX) $(TSAN_CXXFLAGS) $(TEST_INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

define tsan-test-executable-rule
TSAN_TEST_OBJ_$1 := $(TSAN_OBJ_DIR)/tests/$(basename $(notdir $1)).o

$(TSAN_APP_DIR)/$2$(EXE_EXTENSION): $$(TSAN_TEST_OBJ_$1) $(TSAN_ARCHIVE)
	@mkdir -p $$(@D)
	$(CXX) $(TSAN_CXXFLAGS) -o $$@ $$(TSAN_TEST_OBJ_$1) $(TSAN_LDFLAGS) $(TEST_LDFLAGS) $(TSAN_WASMLIBRARY) $(TESTFLAGS)
endef

$(foreach pair,$(TEST_PAIRS),\
	$(eval $(call tsan-test-executable-rule,$(word 1,$(subst |, ,$(pair))),$(word 2,$(subst |, ,$(pair))))))

TSAN_TEST_EXECUTABLES := $(addprefix $(TSAN_APP_DIR)/,$(addsuffix $(EXE_EXTENSION),$(TEST_NAMES)))
TSAN_RUN_ENV := LD_PRELOAD= LD_LIBRARY_PATH="$(TSAN_APP_DIR):$(RJIT_LIBDIR):$(RCORE_LIBDIR):$(CUTIL_LIBDIR):$(RDBG_LIBDIR):$(RHEAP_LIBDIR):$(LIB_INSTALL_PATH)/$(SUITE)" \
	TSAN_OPTIONS="halt_on_error=1:history_size=7:second_deadlock_stack=1"

test-tsan: $(TSAN_TEST_EXECUTABLES) ## Build with TSan and run the tests (Linux only)
ifeq ($(OS_NAME), Linux)
	@for test_exe in $(TSAN_TEST_EXECUTABLES); do \
		printf '\n### TSan %s ###\n\n' "$$(basename $$test_exe)"; \
		$(TSAN_RUN_ENV) tools/run-gtest-full.sh $$test_exe || exit 1; \
	done
	@printf '\nTSan suite clean.\n'
else
	@printf 'ThreadSanitizer builds are only supported on Linux\n' >&2; exit 1
endif

####################################################################
# Fuzzing
####################################################################

FUZZ_CC ?= clang
FUZZ_CXX ?= clang++
FUZZ_CC_OK := $(shell which $(FUZZ_CC) 2>/dev/null)
FUZZ_SAN := -fsanitize=address,$(UBSAN_CHECKS) -fno-omit-frame-pointer -g -O1 -fstrict-aliasing -ffp-contract=off
FUZZ_LIB_FLAGS := $(FUZZ_SAN) -fsanitize=fuzzer-no-link
FUZZ_BIN_FLAGS := $(FUZZ_SAN) -fsanitize=fuzzer
FUZZ_DIR := $(BUILD_DIR)/fuzz
FUZZ_OBJ_DIR := $(FUZZ_DIR)/objects
FUZZ_FLAGS_STAMP := $(FUZZ_OBJ_DIR)/.flags
FUZZ_APP_DIR := $(FUZZ_DIR)/apps
FUZZ_OBJECTS := $(patsubst src/%.c,$(FUZZ_OBJ_DIR)/%.o,$(SOURCES))
FUZZ_CORPUS := tests/fuzz/corpus
FUZZ_TIME ?= 5

$(FUZZ_OBJ_DIR)/%.o: src/%.c $(FUZZ_FLAGS_STAMP) | $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(FUZZ_CC) $(FUZZ_LIB_FLAGS) -std=c17 -fvisibility=hidden -DGLWASM_BUILD $(INCLUDE) -c $< -o $@

define fuzz-rule
fuzz-$2: $$(FUZZ_APP_DIR)/fuzz_$2 ## Build the $2 fuzzer

$$(FUZZ_APP_DIR)/fuzz_$2: tests/fuzz/fuzz_$2.c $$(FUZZ_OBJECTS) $$(FUZZ_FLAGS_STAMP) | $(LIBVER_GEN)
	@if [ -z "$$(FUZZ_CC_OK)" ]; then \
		echo "fuzzing requires $$(FUZZ_CC)"; \
		exit 1; \
	fi
	@mkdir -p $$(@D) $$(FUZZ_CORPUS)/$2
	$$(FUZZ_CC) $$(FUZZ_BIN_FLAGS) -std=c17 $(INCLUDE) \
		-o $$@ $$< $$(FUZZ_OBJECTS) $(RJIT_LIBS) $(RCORE_LIBS) $(CUTIL_LIBS) -lm

fuzz-run-$2: $$(FUZZ_APP_DIR)/fuzz_$2 ## Run the $2 fuzzer for $$(FUZZ_TIME) seconds
	@mkdir -p $$(FUZZ_CORPUS)/$2
	@LD_LIBRARY_PATH="$(TEST_LD_PATH)" $$(FUZZ_APP_DIR)/fuzz_$2 $$(FUZZ_CORPUS)/$2 \
		-max_total_time=$$(FUZZ_TIME) -print_final_stats=1
endef

FUZZ_SOURCES := $(wildcard tests/fuzz/fuzz_*.c)
FUZZERS := $(patsubst tests/fuzz/fuzz_%.c,%,$(FUZZ_SOURCES))

$(foreach f,$(FUZZERS),$(eval $(call fuzz-rule,$(f),$(f))))

fuzz: $(addprefix fuzz-run-,$(FUZZERS)) ## Build and run every fuzzer briefly
ifeq ($(strip $(FUZZERS)),)
	@printf 'fuzz: no fuzzers in tests/fuzz\n' >&2; exit 1
endif

$(APP_DIR)/fuzz/replay_%$(EXE_EXTENSION): tests/fuzz/fuzz_%.c tests/fuzz/replay_main.c \
		$(APP_DIR)/$(STATIC_TARGET) $(FLAGS_STAMP) | $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ tests/fuzz/fuzz_$*.c tests/fuzz/replay_main.c $(LDFLAGS) $(WASMLIBRARY) -lm

fuzz-replay: $(APP_DIR)/fuzz/replay_decode$(EXE_EXTENSION) $(APP_DIR)/fuzz/replay_validate$(EXE_EXTENSION) ## Replay the seed corpus
	@files=$$(find tests/fuzz/corpus -type f | sort); \
	if [ -z "$$files" ]; then \
		printf 'fuzz-replay: no corpus files\n' >&2; \
		exit 1; \
	fi; \
	LD_LIBRARY_PATH="$(TEST_LD_PATH)" $(APP_DIR)/fuzz/replay_decode$(EXE_EXTENSION) $$files || exit $$?; \
	LD_LIBRARY_PATH="$(TEST_LD_PATH)" $(APP_DIR)/fuzz/replay_validate$(EXE_EXTENSION) $$files

fuzz-clean: ## Remove the fuzz build
	-@rm -rf $(FUZZ_DIR)

####################################################################
# Install
####################################################################

LDCONF_INSTALL_PATH ?= /etc/ld.so.conf.d
PC_REQUIRES := $(CUTIL_PC) $(RCORE_PC) $(RJIT_PC)
PKGCONFIG_INSTALL_PATH ?= $(PC_INSTALL_PATH)

CHECK_INSTALL_PREFIX ?= $(if $(PREFIX),$(PREFIX),/usr/local)
CHECK_INSTALL_MODULE ?= ghoti.io-lang-wasm$(BRANCH)
check-install: ## Verify an installed consumer using pkg-config and the public umbrella header
	@tools/check-install.sh "$(CHECK_INSTALL_PREFIX)" "$(CHECK_INSTALL_MODULE)"

install: all ## Install the library
	@mkdir -p $(LIB_INSTALL_PATH)/$(SUITE)
ifeq ($(OS_NAME), Linux)
	@cp $(APP_DIR)/$(TARGET) $(LIB_INSTALL_PATH)/$(SUITE)/
	@ln -f -s $(TARGET) $(LIB_INSTALL_PATH)/$(SUITE)/$(SO_NAME)
	@ln -f -s $(SO_NAME) $(LIB_INSTALL_PATH)/$(SUITE)/$(BASE_NAME)
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then mkdir -p $(LDCONF_INSTALL_PATH); fi
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then echo "$(LIB_INSTALL_PATH)/$(SUITE)" > $(LDCONF_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).conf; fi
endif
ifeq ($(OS_NAME), Windows)
	@mkdir -p $(BIN_INSTALL_PATH) $(LIB_INSTALL_PATH)/$(SUITE)
	@cp $(APP_DIR)/$(TARGET).a $(LIB_INSTALL_PATH)/$(SUITE)/
	@cp $(APP_DIR)/$(TARGET) $(BIN_INSTALL_PATH)/
endif
	@rm -rf $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	@mkdir -p $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	@if [ -d include/ghoti.io ]; then \
		cp -r include/ghoti.io $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)/ ; \
	fi
	@if [ -d $(GEN_DIR)/ghoti.io ]; then \
		cp -r $(GEN_DIR)/ghoti.io $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)/ ; \
	fi
	@mkdir -p $(PKGCONFIG_INSTALL_PATH)
	@cat pkgconfig/$(SUITE)-$(PROJECT).pc | sed 's/(SUITE)/$(SUITE)/g; s/(PROJECT)/$(PROJECT)/g; s/(BRANCH)/$(BRANCH)/g; s/(VERSION)/$(VERSION)/g; s|(PC_LIB_DIR)|$(PC_LIB_DIR)|g; s|(PC_INCLUDE_DIR)|$(PC_INCLUDE_DIR)|g; s|(REQUIRES)|$(PC_REQUIRES)|g' > $(PKGCONFIG_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).pc
ifeq ($(OS_NAME), Linux)
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then ldconfig >> /dev/null 2>&1; fi
endif
	@echo "Ghoti.io $(PROJECT)$(BRANCH) installed"

uninstall: ## Delete the installed files
ifeq ($(OS_NAME), Linux)
	@rm -f $(LIB_INSTALL_PATH)/$(SUITE)/$(BASE_NAME)*
	@rm -f $(LDCONF_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).conf
endif
	@rm -rf $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	@rm -f $(PKGCONFIG_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).pc
	@echo "Ghoti.io $(PROJECT)$(BRANCH) has been uninstalled"

docs: ## Generate documentation under docs/
	doxygen

docs-pdf: docs ## Generate the PDF manual
	cd ./docs/latex/ && make
	mv -f ./docs/latex/refman.pdf ./docs/$(SUITE)-$(PROJECT)$(BRANCH)-docs.pdf

cloc: ## Count the lines of code used in the project
	cloc src include tests Makefile

coverage: ## Build instrumented, run the tests, and report line coverage
	@rm -rf $(COV_BUILD_DIR)/objects/*.gcda \
		$(COV_BUILD_DIR)/objects/*/*.gcda 2> /dev/null || true
	@status=0; \
	$(MAKE) --no-print-directory test TEST_GATES= \
		BUILD_DIR=$(COV_BUILD_DIR) \
		EXTRA_CFLAGS="--coverage -O0 -fprofile-update=atomic" \
		EXTRA_LDFLAGS="--coverage" > /dev/null || status=$$?; \
	if [ $$status -eq 0 ]; then \
		tools/coverage.sh $(COV_BUILD_DIR)/objects || status=$$?; \
	else \
		printf 'coverage: the instrumented test run failed; no report\n' >&2; \
	fi; \
	exit $$status

clean: ## Remove the build directories
	-@rm -rf ./build

####################################################################
# Optional Story 11 real-program corpus gate. Source and toolchain fetches are
# explicit so `make test` remains hermetic and does not require network access.
####################################################################

REAL_CORPUS_OBJECT := $(OBJ_DIR)/tests/real_corpus_test.o
REAL_CORPUS_APP := $(APP_DIR)/test_real_corpus$(EXE_EXTENSION)
REAL_CORPUS_RELOCATE_OBJECT := $(OBJ_DIR)/tests/real_corpus_relocate_test.o
REAL_CORPUS_RELOCATE_APP := $(APP_DIR)/test_real_corpus_relocate$(EXE_EXTENSION)
-include $(REAL_CORPUS_OBJECT:.o=.d) $(REAL_CORPUS_RELOCATE_OBJECT:.o=.d)
REAL_CORPUS_BUILD_DIR ?= $(if $(GLWASM_REAL_CORPUS_BUILD_DIR),$(GLWASM_REAL_CORPUS_BUILD_DIR),$(abspath build/real-corpus))
NEED_REAL_CORPUS_HEAP := $(filter test-real-corpus test-real-corpus-relocate,$(MAKECMDGOALS))
ifneq ($(strip $(NEED_REAL_CORPUS_HEAP)),)
ifeq ($(strip $(RHEAP_LIBS)),)
$(error ghoti.io-runtime-heap was not found by pkg-config; the real-corpus stress modes require it)
endif
endif

$(REAL_CORPUS_OBJECT): tests/real-corpus/real_corpus_test.cpp $(FLAGS_STAMP) | $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(TEST_INCLUDE) $(RHEAP_CFLAGS) -MMD -MP -MF $(@:.o=.d) -c $< -o $@

$(REAL_CORPUS_APP): $(REAL_CORPUS_OBJECT) $(APP_DIR)/$(STATIC_TARGET) $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS) $(TEST_LDFLAGS) $(TEST_WASMLIBRARY) $(RHEAP_LIBS) $(TESTFLAGS)

.PHONY: check-real-corpus-relocate-header
check-real-corpus-relocate-header:
	@test -f "$(patsubst -I%,%,$(firstword $(RHEAP_CFLAGS)))/ghoti.io/runtime-heap/relocate.h" || { \
		printf 'test-real-corpus-relocate requires a RELOCATE=yes runtime-heap install on PKG_CONFIG_PATH\n' >&2; \
		exit 1; \
	}

$(REAL_CORPUS_RELOCATE_OBJECT): tests/real-corpus/real_corpus_test.cpp $(FLAGS_STAMP) | check-real-corpus-relocate-header $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(TEST_INCLUDE) $(RHEAP_CFLAGS) -DGLWASM_CORPUS_RELOCATE -MMD -MP -MF $(@:.o=.d) -c $< -o $@

$(REAL_CORPUS_RELOCATE_APP): $(REAL_CORPUS_RELOCATE_OBJECT) $(APP_DIR)/$(STATIC_TARGET) $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -o $@ $< $(RHEAP_RELOCATE_LDFLAGS) $(LDFLAGS) $(TEST_LDFLAGS) $(TEST_WASMLIBRARY) $(RHEAP_LIBS) $(TESTFLAGS)

.PHONY: test-real-corpus
test-real-corpus: $(REAL_CORPUS_APP) ## Verify pinned native, interpreter, JIT and Wasmtime real-program corpus
	@LD_LIBRARY_PATH="$(TEST_LD_PATH):$(RHEAP_LIBDIR)" python3 tests/real-corpus/test_selection_guard.py "$(REAL_CORPUS_APP)" 12
	@GLWASM_REAL_CORPUS_BUILD_DIR="$(REAL_CORPUS_BUILD_DIR)" tools/real-corpus/build-and-run.sh --all-tiers
	@GLWASM_REAL_CORPUS_DIR="$(abspath tests/real-corpus)" \
	 GLWASM_REAL_CORPUS_MODULE_DIR="$(REAL_CORPUS_BUILD_DIR)/modules" \
	 LD_LIBRARY_PATH="$(TEST_LD_PATH):$(RHEAP_LIBDIR)" $(REAL_CORPUS_APP) --gtest_brief=1

.PHONY: test-real-corpus-relocate
test-real-corpus-relocate: $(REAL_CORPUS_RELOCATE_APP) ## Require compiled runtime-heap relocation mode and run the full corpus in both tiers
	@LD_LIBRARY_PATH="$(RHEAP_RELOCATE_LD_PATH)" python3 tests/real-corpus/test_selection_guard.py "$(REAL_CORPUS_RELOCATE_APP)" 18
	@GLWASM_REAL_CORPUS_BUILD_DIR="$(REAL_CORPUS_BUILD_DIR)" tools/real-corpus/build-and-run.sh --all-tiers
	@GLWASM_REAL_CORPUS_DIR="$(abspath tests/real-corpus)" \
	 GLWASM_REAL_CORPUS_MODULE_DIR="$(REAL_CORPUS_BUILD_DIR)/modules" \
	 LD_LIBRARY_PATH="$(RHEAP_RELOCATE_LD_PATH)" $(REAL_CORPUS_RELOCATE_APP) --gtest_brief=1

help: ## Display this help
	@grep -E '^[ a-zA-Z_-]+:.*?## .*$$' Makefile | sort | sed 's/\([^:]*\):.*## \(.*\)/\1:\2/' | awk -F: '{printf "%-22s %s\n", $$1, $$2}'

####################################################################
# Flag stamps. At the end so they are not the default goal.
####################################################################

.PHONY: force-flags

$(FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(CC) $(CXX) $(LIB_CFLAGS) $(CFLAGS) $(CXXFLAGS) $(LDFLAGS) $(INCLUDE) $(WASMLIBRARY) $(RCORE_LIBS) $(RCORE_CFLAGS) $(CUTIL_LIBS) $(CUTIL_CFLAGS) $(TEST_INCLUDE) $(RJIT_LIBS) $(RDBG_LIBS) $(RHEAP_CFLAGS) $(RHEAP_LIBS) $(RHEAP_RELOCATE_LDFLAGS) $(TEST_WASMLIBRARY) $(BENCH_GSEC_CFLAGS) $(BENCH_GSEC_LIBS) $(TESTFLAGS) $(TEST_LDFLAGS) $(OS_SPECIFIC_LIBRARY_NAME_FLAG)' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@

$(ASAN_FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(CC) $(CXX) $(ASAN_CFLAGS) $(ASAN_CXXFLAGS) $(ASAN_LDFLAGS) $(INCLUDE) $(TEST_INCLUDE) $(ASAN_WASMLIBRARY) $(RCORE_LIBS) $(RCORE_CFLAGS) $(CUTIL_LIBS) $(RJIT_LIBS) $(RDBG_LIBS) $(TESTFLAGS)' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@

$(TSAN_FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(CC) $(CXX) $(TSAN_CFLAGS) $(TSAN_CXXFLAGS) $(TSAN_LDFLAGS) $(INCLUDE) $(TEST_INCLUDE) $(TSAN_WASMLIBRARY) $(RJIT_LIBS) $(RDBG_LIBS) $(RHEAP_LIBS) $(RCORE_LIBS) $(CUTIL_LIBS) $(TESTFLAGS) $(TEST_LDFLAGS)' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@

$(FUZZ_FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(FUZZ_CC) $(FUZZ_CXX) $(FUZZ_LIB_FLAGS) $(FUZZ_BIN_FLAGS) $(INCLUDE) $(RJIT_LIBS) $(RCORE_LIBS) $(RCORE_CFLAGS) $(CUTIL_LIBS) $(FUZZ_SAN)' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@

include benchmarks/targets.mk
