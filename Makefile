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
M2 := out/m2
M3 := out/m3

# devkitARM lives outside PATH unless the shell profile adds it.
ifdef DEVKITARM
export PATH := $(DEVKITARM)/bin:$(PATH)
endif

.PHONY: test test-m0 test-m1 test-m2 test-m3 venv clean
.SECONDARY:          # keep intermediate .o / .elf / generated .c for inspection
.DELETE_ON_ERROR:

test: test-m0 test-m1 test-m2 test-m3

venv: $(VENV)/.installed

$(VENV)/.installed: requirements.txt
	python3 -m venv $(VENV)
	$(PYTHON) -m pip install -q -r requirements.txt
	touch $@

$(M0) $(M1) $(M2) $(M3):
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

# ---------------------------------------------------------------- M2

$(M2)/%.elf: tests/m2/%.c | $(M2)
	$(ARM_CC) $(ARM_FLAGS) $(ARM_LDFLAGS) $< -o $@

$(M2)/%.elf: tests/m2/%.S | $(M2)
	$(ARM_CC) $(ARM_FLAGS) $(ARM_LDFLAGS) $< -o $@

$(M2)/%.dis: $(M2)/%.elf
	$(OBJDUMP) -d $< > $@

$(M2)/gen_%.c: $(M2)/%.elf $(RECOMP_DEP)
	$(PYTHON) -m recomp $< -o $@

# harness.c #includes mem.c as the host-native reference.
$(M2)/mem_harness: tests/m2/harness.c tests/m2/mem.c $(M2)/gen_mem.c $(RUNTIME_DEP)
	$(HOST_CC) $(HOST_FLAGS) $< $(M2)/gen_mem.c $(RUNTIME_SRC) -o $@

$(M2)/addr_harness: tests/m2/addr_harness.c $(M2)/gen_addr.c $(RUNTIME_DEP)
	$(HOST_CC) $(HOST_FLAGS) $< $(M2)/gen_addr.c $(RUNTIME_SRC) -o $@

M2_BAD := $(M2)/bad_ldr_pc.elf $(M2)/bad_ldm_wb.elf $(M2)/bad_pool.elf

test-m2: $(M2)/mem.dis $(M2)/addr.dis $(M2)/mem_harness $(M2)/addr_harness $(M2_BAD) $(RECOMP_DEP)
	@echo '=== M2: differential test vs native ==='
	@timeout 60 ./$(M2)/mem_harness
	@echo '=== M2: addressing modes and block transfers ==='
	@timeout 60 ./$(M2)/addr_harness
	@$(REJECT) $(M2)/bad_ldr_pc.elf 'load into pc'
	@$(REJECT) $(M2)/bad_ldm_wb.elf 'base register in the list is unpredictable'
	@$(REJECT) $(M2)/bad_pool.elf 'runs into data'

# ---------------------------------------------------------------- M3
#
# Every linked test program again, generated with --trace and run in
# lockstep against Unicorn (a reference ARM CPU) by tests/m3/diff.c.

M3_PROGS := m1/funcs m1/flags m2/mem m2/addr

define m3_prog
$(M3)/gen_$(notdir $(1)).c: out/$(1).elf $(RECOMP_DEP) | $(M3)
	$$(PYTHON) -m recomp --trace $$< -o $$@

$(M3)/$(notdir $(1))_diff: tests/m3/diff.c $(M3)/gen_$(notdir $(1)).c $$(RUNTIME_DEP)
	$$(HOST_CC) $$(HOST_FLAGS) -DRECOMP_TRACING=1 $$< $(M3)/gen_$(notdir $(1)).c $$(RUNTIME_SRC) -lunicorn -o $$@
endef
$(foreach p,$(M3_PROGS),$(eval $(call m3_prog,$(p))))

M3_DIFFS := $(foreach p,$(M3_PROGS),$(M3)/$(notdir $(p))_diff)

test-m3: $(M3_DIFFS) out/m2/addr.elf $(RECOMP_DEP)
	@for d in $(M3_DIFFS); do \
	    echo "=== M3: $$(basename $$d _diff) vs unicorn ==="; \
	    timeout 120 ./$$d || exit 1; \
	done
	@echo '=== M3: self-test (planted bug must be pinpointed) ==='
	@sh tests/m3/selftest.sh $(PYTHON) $(HOST_CC) $(HOST_FLAGS)

clean:
	rm -rf out
