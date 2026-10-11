/* M6b kernel test: guest threads and synchronisation, built against real
 * libctru. It needs no services (empty __appInit/__appExit), only the
 * kernel: threads, LightLock/LightEvent (ldrex/strex + ArbitrateAddress),
 * kernel events, mutexes, semaphores, sleeps, timeouts and priorities.
 *
 * Output goes to stderr, which consoleDebugInit(debugDevice_SVC) routes
 * through svcOutputDebugString. Every line's order is fixed by
 * synchronisation (not by luck), so threads.expected would also hold on
 * real hardware. */
#include <3ds.h>
#include <stdio.h>

#define say(...) fprintf(stderr, __VA_ARGS__)
#define MAIN_PRIO 0x30
#define STACK 0x4000

void __appInit(void) {}
void __appExit(void) {}

static LightLock lock;
static char order[32];
static int order_len;

static void note(char c)
{
    LightLock_Lock(&lock);
    order[order_len++] = c;
    order[order_len] = 0;
    LightLock_Unlock(&lock);
}

static void reset_order(void) { order_len = 0; order[0] = 0; }

static Thread spawn(ThreadFunc fn, void *arg, int prio)
{
    Thread t = threadCreate(fn, arg, STACK, prio, -2, false);
    if (!t)
        say("threadCreate failed\n");
    return t;
}

static void join(Thread t)
{
    threadJoin(t, U64_MAX);
    threadFree(t);
}

/* 1. LightLock under contention: each adder yields while holding the lock. */
static int counter;
static void adder(void *arg)
{
    (void)arg;
    for (int i = 0; i < 1000; i++) {
        LightLock_Lock(&lock);
        int v = counter;
        if ((i & 63) == 0)
            svcSleepThread(0);          /* yield with the lock held */
        counter = v + 1;
        LightLock_Unlock(&lock);
    }
}

/* 2. LightEvent wakeup. */
static LightEvent light_ev;
static void light_waiter(void *arg)
{
    (void)arg;
    LightEvent_Wait(&light_ev);
    note('w');
}

/* 3. Semaphore producer/consumer. */
static Handle sem;
static int items[5], got[5];
static void consumer(void *arg)
{
    (void)arg;
    for (int i = 0; i < 5; i++) {
        svcWaitSynchronization(sem, U64_MAX);
        got[i] = items[i] * 10;
    }
}

/* 4. Sleep ordering: the shortest sleep wakes first. */
static void sleeper(void *arg)
{
    int ms = (int)arg;
    svcSleepThread(ms * 1000000LL);
    note('0' + ms);
}

/* 6. Priorities: a higher-priority thread runs as soon as it is woken. */
static Handle prio_ev;
static void prio_waiter(void *arg)
{
    svcWaitSynchronization(prio_ev, U64_MAX);
    note((char)(int)arg);
}

/* 8. Kernel mutex handed over on release. */
static Handle mutex;
static void mutex_taker(void *arg)
{
    (void)arg;
    svcWaitSynchronization(mutex, U64_MAX);
    note('T');
    svcReleaseMutex(mutex);
}

int main(void)
{
    consoleDebugInit(debugDevice_SVC);
    LightLock_Init(&lock);
    say("threads test\n");

    /* 1 */
    Thread a = spawn(adder, NULL, MAIN_PRIO), b = spawn(adder, NULL, MAIN_PRIO);
    join(a);
    join(b);
    say("1 lightlock: counter = %d\n", counter);

    /* 2 */
    reset_order();
    LightEvent_Init(&light_ev, RESET_ONESHOT);
    Thread w = spawn(light_waiter, NULL, MAIN_PRIO);
    svcSleepThread(1000000);
    note('m');                          /* before the signal, so always first */
    LightEvent_Signal(&light_ev);
    join(w);
    say("2 lightevent: %s\n", order);

    /* 3 */
    svcCreateSemaphore(&sem, 0, 5);
    Thread c = spawn(consumer, NULL, MAIN_PRIO);
    for (int i = 0; i < 5; i++) {
        s32 prev;
        items[i] = i + 1;
        svcReleaseSemaphore(&prev, sem, 1);
        svcSleepThread(100000);
    }
    join(c);
    say("3 semaphore: %d %d %d %d %d\n", got[0], got[1], got[2], got[3], got[4]);
    svcCloseHandle(sem);

    /* 4 */
    reset_order();
    Thread s3 = spawn(sleeper, (void *)3, MAIN_PRIO);
    Thread s1 = spawn(sleeper, (void *)1, MAIN_PRIO);
    Thread s2 = spawn(sleeper, (void *)2, MAIN_PRIO);
    join(s3);
    join(s1);
    join(s2);
    say("4 sleeps: %s\n", order);

    /* 5 */
    Handle ev;
    svcCreateEvent(&ev, RESET_ONESHOT);
    u64 t0 = svcGetSystemTick();
    Result r = svcWaitSynchronization(ev, 2000000);     /* 2 ms */
    u64 elapsed = svcGetSystemTick() - t0;
    say("5 timeout: result %08lx, waited >= 2ms: %s\n", (unsigned long)r,
        elapsed >= SYSCLOCK_ARM11 / 500 ? "yes" : "no");
    svcCloseHandle(ev);

    /* 6 */
    reset_order();
    svcCreateEvent(&prio_ev, RESET_STICKY);
    Thread hi = spawn(prio_waiter, (void *)'H', 0x20);
    Thread lo = spawn(prio_waiter, (void *)'L', 0x38);
    svcSleepThread(1000000);            /* both are now waiting */
    svcSignalEvent(prio_ev);            /* H preempts us right here */
    note('M');
    join(hi);
    join(lo);                           /* L only runs once we block */
    say("6 priorities: %s\n", order);
    svcCloseHandle(prio_ev);

    /* 7 */
    Handle evs[2];
    s32 index = -1;
    svcCreateEvent(&evs[0], RESET_ONESHOT);
    svcCreateEvent(&evs[1], RESET_ONESHOT);
    svcSignalEvent(evs[1]);
    r = svcWaitSynchronizationN(&index, evs, 2, false, U64_MAX);
    say("7 wait any: result %08lx, index %ld\n", (unsigned long)r, (long)index);
    svcSignalEvent(evs[0]);
    r = svcWaitSynchronizationN(&index, evs, 2, true, 1000000);
    say("7 wait all, one signalled: result %08lx\n", (unsigned long)r);
    svcSignalEvent(evs[1]);
    r = svcWaitSynchronizationN(&index, evs, 2, true, 1000000);
    say("7 wait all, both signalled: result %08lx\n", (unsigned long)r);
    svcCloseHandle(evs[0]);
    svcCloseHandle(evs[1]);

    /* 8 */
    reset_order();
    svcCreateMutex(&mutex, true);
    Thread t = spawn(mutex_taker, NULL, MAIN_PRIO);
    svcSleepThread(1000000);
    note('m');
    svcReleaseMutex(mutex);
    join(t);
    say("8 mutex: %s\n", order);
    svcCloseHandle(mutex);

    say("done\n");
    return 0;
}
