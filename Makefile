# Run inside WSL:  make test
#
# Chain per test program:
#   tests/mN/X.{c,S} --arm-none-eabi-gcc-->  out/mN/X.o (M0) or X.elf (M1+, linked)
#                    --python -m recomp-->   out/mN/gen_X.c
#                    --clang + harness-->    out/mN/X_harness  (run it)

ARM_CC      := arm-none-eabi-gcc
OBJDUMP     := arm-none-eabi-objdump
ARM_FLAGS   := -marm -mcpu=mpcore -O1
# Link at the 3DS homebrew load address, with no C library or startup code.
ARM_LDFLAGS := -nostdlib -Wl,-Ttext=0x00100000 -Wl,--entry=0x00100000

HOST_CC    := clang
HOST_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror \
              -fsanitize=undefined,address -fno-sanitize-recover=all \
              -Iruntime

VENV   := .venv
PYTHON := $(VENV)/bin/python

RECOMP_SRC  := $(wildcard recomp/*.py)
RUNTIME_SRC := runtime/runtime.c
RUNTIME_DEP := $(RUNTIME_SRC) $(wildcard runtime/*.h)
RECOMP_DEP  := $(RECOMP_SRC) $(VENV)/.installed
REJECT      := sh tests/expect_reject.sh $(PYTHON)

M0 := out/m0
M1 := out/m1

# devkitARM lives outside PATH unless the shell profile adds it.
ifdef DEVKITARM
export PATH := $(DEVKITARM)/bin:$(PATH)
endif

.PHONY: test test-m0 test-m1 venv clean
.SECONDARY:          # keep intermediate .o / .elf / generated .c for inspection
.DELETE_ON_ERROR:

test: test-m0 test-m1

venv: $(VENV)/.installed

$(VENV)/.installed: requirements.txt
	python3 -m venv $(VENV)
	$(PYTHON) -m pip install -q -r requirements.txt
	touch $@

$(M0) $(M1):
	mkdir -p $@

# ---------------------------------------------------------------- M0

$(M0)/%.o: tests/m0/%.c | $(M0)
	$(ARM_CC) $(ARM_FLAGS) -c $< -o $@

$(M0)/%.o: tests/m0/%.S | $(M0)
	$(ARM_CC) $(ARM_FLAGS) -c $< -o $@

$(M0)/%.dis: $(M0)/%.o
	$(OBJDUMP) -d $< > $@

$(M0)/gen_%.c: $(M0)/%.o $(RECOMP_DEP)
	$(PYTHON) -m recomp $< -o $@

$(M0)/test_harness: tests/m0/harness.c $(M0)/gen_test.c $(RUNTIME_DEP)
	$(HOST_CC) $(HOST_FLAGS) $< $(M0)/gen_test.c $(RUNTIME_SRC) -o $@

$(M0)/shifts_harness: tests/m0/shifts_harness.c $(M0)/gen_shifts.c $(RUNTIME_DEP)
	$(HOST_CC) $(HOST_FLAGS) $< $(M0)/gen_shifts.c $(RUNTIME_SRC) -o $@

test-m0: $(M0)/test.dis $(M0)/shifts.dis $(M0)/test_harness $(M0)/shifts_harness $(M0)/unsupported.o $(RECOMP_DEP)
	@echo '=== M0: test(a, b) = (a + b) * 3 ==='
	@timeout 60 ./$(M0)/test_harness
	@echo '=== M0: barrel shifter ==='
	@timeout 60 ./$(M0)/shifts_harness
	@$(REJECT) $(M0)/unsupported.o 'unsupported instruction `mul'

# ---------------------------------------------------------------- M1

$(M1)/%.elf: tests/m1/%.c | $(M1)
	$(ARM_CC) $(ARM_FLAGS) $(ARM_LDFLAGS) $< -o $@

$(M1)/%.elf: tests/m1/%.S | $(M1)
	$(ARM_CC) $(ARM_FLAGS) $(ARM_LDFLAGS) $< -o $@

$(M1)/%.dis: $(M1)/%.elf
	$(OBJDUMP) -d $< > $@

$(M1)/gen_%.c: $(M1)/%.elf $(RECOMP_DEP)
	$(PYTHON) -m recomp $< -o $@

# harness.c #includes funcs.c as the host-native reference.
$(M1)/funcs_harness: tests/m1/harness.c tests/m1/funcs.c $(M1)/gen_funcs.c $(RUNTIME_DEP)
	$(HOST_CC) $(HOST_FLAGS) $< $(M1)/gen_funcs.c $(RUNTIME_SRC) -o $@

$(M1)/flags_harness: tests/m1/flags_harness.c $(M1)/gen_flags.c $(RUNTIME_DEP)
	$(HOST_CC) $(HOST_FLAGS) $< $(M1)/gen_flags.c $(RUNTIME_SRC) -o $@

test-m1: $(M1)/funcs.dis $(M1)/flags.dis $(M1)/funcs_harness $(M1)/flags_harness $(M1)/bad_bl.elf $(M0)/unsupported.o $(RECOMP_DEP)
	@echo '=== M1: differential test vs native ==='
	@timeout 60 ./$(M1)/funcs_harness
	@echo '=== M1: flags and condition codes ==='
	@timeout 60 ./$(M1)/flags_harness
	@$(REJECT) $(M1)/bad_bl.elf 'is not the start of a known function'

clean:
	rm -rf out
