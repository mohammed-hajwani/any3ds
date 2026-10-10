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
# -fno-math-errno: sqrt becomes one instruction (and needs no libm).
HOST_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -fno-math-errno \
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
M4 := out/m4
M5 := out/m5

# devkitARM lives outside PATH unless the shell profile adds it.
ifdef DEVKITARM
export PATH := $(DEVKITARM)/bin:$(PATH)
endif

.PHONY: test test-m0 test-m1 test-m2 test-m3 test-m4 test-m5 test-real test-m6 venv clean
.SECONDARY:          # keep intermediate .o / .elf / generated .c for inspection
.DELETE_ON_ERROR:

test: test-m0 test-m1 test-m2 test-m3 test-m4 test-m5 test-real test-m6

venv: $(VENV)/.installed

$(VENV)/.installed: requirements.txt
	python3 -m venv $(VENV)
	$(PYTHON) -m pip install -q -r requirements.txt
	touch $@

$(M0) $(M1) $(M2) $(M3) $(M4) $(M5):
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
	@$(REJECT) $(M0)/unsupported.o 'unsupported instruction `qadd'

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
	@$(REJECT) $(M1)/bad_bl.elf 'not inside any known function or code region'

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
	@sh tests/m3/selftest.sh $(PYTHON) $(HOST_CC) ldrsh out/m2/addr.elf 'ldrsh r1' r1 $(HOST_FLAGS)

# ---------------------------------------------------------------- M4
#
# Thumb (ARMv6K = Thumb-1) and ARM/Thumb interworking. The M1/M2 programs
# are rebuilt as Thumb and reuse their native-diff harnesses; every M4
# program also runs in lockstep against Unicorn.

THUMB_FLAGS := -mthumb -mcpu=mpcore -O1 -mthumb-interwork

$(M4)/funcs_thumb.elf: tests/m1/funcs.c | $(M4)
	$(ARM_CC) $(THUMB_FLAGS) $(ARM_LDFLAGS) $< -o $@

$(M4)/mem_thumb.elf: tests/m2/mem.c | $(M4)
	$(ARM_CC) $(THUMB_FLAGS) $(ARM_LDFLAGS) $< -o $@

$(M4)/vshift_arm.elf: tests/m4/vshift.c | $(M4)
	$(ARM_CC) $(ARM_FLAGS) $(ARM_LDFLAGS) $< -o $@

$(M4)/vshift_thumb.elf: tests/m4/vshift.c | $(M4)
	$(ARM_CC) $(THUMB_FLAGS) $(ARM_LDFLAGS) $< -o $@

# One ARM file + one Thumb file, linked together. --use-blx makes the
# linker switch modes with blx (ARMv5T+) instead of inserting "veneers"
# (tiny stubs that mix both modes: `bx pc` + an ARM `b`), which the
# recompiler doesn't recognise yet.
$(M4)/interwork.elf: tests/m4/interwork_arm.c tests/m4/interwork_thumb.c tests/m4/interwork.h | $(M4)
	$(ARM_CC) $(ARM_FLAGS) -mthumb-interwork -c tests/m4/interwork_arm.c -o $(M4)/interwork_arm.o
	$(ARM_CC) $(THUMB_FLAGS) -c tests/m4/interwork_thumb.c -o $(M4)/interwork_thumb.o
	$(ARM_CC) $(ARM_FLAGS) $(ARM_LDFLAGS) -Wl,--use-blx $(M4)/interwork_arm.o $(M4)/interwork_thumb.o -o $@

$(M4)/%.elf: tests/m4/%.S | $(M4)
	$(ARM_CC) $(ARM_FLAGS) $(ARM_LDFLAGS) $< -o $@

$(M4)/%.dis: $(M4)/%.elf
	$(OBJDUMP) -d $< > $@

$(M4)/gen_%.c: $(M4)/%.elf $(RECOMP_DEP)
	$(PYTHON) -m recomp $< -o $@

$(M4)/funcs_thumb_harness: tests/m1/harness.c tests/m1/funcs.c $(M4)/gen_funcs_thumb.c $(RUNTIME_DEP)
	$(HOST_CC) $(HOST_FLAGS) $< $(M4)/gen_funcs_thumb.c $(RUNTIME_SRC) -o $@

$(M4)/mem_thumb_harness: tests/m2/harness.c tests/m2/mem.c $(M4)/gen_mem_thumb.c $(RUNTIME_DEP)
	$(HOST_CC) $(HOST_FLAGS) $< $(M4)/gen_mem_thumb.c $(RUNTIME_SRC) -o $@

$(M4)/vshift_%_harness: tests/m4/vshift_harness.c tests/m4/vshift.c $(M4)/gen_vshift_%.c $(RUNTIME_DEP)
	$(HOST_CC) $(HOST_FLAGS) $< $(M4)/gen_vshift_$*.c $(RUNTIME_SRC) -o $@

