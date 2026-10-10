/* M2 test functions: loads, stores, globals, literal pools, structs,
 * stack arguments, block copies and a switch jump table.
 *
 * Built twice: for ARM (then recompiled) and natively as the reference.
 * Keep it free of undefined behaviour and avoid `long`/pointers in any
 * data the harness compares byte for byte (they differ in size between
 * the 32-bit guest and the 64-bit host).
 */
#include <stddef.h>
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

typedef struct { uint8_t tag; int8_t delta; uint16_t id; int16_t temp; uint32_t value; } Rec;
typedef struct Node { struct Node *next; uint32_t val; } Node;
typedef struct { uint32_t a, b, c, d, e, f; } Big;

/* Globals: .data, .bss and .rodata. */
uint32_t g_counter = 5;
uint32_t g_table[16] = { 1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610, 987 };
uint8_t  g_buf[64];
static const char msg[] = "hello, 3ds";
Node g_nodes[4] = { { &g_nodes[1], 10 }, { &g_nodes[2], 20 }, { &g_nodes[3], 30 }, { 0, 40 } };

/* Literal pool: a 32-bit constant ARMv6K can't build in one instruction. */
NOINLINE uint32_t load_const(void) { return 0x12345678u; }

NOINLINE uint32_t bump(void) { return ++g_counter; }

NOINLINE uint32_t table_sum(uint32_t n)
{
    uint32_t s = 0;
    for (uint32_t i = 0; i < n && i < 16; i++)
        s += g_table[i];
    return s;
}

NOINLINE void fill_buf(uint32_t v, uint32_t n)
{
    for (uint32_t i = 0; i < n && i < sizeof g_buf; i++)
        g_buf[i] = (uint8_t)(v + i);
}

/* Byte loads, pointer walking. */
NOINLINE size_t my_strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
NOINLINE size_t msg_len(void) { return my_strlen(msg); }

NOINLINE int my_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

NOINLINE void my_memcpy(uint8_t *d, const uint8_t *s, uint32_t n) { while (n--) *d++ = *s++; }

/* Byte / halfword / signed fields. */
NOINLINE int32_t rec_mix(const Rec *r) { return r->tag + r->delta + r->id + r->temp + (int32_t)r->value; }

NOINLINE void rec_set(Rec *r, int32_t x)
{
    r->tag = (uint8_t)x;
    r->delta = (int8_t)(x >> 3);
    r->id = (uint16_t)(x >> 5);
    r->temp = (int16_t)(x >> 7);
    r->value = (uint32_t)x;
}

/* Pointers stored in memory. */
NOINLINE uint32_t list_sum(const Node *n) { uint32_t s = 0; while (n) { s += n->val; n = n->next; } return s; }
NOINLINE uint32_t global_list(uint32_t start) { return list_sum(&g_nodes[start & 3]); }

/* Struct copy: gcc uses ldm/stm. */
NOINLINE void big_copy(Big *d, const Big *s) { *d = *s; }

/* More than four arguments: the rest go on the stack. */
NOINLINE uint32_t many_args(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f)
{
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f;
}
NOINLINE uint32_t call_many(uint32_t x) { return many_args(x, x + 1, x + 2, x + 3, x + 4, x + 5); }

/* A local array on the stack. */
NOINLINE uint32_t local_array(uint32_t seed)
{
    uint32_t a[8];
    for (int i = 0; i < 8; i++)
        a[i] = seed * (uint32_t)i;
    uint32_t s = 0;
    for (int i = 7; i >= 0; i--)
        s ^= a[i] + (uint32_t)i;
    return s;
}

NOINLINE void bubble(int32_t *a, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = 0; j + 1 < n - i; j++)
            if (a[j] > a[j + 1]) { int32_t t = a[j]; a[j] = a[j + 1]; a[j + 1] = t; }
}

/* 64-bit values in memory. */
NOINLINE uint64_t load64(const uint64_t *p) { return p[1] + p[2]; }
NOINLINE void store64(uint64_t *p, uint64_t v) { p[0] = v; p[3] = v + 1; }

/* Dense switch: a jump table indexed through pc. */
NOINLINE uint32_t sw(uint32_t x)
{
    switch (x) {
    case 0: return 11;
    case 1: return 22;
    case 2: return 33;
    case 3: return 44;
    case 4: return 55;
    case 5: return 66;
    default: return 0;
    }
}
