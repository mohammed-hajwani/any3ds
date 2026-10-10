/* Functions split across an ARM-mode file and a Thumb-mode file, calling
 * each other in both directions (the linker turns bl into blx where the
 * mode changes). */
#ifndef INTERWORK_H
#define INTERWORK_H

#include <stdint.h>

#define NOINLINE __attribute__((noinline))

/* interwork_arm.c (ARM) */
uint32_t a_double(uint32_t x);
uint32_t a_calls_t(uint32_t x);
uint32_t a_nested(uint32_t x);
uint32_t a_deep(uint32_t x, uint32_t n);

/* interwork_thumb.c (Thumb) */
uint32_t t_square(uint32_t x);
uint32_t t_calls_a(uint32_t x);
uint32_t t_deep(uint32_t x, uint32_t n);
uint32_t t_sum_both(uint32_t x, uint32_t y);

#endif