$(M4)/interwork_harness: tests/m4/interwork_harness.c tests/m4/interwork_arm.c tests/m4/interwork_thumb.c $(M4)/gen_interwork.c $(RUNTIME_DEP)
	$(HOST_CC) $(HOST_FLAGS) $< $(M4)/gen_interwork.c $(RUNTIME_SRC) -o $@

M4_HARNESSES := $(M4)/funcs_thumb_harness $(M4)/mem_thumb_harness $(M4)/vshift_arm_harness \
                $(M4)/vshift_thumb_harness $(M4)/interwork_harness
M4_PROGS := m4/funcs_thumb m4/mem_thumb m4/vshift_arm m4/vshift_thumb m4/interwork m4/thumb m4/arm_regshift
$(foreach p,$(M4_PROGS),$(eval $(call m3_prog,$(p))))
M4_DIFFS := $(foreach p,$(M4_PROGS),$(M3)/$(notdir $(p))_diff)
M4_BAD := $(M4)/bad_thumb2.elf $(M4)/bad_mode.elf

test-m4: $(M4_HARNESSES) $(M4_DIFFS) $(M4_BAD) $(M4)/mem_thumb.dis $(M4)/thumb.dis $(M4)/interwork.dis $(RECOMP_DEP)
	@for h in $(M4_HARNESSES); do \
	    echo "=== M4: $$(basename $$h _harness) vs native ==="; \
	    timeout 60 ./$$h || exit 1; \
	done
	@for d in $(M4_DIFFS); do \
	    echo "=== M4: $$(basename $$d _diff) vs unicorn ==="; \
	    timeout 120 ./$$d || exit 1; \
	done
	@$(REJECT) $(M4)/bad_thumb2.elf 'Thumb-2 instruction'
	@$(REJECT) $(M4)/bad_mode.elf 'ARM code inside a Thumb function'
	@echo '=== M4: self-test (planted Thumb pc bug must be pinpointed) ==='
	@sh tests/m3/selftest.sh $(PYTHON) $(HOST_CC) thumb-pc $(M4)/thumb.elf 'ldr r1, \[pc' r1 $(HOST_FLAGS)

# ---------------------------------------------------------------- M5
#
# VFPv2 floating point (ARM mode only: Thumb-1 has no VFP instructions),
# hard-float ABI as used for 3DS homebrew.

VFP_FLAGS := -marm -mcpu=mpcore -mfloat-abi=hard -mfpu=vfp -O1 -fno-math-errno

$(M5)/fp.elf: tests/m5/fp.c | $(M5)
	$(ARM_CC) $(VFP_FLAGS) $(ARM_LDFLAGS) $< -o $@

$(M5)/%.elf: tests/m5/%.S | $(M5)
	$(ARM_CC) $(VFP_FLAGS) $(ARM_LDFLAGS) $< -o $@

$(M5)/%.dis: $(M5)/%.elf
	$(OBJDUMP) -d $< > $@

$(M5)/gen_%.c: $(M5)/%.elf $(RECOMP_DEP)
	$(PYTHON) -m recomp $< -o $@

$(M5)/fp_harness: tests/m5/harness.c tests/m5/fp.c $(M5)/gen_fp.c $(RUNTIME_DEP)
	$(HOST_CC) $(HOST_FLAGS) $< $(M5)/gen_fp.c $(RUNTIME_SRC) -o $@

M5_PROGS := m5/fp m5/vfp
$(foreach p,$(M5_PROGS),$(eval $(call m3_prog,$(p))))
M5_DIFFS := $(foreach p,$(M5_PROGS),$(M3)/$(notdir $(p))_diff)

test-m5: $(M5)/fp_harness $(M5_DIFFS) $(M5)/bad_vcvtr.elf $(M5)/fp.dis $(M5)/vfp.dis $(RECOMP_DEP)
	@echo '=== M5: fp vs native ==='
	@timeout 60 ./$(M5)/fp_harness
	@for d in $(M5_DIFFS); do \
	    echo "=== M5: $$(basename $$d _diff) vs unicorn ==="; \
	    timeout 120 ./$$d || exit 1; \
	done
	@$(REJECT) $(M5)/bad_vcvtr.elf 'FPSCR rounding mode'
	@echo '=== M5: self-test (planted x86-NaN bug must be pinpointed) ==='
	@sh tests/m3/selftest.sh $(PYTHON) $(HOST_CC) vfp-sqrt $(M5)/vfp.elf 'vsqrt.f32' d4 $(HOST_FLAGS)

