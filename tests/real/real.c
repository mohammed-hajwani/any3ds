/* "Real code" readiness: what -O2 code and libgcc actually look like.
 *
 * Built at -O2 as ARM and as Thumb, linked with libgcc (division, 64-bit
 * helpers). Exercises function pointers (indirect calls), vtables, tail
 * calls, 64-bit multiply/divide, clz, byte swaps, 16-bit extends, ldrd.
 *
 * The test_* entry points take and return plain integers, so the native
 * harness can call them like M1's; they drive the pointer-heavy code
 * internally on static buffers.
 */
#include <stddef.h>
#include <stdint.h>

/* ---- generic insertion sort with a comparator (indirect calls) ---- */
typedef int (*cmp_fn)(const void *, const void *);

static void swap_bytes(uint8_t *a, uint8_t *b, size_t n)
{
    while (n--) { uint8_t t = *a; *a++ = *b; *b++ = t; }
}

void isort(void *base, size_t n, size_t sz, cmp_fn cmp)
{
    uint8_t *b = base;
    for (size_t i = 1; i < n; i++)
        for (size_t j = i; j > 0 && cmp(b + (j - 1) * sz, b + j * sz) > 0; j--)
            swap_bytes(b + (j - 1) * sz, b + j * sz, sz);
}

int cmp_i32(const void *a, const void *b)
{
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

int cmp_u16_desc(const void *a, const void *b) { return (int)*(const uint16_t *)b - (int)*(const uint16_t *)a; }

/* ---- a tiny vtable ---- */
typedef struct Shape Shape;
typedef struct { int32_t (*area)(const Shape *); int32_t (*perim)(const Shape *); } ShapeOps;
struct Shape { const ShapeOps *ops; int32_t a, b; };

static int32_t rect_area(const Shape *s)  { return s->a * s->b; }
static int32_t rect_perim(const Shape *s) { return 2 * (s->a + s->b); }
static int32_t tri_area(const Shape *s)   { return s->a * s->b / 2; }
static int32_t tri_perim(const Shape *s)  { return s->a + s->b + (s->a > s->b ? s->a : s->b); }
static const ShapeOps rect_ops = { rect_area, rect_perim };
static const ShapeOps tri_ops = { tri_area, tri_perim };

int32_t shape_total(const Shape *s, int n)
{
    int32_t t = 0;
    for (int i = 0; i < n; i++)
        t += s[i].ops->area(&s[i]) + s[i].ops->perim(&s[i]);
    return t;
}

/* ---- arithmetic that needs the newer instructions / libgcc ---- */
uint64_t mul64(uint64_t a, uint64_t b) { return a * b; }
uint64_t umul32(uint32_t a, uint32_t b) { return (uint64_t)a * b; }
int64_t  smul32(int32_t a, int32_t b) { return (int64_t)a * b; }
uint64_t umac(uint64_t acc, uint32_t a, uint32_t b) { return acc + (uint64_t)a * b; }
uint32_t udiv(uint32_t a, uint32_t b) { return b ? a / b : 0; }
int32_t  sdiv(int32_t a, int32_t b) { return b && !(a == INT32_MIN && b == -1) ? a / b : 0; }
uint32_t umod(uint32_t a, uint32_t b) { return b ? a % b : 0; }
uint64_t udiv64(uint64_t a, uint64_t b) { return b ? a / b : 0; }
uint32_t clz32(uint32_t x) { return x ? (uint32_t)__builtin_clz(x) : 32; }
uint32_t bswap(uint32_t x) { return __builtin_bswap32(x); }
uint32_t bswap16(uint32_t x) { return __builtin_bswap16((uint16_t)x); }
int32_t  sext16(int32_t x) { return (int16_t)x; }
int32_t  sext8(int32_t x) { return (int8_t)x; }
uint32_t zext16(uint32_t x) { return (uint16_t)x; }

/* ---- recursion and a tail call ---- */
uint32_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
__attribute__((noinline)) uint32_t tail_helper(uint32_t x) { return x ^ (x >> 7); }
uint32_t tail_caller(uint32_t x) { return tail_helper(x * 3 + 1); }

/* ---- a function-pointer dispatch table ---- */
typedef uint32_t (*op_fn)(uint32_t);
static uint32_t op_a(uint32_t x) { return x + 1; }
static uint32_t op_b(uint32_t x) { return x * 5; }
static uint32_t op_c(uint32_t x) { return x >> 2; }
static const op_fn ops[] = { op_a, op_b, op_c };

uint32_t run_ops(uint32_t x, const uint8_t *prog, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        x = ops[prog[i] % 3](x);
    return x;
}

/* ---- number formatting: division by constants and variables ---- */
char *utoa(uint32_t v, char *buf, unsigned base)
{
    char tmp[33];
    int i = 0;
    do {
        uint32_t d = v % base;
        tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= base;
    } while (v);
    while (i)
        *buf++ = tmp[--i];
    *buf = 0;
    return buf;
}

int fmt(char *out, const char *f, const uint32_t *args)
{
    char *o = out;
    for (; *f; f++) {
        if (*f != '%') { *o++ = *f; continue; }
        switch (*++f) {
        case 'd': {
            uint32_t v = *args++;
            if ((int32_t)v < 0) { *o++ = '-'; v = 0u - v; }
            o = utoa(v, o, 10);
            break;
        }
        case 'u': o = utoa(*args++, o, 10); break;
        case 'x': o = utoa(*args++, o, 16); break;
        case 'o': o = utoa(*args++, o, 8); break;
        case 'c': *o++ = (char)*args++; break;
        case '%': *o++ = '%'; break;
        default: *o++ = '?'; break;
        }
    }
    *o = 0;
    return (int)(o - out);
}

/* ---- scalar entry points for the native harness ---- */
static uint32_t hash_bytes(const void *p, size_t n)
{
    const uint8_t *b = p;
    uint32_t h = 2166136261u;
    while (n--) h = (h ^ *b++) * 16777619u;
    return h;
}

static int32_t i32buf[24];
static uint16_t u16buf[24];

uint32_t test_sort(uint32_t seed)
{
    uint32_t x = seed | 1;
    for (int i = 0; i < 24; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        i32buf[i] = (int32_t)x;
        u16buf[i] = (uint16_t)(x >> 7);
    }
    isort(i32buf, 1 + seed % 24, sizeof i32buf[0], cmp_i32);
    isort(u16buf, 24 - seed % 24, sizeof u16buf[0], cmp_u16_desc);
    return hash_bytes(i32buf, sizeof i32buf) ^ hash_bytes(u16buf, sizeof u16buf);
}

int32_t test_shapes(uint32_t seed)
{
    Shape s[6];
    for (int i = 0; i < 6; i++) {
        s[i].ops = (seed >> i) & 1 ? &tri_ops : &rect_ops;
        s[i].a = (int32_t)((seed >> (2 * i)) & 0xFF) - 20;
        s[i].b = (int32_t)((seed >> (3 * i + 1)) & 0x7F);
    }
    return shape_total(s, 1 + (int)(seed % 6));
}

uint32_t test_ops(uint32_t seed, uint32_t n)
{
    uint8_t prog[16];
    for (int i = 0; i < 16; i++)
        prog[i] = (uint8_t)(seed >> (i % 29));
    return run_ops(seed, prog, n % 17);
}

static char fmtbuf[160];

uint32_t test_fmt(uint32_t a, uint32_t b)
{
    uint32_t args[6] = { a, b, a ^ b, a >> 3, 'A' + b % 26, b };
    int n = fmt(fmtbuf, "a=%d b=%u x=%x o=%o c=%c %% %q|%d", args);
    return hash_bytes(fmtbuf, (size_t)n) + (uint32_t)n;
}
