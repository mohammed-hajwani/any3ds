/* The M13 reference: modules A and B compiled for the host and linked
 * directly, running the same sequence as ro_test.c on the 3DS side. */
#include "sequence.h"

int a_compute(int x);
int b_add(int x, int y);
int b_count(void);
extern int b_counter, a_state;

static void reload_a(Exports *e)
{
    (void)e;
    a_state = 100;              /* reloading A resets its .data */
}

static void say(const char *line) { puts(line); }

int main(void)
{
    Exports e = { a_compute, b_add, b_count, &b_counter, &a_state };
    run_sequence(&e, reload_a, say);
    return 0;
}