# ---------------------------------------------------------------- real code
#
# What -O2 code and libgcc look like: function pointers (indirect calls),
# vtables, tail calls, calls into the middle of functions, 64-bit maths,
# clz/rev/extends/ldrd. ARM and Thumb builds, both linked with libgcc.

REAL := out/real
REAL_LDFLAGS := $(ARM_LDFLAGS) -Wl,--use-blx

$(REAL):
	mkdir -p $@

$(REAL)/real_arm.elf: tests/real/real.c | $(REAL)
	$(ARM_CC) $(VFP_FLAGS) -O2 $(REAL_LDFLAGS) $< -lgcc -o $@

$(REAL)/real_thumb.elf: tests/real/real.c | $(REAL)
	$(ARM_CC) $(THUMB_FLAGS) -O2 $(REAL_LDFLAGS) $< -lgcc -o $@

$(REAL)/%.dis: $(REAL)/%.elf
	$(OBJDUMP) -d $< > $@

$(REAL)/gen_%.c: $(REAL)/%.elf $(RECOMP_DEP)
	$(PYTHON) -m recomp $< -o $@

$(REAL)/%_harness: tests/real/harness.c tests/real/real.c $(REAL)/gen_%.c $(RUNTIME_DEP)
	$(HOST_CC) $(HOST_FLAGS) $< $(REAL)/gen_$*.c $(RUNTIME_SRC) -o $@

REAL_PROGS := real/real_arm real/real_thumb
$(foreach p,$(REAL_PROGS),$(eval $(call m3_prog,$(p))))
REAL_DIFFS := $(foreach p,$(REAL_PROGS),$(M3)/$(notdir $(p))_diff)
REAL_SKIP := isort,shape_total

test-real: $(REAL)/real_arm_harness $(REAL)/real_thumb_harness $(REAL_DIFFS) $(REAL)/real_arm.dis $(REAL)/real_thumb.dis
	@for h in $(REAL)/real_arm_harness $(REAL)/real_thumb_harness; do \
	    echo "=== real: $$(basename $$h _harness) vs native ==="; \
	    timeout 60 ./$$h || exit 1; \
	done
	@for d in $(REAL_DIFFS); do \
	    echo "=== real: $$(basename $$d _diff) vs unicorn ==="; \
	    timeout 180 ./$$d 100 $(REAL_SKIP) || exit 1; \
	done

# ---------------------------------------------------------------- M6
#
# The canonical libctru "hello world", built against the real libctru +
# newlib exactly as 3DS homebrew is, then recompiled in full. Running it
# needs the OS layer (svc + IPC services), which comes next; for now the
# check is that every function recompiles, the C compiles, and newlib's
# hand-optimised string/memory routines match Unicorn.

DEVKITPRO ?= /opt/devkitpro
CTRU := $(DEVKITPRO)/libctru
CTRU_FLAGS := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -O2 -mword-relocations \
              -ffunction-sections -I$(CTRU)/include
M6 := out/m6
M6_DIFF_ONLY := +strcmp,memcpy,memset,memmove,strlen,strchr

$(M6):
	mkdir -p $@

$(M6)/hello.elf: tests/m6/hello.c | $(M6)
	$(ARM_CC) $(CTRU_FLAGS) -specs=3dsx.specs -L$(CTRU)/lib $< -lctru -lm -o $@

$(M6)/hello.dis: $(M6)/hello.elf
	$(OBJDUMP) -d $< > $@

$(M6)/gen_hello.c: $(M6)/hello.elf $(RECOMP_DEP)
	$(PYTHON) -m recomp $< -o $@ > $(M6)/recomp.log

$(M6)/gen_hello.o: $(M6)/gen_hello.c $(RUNTIME_DEP)
	$(HOST_CC) $(HOST_FLAGS) -c $< -o $@

$(eval $(call m3_prog,m6/hello))

test-m6: $(M6)/gen_hello.o $(M3)/hello_diff $(M6)/hello.dis
	@echo "=== M6: libctru hello world: $$(grep -c '^  f_' $(M6)/recomp.log) functions recompiled," \
	      "$$(wc -l < $(M6)/gen_hello.c) lines of C, compiled cleanly ==="
	@echo '=== M6: newlib string/memory routines vs unicorn ==='
	@timeout 300 ./$(M3)/hello_diff 100 $(M6_DIFF_ONLY)

# M6b, first step: actually start the recompiled hello world and log its
# system calls until the first one that needs real OS services. Not part of
# `make test`: today it stops on purpose at svcConnectToPort("srv:").
.PHONY: run-m6
run-m6: tests/m6/run_hello.c $(M6)/gen_hello.c $(RUNTIME_DEP)
	$(HOST_CC) $(HOST_FLAGS) $< $(M6)/gen_hello.c $(RUNTIME_SRC) -o $(M6)/run_hello
	-./$(M6)/run_hello

clean:
	rm -rf out
