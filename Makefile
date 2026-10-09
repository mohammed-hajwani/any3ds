# Run inside WSL:  make test
#
# Chain per test program:
#   tests/m0/X.{c,S} --arm-none-eabi-gcc--> out/m0/X.o
#                    --python -m recomp-->  out/m0/gen_X.c
#                    --clang + harness-->   out/m0/X_harness  (run it)

ARM_CC    := arm-none-eabi-gcc
OBJDUMP   := arm-none-eabi-objdump
ARM_FLAGS := -marm -mcpu=mpcore -O1

HOST_CC    := clang
HOST_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror \
              -fsanitize=undefined,address -fno-sanitize-recover=all \
              -Iruntime

VENV   := .venv
PYTHON := $(VENV)/bin/python

OUT         := out/m0
RECOMP_SRC  := $(wildcard recomp/*.py)
RUNTIME_HDR := $(wildcard runtime/*.h)

# devkitARM lives outside PATH unless the shell profile adds it.
ifdef DEVKITARM
export PATH := $(DEVKITARM)/bin:$(PATH)
endif

.PHONY: test venv clean check-unsupported
.SECONDARY:          # keep .o / generated .c around for inspection
.DELETE_ON_ERROR:

test: $(OUT)/test.dis $(OUT)/shifts.dis $(OUT)/test_harness $(OUT)/shifts_harness check-unsupported
	@echo '--- M0: test(a, b) = (a + b) * 3 ---'
	@./$(OUT)/test_harness
	@echo '--- barrel shifter ---'
	@./$(OUT)/shifts_harness

venv: $(VENV)/.installed

$(VENV)/.installed: requirements.txt
	python3 -m venv $(VENV)
	$(PYTHON) -m pip install -q -r requirements.txt
	touch $@

$(OUT):
	mkdir -p $@

# Guest side: build bare ARM objects (no libctru).
$(OUT)/%.o: tests/m0/%.c | $(OUT)
	$(ARM_CC) $(ARM_FLAGS) -c $< -o $@

$(OUT)/%.o: tests/m0/%.S | $(OUT)
	$(ARM_CC) $(ARM_FLAGS) -c $< -o $@

# Reference disassembly, to compare by eye with the generated C.
$(OUT)/%.dis: $(OUT)/%.o
	$(OBJDUMP) -d $< > $@

# Recompile.
$(OUT)/gen_%.c: $(OUT)/%.o $(RECOMP_SRC) $(VENV)/.installed
	$(PYTHON) -m recomp $< -o $@

# Host side: generated code + harness.
$(OUT)/test_harness: tests/m0/harness.c $(OUT)/gen_test.c $(RUNTIME_HDR)
	$(HOST_CC) $(HOST_FLAGS) tests/m0/harness.c $(OUT)/gen_test.c -o $@

$(OUT)/shifts_harness: tests/m0/shifts_harness.c $(OUT)/gen_shifts.c $(RUNTIME_HDR)
	$(HOST_CC) $(HOST_FLAGS) tests/m0/shifts_harness.c $(OUT)/gen_shifts.c -o $@

# The recompiler must refuse code it can't translate, name the address and
# mnemonic, and write no output.
check-unsupported: $(OUT)/unsupported.o $(RECOMP_SRC) $(VENV)/.installed
	@rm -f $(OUT)/gen_unsupported.c $(OUT)/gen_unsupported.h
	@if $(PYTHON) -m recomp $< -o $(OUT)/gen_unsupported.c 2> $(OUT)/unsupported.err; then \
	    echo 'FAIL: recompiler accepted an unsupported instruction'; exit 1; fi
	@grep -Eq '^recomp: error: [0-9a-f]{8}: unsupported instruction `mul' $(OUT)/unsupported.err || \
	    { echo 'FAIL: unexpected error message:'; cat $(OUT)/unsupported.err; exit 1; }
	@test ! -e $(OUT)/gen_unsupported.c || { echo 'FAIL: output written despite error'; exit 1; }
	@echo "PASS rejects unsupported code: $$(cat $(OUT)/unsupported.err)"

clean:
	rm -rf out
