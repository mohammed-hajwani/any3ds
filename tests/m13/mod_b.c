/* M13 test module B: exports functions for module A to import, and keeps
 * its own data, bss and a table of pointers (internal relocations). */
int b_counter = 5;                  /* .data */
static int b_calls;                 /* .bss */

static int b_double(int x) { return 2 * x; }
static int b_square(int x) { return x * x; }

/* A table of function pointers: each entry is an absolute address the
   loader patches in (an internal relocation into text). */
int (*const b_ops[2])(int) = { b_double, b_square };

int b_add(int x, int y)
{
    b_calls++;
    b_counter += x;
    return x + y + b_counter;
}

int b_apply(int op, int x)
{
    b_calls++;
    return b_ops[op & 1](x);
}

int b_count(void)
{
    return b_calls;
}
