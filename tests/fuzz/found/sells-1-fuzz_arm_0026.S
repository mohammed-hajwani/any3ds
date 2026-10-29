@ Seed 1; mode arm; M3 runs 10.
@ TRIAGE: harness-state mismatch, not an established translator bug.
@ M3 preserves Unicorn CPSR.GE between input runs but clears Ctx.ge.
@ UASX on one run leaves GE for SEL on the next run to consume.
@ This case deliberately omits the new generator's GE-reset preamble.
@ Deletion-minimal instruction units; replay with the M3 driver.
@ Original encodings before shrinking:
@ 208c1590: umullcs	r1, ip, r0, r5
@ c6374f73: shsub16gt	r4, r7, r3
@ b6ff0075: uxthlt	r0, r5
@ e3a0a302: mov	sl, #134217728	@ 0x8000000
@ e28aa080: add	sl, sl, #128	@ 0x80
@ e3a0b008: mov	fp, #8
@ 611a50db: ldrsbvs	r5, [sl, -fp]
@ e3a0a302: mov	sl, #134217728	@ 0x8000000
@ e28aa080: add	sl, sl, #128	@ 0x80
@ e3a0b008: mov	fp, #8
@ d9ba000f: ldmible	sl!, {r0, r1, r2, r3}
@ be271b40: vnmullt.f64	d1, d7, d0
@ 51d246c6: bicspl	r4, r2, r6, asr #13
@ 4294709a: addsmi	r7, r4, #154	@ 0x9a
@ a0ec2496: smlalge	r2, ip, r6, r4
@ 96812fb3: sells	r2, r1, r3
@ 06b12476: sxtaheq	r2, r1, r6, ror #8
@ e3a0a302: mov	sl, #134217728	@ 0x8000000
@ e28aa080: add	sl, sl, #128	@ 0x80
@ e3a0b008: mov	fp, #8
@ 113a50bb: ldrhne	r5, [sl, -fp]!
@ e3a0a302: mov	sl, #134217728	@ 0x8000000
@ e28aa080: add	sl, sl, #128	@ 0x80
@ e3a0b008: mov	fp, #8
@ 793a000f: ldmdbvc	sl!, {r0, r1, r2, r3}
@ 9e261b07: vmulls.f64	d1, d6, d7
@ e0232c54: eor	r2, r3, r4, asr ip
@ c1740302: cmngt	r4, r2, lsl #6
@ d0ac6290: umlalle	r6, ip, r0, r2
@ c6503f31: uasxgt	r3, r0, r1
@ 26b85014: ssatcs	r5, #25, r4
@ e3a0a302: mov	sl, #134217728	@ 0x8000000
@ e28aa080: add	sl, sl, #128	@ 0x80
@ e3a0b008: mov	fp, #8
@ a51a0008: ldrge	r0, [sl, #-8]
@ e12fff1e: bx	lr
.syntax unified
.fpu vfp
.arm
.text
.balign 4
.global fuzz_arm_0026
.type fuzz_arm_0026, %function
fuzz_arm_0026:
    sells r2, r1, r3
    uasxgt r3, r0, r1
    bx lr
.size fuzz_arm_0026, . - fuzz_arm_0026
