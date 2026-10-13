/* The M13 test sequence, shared by the native reference (native_main.c)
 * and the 3DS program (ro_test.c), so both print exactly the same lines if
 * the CRO loader and the recompiled modules are right. */
#include <stdio.h>

typedef struct {
    int (*a_compute)(int);
    int (*b_add)(int, int);
    int (*b_count)(void);
    int *b_counter;
    int *a_state;
} Exports;

/* reload_a: unload module A and load it again (at another address on the
   3DS); its .data goes back to its initial values. */
static void run_sequence(Exports *e, void (*reload_a)(Exports *e), void (*say)(const char *line))
{
    char line[128];
    for (int round = 0; round < 2; round++) {
        for (int x = 0; x < 6; x++) {
            snprintf(line, sizeof line, "round %d: a_compute(%d) = %d", round, x, e->a_compute(x * 13 - 20));
            say(line);
        }
        snprintf(line, sizeof line, "round %d: b_add(3, 4) = %d, b_count() = %d, b_counter = %d, a_state = %d",
                 round, e->b_add(3, 4), e->b_count(), *e->b_counter, *e->a_state);
        say(line);
        if (round == 0)
            reload_a(e);
    }
}
