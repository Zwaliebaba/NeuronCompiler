# NeuronCompiler — development build (Linux/macOS hosts; see build.cmd for Windows).
# The only build requirement is a C23 compiler (Design/README.md D-07); make is
# a convenience, every target is a plain compiler invocation.

CC      ?= clang
BUILD   ?= build
CSTD    ?= -std=c2x
WARN    ?= -Wall -Wextra -Werror -Wshadow -Wno-unused-parameter
OPT     ?= -O1 -g
CFLAGS  += $(CSTD) $(WARN) $(OPT)
LDLIBS  += -lm

LIB_SRC := $(wildcard src/core/*.c) \
           $(wildcard src/target/arm64/*.c) \
           $(wildcard src/obj/coff/*.c) \
           $(wildcard src/debug/codeview/*.c)
LIB_OBJ := $(LIB_SRC:%.c=$(BUILD)/%.o)
TEST_SRC := $(wildcard tests/unit/*.c)
TEST_OBJ := $(TEST_SRC:%.c=$(BUILD)/%.o)

.PHONY: all test test-link check-windows check-format tidy check-tree clean

all: $(BUILD)/ncc-test $(BUILD)/p0demo $(BUILD)/ncc-uwcheck $(BUILD)/ncc-check-tree

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/libncc.a: $(LIB_OBJ)
	@rm -f $@
	ar rcs $@ $^

$(BUILD)/ncc-test: $(TEST_OBJ) $(BUILD)/libncc.a
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/p0demo: $(BUILD)/src/tools/p0demo.o $(BUILD)/libncc.a
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/ncc-uwcheck: $(BUILD)/src/tools/uwcheck.o $(BUILD)/libncc.a
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/ncc-check-tree: $(BUILD)/src/tools/checktree.o $(BUILD)/libncc.a
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

test: $(BUILD)/ncc-test
	NCC_TEST_TMP=$(BUILD) $(BUILD)/ncc-test

test-link: $(BUILD)/p0demo $(BUILD)/ncc-uwcheck
	sh tests/link/run.sh $(BUILD)

# Compiles every source for Windows ARM64 against the mingw-w64 headers
# (syntax and type checking only: catches _WIN32-only breakage on Linux).
MINGW_INC ?= /usr/share/mingw-w64/include
check-windows:
	@for f in $(LIB_SRC) $(TEST_SRC) src/tools/*.c; do \
	  clang --target=aarch64-w64-windows-gnu -isystem $(MINGW_INC) $(CSTD) $(WARN) \
	        -fsyntax-only $$f || exit 1; done
	@echo "all sources compile for Windows ARM64"

# ---- the conformance gates (AGENTS.md §6) ---------------------------------
# The LLVM tools are PINNED: "clean" is only meaningful under a named version,
# and a runner image moving LLVM underneath the gate must not be able to turn
# main red with no change to the code (Q-28). 18.1.3 is what ubuntu-24.04
# installs.
LLVM_PIN     ?= 18.1.3
CLANG_FORMAT ?= clang-format
CLANG_TIDY   ?= clang-tidy
FORMAT_FILES := $(sort $(shell find src tests -type f \( -name '*.[ch]' -o -name '*.inc' \)))
TIDY_FILES   := $(sort $(shell find src tests -type f -name '*.c'))
TREE_FILES   := $(sort $(shell find src tests -type f))

define require_llvm_pin
	@$(1) --version | grep -q 'version $(LLVM_PIN)' || { \
	  echo "$(2) needs $(1) $(LLVM_PIN) (pinned, AGENTS.md §6); found: $$($(1) --version | head -1)"; exit 1; }
endef

check-format:
	$(call require_llvm_pin,$(CLANG_FORMAT),check-format)
	@$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)
	@echo "check-format: $(words $(FORMAT_FILES)) files clean (clang-format $(LLVM_PIN))"

# Every translation unit twice: for the host, and for Windows ARM64 against the
# mingw-w64 headers (as check-windows compiles), so the _WIN32 halves are linted.
tidy:
	$(call require_llvm_pin,$(CLANG_TIDY),tidy)
	@for f in $(TIDY_FILES); do \
	  $(CLANG_TIDY) --quiet $$f -- $(CSTD) || exit 1; \
	  $(CLANG_TIDY) --quiet $$f -- $(CSTD) --target=aarch64-w64-windows-gnu -isystem $(MINGW_INC) || exit 1; \
	done
	@echo "tidy: $(words $(TIDY_FILES)) translation units clean, host and Windows ARM64 (clang-tidy $(LLVM_PIN))"

check-tree: $(BUILD)/ncc-check-tree
	@$(BUILD)/ncc-check-tree $(TREE_FILES)

clean:
	rm -rf $(BUILD)

-include $(LIB_OBJ:.o=.d) $(TEST_OBJ:.o=.d) $(BUILD)/src/tools/p0demo.d $(BUILD)/src/tools/uwcheck.d $(BUILD)/src/tools/checktree.d
