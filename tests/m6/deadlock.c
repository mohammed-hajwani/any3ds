/* M6b kernel test: two threads that wait on each other forever. The HLE
 * kernel must notice that no thread can ever run again and stop with a
 * report naming each thread and what it waits on, instead of hanging. */
#include <3ds.h>

void __appInit(void) {}
void __appExit(void) {}

static Handle ev_a, ev_b;

static void other(void *arg)
{
    (void)arg;
    svcWaitSynchronization(ev_a, U64_MAX);     /* nobody signals A... */
    svcSignalEvent(ev_b);
}

int main(void)
{
    svcCreateEvent(&ev_a, RESET_ONESHOT);
    svcCreateEvent(&ev_b, RESET_ONESHOT);
    threadCreate(other, NULL, 0x4000, 0x30, -2, true);
    svcWaitSynchronization(ev_b, U64_MAX);     /* ...so nobody signals B */
    return 0;
}
