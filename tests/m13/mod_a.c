/* M13 test module A: imports from module B through words the loader fills
 * in (import relocations), and uses its own strings, data, bss, a switch
 * and a callback table (internal relocations). */
extern int b_add(int x, int y);
extern int b_apply(int op, int x);
extern int b_counter;

/* Imports are reached through these words: R_ARM_ABS32 against symbols
   module B defines, which mkcro.py turns into import patches. */
int (*const volatile imp_add)(int, int) = b_add;
int (*const volatile imp_apply)(int, int) = b_apply;
int *const volatile imp_counter = &b_counter;

static const char message[] = "module A";       /* .rodata */
int a_state = 100;                              /* .data */
static int a_scratch[8];                        /* .bss */

static int step(int kind, int x)
{
    switch (kind) {                             /* a jump table */
    case 0: return x + 1;
    case 1: return x * 3;
    case 2: return x - 7;
    case 3: return x ^ 0x55;
    case 4: return x << 2;
    case 5: return -x;
    default: return x;
    }
}

int a_compute(int x)
{
    int r = 0;
    for (int i = 0; i < 8; i++) {
        a_scratch[i] = step(i % 7, x + i);
        r += a_scratch[i];
    }
    r += imp_add(x, a_state);
    r += imp_apply(1, x) + imp_apply(0, r & 0xff);
    a_state += 1;
    return r + *imp_counter + message[x & 7];
}
