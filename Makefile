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

.PHONY: all test test-link clean

all: $(BUILD)/ncc-test $(BUILD)/p0demo

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

test: $(BUILD)/ncc-test
	NCC_TEST_TMP=$(BUILD) $(BUILD)/ncc-test

test-link: $(BUILD)/p0demo
	sh tests/link/run.sh $(BUILD)

clean:
	rm -rf $(BUILD)

-include $(LIB_OBJ:.o=.d) $(TEST_OBJ:.o=.d) $(BUILD)/src/tools/p0demo.d
