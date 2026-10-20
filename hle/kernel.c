/* HLE kernel: objects, handles, the scheduler and the virtual clock.
 * See kernel.h for the threading model. */
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"
#include "kernel.h"

#define MAX_THREADS 4096            /* every thread ever created (finished ones stay, for joins) */
#define MAX_TLS     64              /* threads alive at once: each has a TLS block */
#define MAX_WAIT    64              /* WaitSynchronizationN's limit */
#define MAX_HANDLES 4096
#define MAX_TIMERS  8
#define NO_DEADLINE UINT64_MAX
#define SVC_COST    (TICKS_PER_SEC / 1000000)   /* each svc takes 1 us */

#define TLS_BASE    0x1FF82000u     /* TLS block i: TLS_BASE + 0x200 * i, reused once its thread finishes */
#define EXIT_ADDR   0x00ABC000u     /* return address of a thread's entry function */
#define HOST_STACK  (16u << 20)

typedef enum { T_READY, T_WAITING, T_DEAD } TState;

typedef struct KThread {
    Ctx ctx;
    KObj *obj;
    uint32_t id, entry;
    int prio, index;
    TState state;
    uint64_t ready_seq;             /* round robin within a priority: oldest ready runs first */
    pthread_cond_t cv;              /* signalled when this thread is given the CPU */
    pthread_t host;
    jmp_buf exit_jmp;

    /* What it waits for, while state == T_WAITING. */
    KObj *wait_objs[MAX_WAIT];
    int wait_n, wait_all;
    KObj *wait_arbiter;             /* ArbitrateAddress: arbiter + address */
    uint32_t wait_addr;
    uint64_t wait_seq, deadline;
    uint32_t timeout_result;        /* r0 when the deadline passes */
    const char *wait_what;
} KThread;

typedef struct { uint64_t due, period; TimerFn fn; void *arg; } Timer;

int hle_trace;

static pthread_mutex_t big_lock = PTHREAD_MUTEX_INITIALIZER;
static KThread *threads[MAX_THREADS];
static KThread *tls_owner[MAX_TLS];
static int nthreads;
static KThread *cur;
static uint64_t now_ticks, seq;
static uint8_t *mem;
static HleConfig config;

static KObj *all_objs;
static KObj *handles[MAX_HANDLES];
static uint32_t handle_cursor;

static Timer timers[MAX_TIMERS];
static int ntimers;
static uint32_t frame_count;
static int vblank_started;

/* ------------------------------------------------------------------ */
/* Objects and handles                                                  */

KObj *kobj_new(KType type, const char *name)
{
    KObj *o = calloc(1, sizeof *o);
    if (!o) {
        perror("kobj_new");
        exit(1);
    }
    o->type = type;
    snprintf(o->name, sizeof o->name, "%s", name);
    o->next_all = all_objs;
    all_objs = o;
    return o;
}

/* Handles are slots in one table: 0x100 + index. Freed slots are reused
   only after the cursor wraps, so a stale handle stays invalid for long. */
uint32_t handle_new(KObj *o)
{
    for (uint32_t tries = 0; tries < MAX_HANDLES; tries++) {
        uint32_t i = handle_cursor++ % MAX_HANDLES;
        if (!handles[i]) {
            handles[i] = o;
            return 0x100 + i;
        }
    }
    fprintf(stderr, "hle: out of handles\n");
    exit(3);
}

KObj *handle_get(uint32_t h)
{
    if (h == 0xFFFF8000u)           /* CUR_THREAD_HANDLE */
        return cur->obj;
    if (h < 0x100 || h - 0x100 >= MAX_HANDLES)
        return NULL;
    return handles[h - 0x100];
}

static void handle_close(uint32_t h)
{
    if (h >= 0x100 && h - 0x100 < MAX_HANDLES)
        handles[h - 0x100] = NULL;
}

KObj *kevent_new(int reset, const char *name)
{
    KObj *o = kobj_new(KO_EVENT, name);
    o->ev.reset = reset;
    return o;
}

uint8_t *kshm_ptr(KObj *shm)
{
    return shm->shm.addr ? mem + shm->shm.addr : NULL;
}

uint8_t *kernel_mem(void) { return mem; }
Ctx *kernel_current(void) { return &cur->ctx; }
uint64_t kernel_now(void) { return now_ticks; }
uint32_t kernel_frame(void) { return frame_count; }
KObj *k_current_thread_obj(void) { return cur->obj; }

void k_close_handle(uint32_t h) { handle_close(h); }

/* ------------------------------------------------------------------ */
/* Waiting and waking                                                   */

static uint64_t ns_to_ticks(int64_t ns)
{
    return (uint64_t)((unsigned __int128)(uint64_t)ns * TICKS_PER_SEC / 1000000000u);
}

static int obj_available(const KObj *o, const KThread *t)
{
    switch (o->type) {
    case KO_EVENT:     return o->ev.signaled;
    case KO_MUTEX:     return o->mtx.owner == NULL || o->mtx.owner == t;
    case KO_SEMAPHORE: return o->sem.count > 0;
    case KO_THREAD:    return o->thread->state == T_DEAD;
    default:           return 0;
    }
}

static void obj_acquire(KObj *o, KThread *t)
{
    switch (o->type) {
    case KO_EVENT:
        if (o->ev.reset == KRESET_ONESHOT)
            o->ev.signaled = 0;
        break;
    case KO_MUTEX:
        o->mtx.owner = t;
        o->mtx.count++;
        break;
    case KO_SEMAPHORE:
        o->sem.count--;
        break;
    default:
        break;
    }
}

/* If t's wait can complete now, take the objects and return 1, with r0/r1
   set as WaitSynchronization(N) returns them. */
static int try_satisfy(KThread *t)
{
    if (t->wait_all) {
        for (int i = 0; i < t->wait_n; i++)
            if (!obj_available(t->wait_objs[i], t))
                return 0;
        for (int i = 0; i < t->wait_n; i++)
            obj_acquire(t->wait_objs[i], t);
        t->ctx.r[0] = RES_OK;
        return 1;
    }
    for (int i = 0; i < t->wait_n; i++)
        if (obj_available(t->wait_objs[i], t)) {
            obj_acquire(t->wait_objs[i], t);
            t->ctx.r[0] = RES_OK;
            t->ctx.r[1] = (uint32_t)i;
            return 1;
        }
    return 0;
}

static void make_ready(KThread *t)
{
    t->state = T_READY;
    t->ready_seq = ++seq;
    t->wait_n = 0;
    t->wait_arbiter = NULL;
    t->deadline = NO_DEADLINE;
    t->wait_what = NULL;
}

/* Is a better than b to wake first: higher priority (lower number), then
   whoever started waiting first. */
static int wakes_before(const KThread *a, const KThread *b)
{
    return !b || a->prio < b->prio || (a->prio == b->prio && a->wait_seq < b->wait_seq);
}

/* Something changed (signal, release, thread exit): wake every waiter
   whose wait can now complete, best first, one at a time, because each
   wakeup can consume what the next one needed. */
static void wake_waiters(void)
{
    for (;;) {
        KThread *best = NULL;
        for (int i = 0; i < nthreads; i++) {
            KThread *t = threads[i];
            if (t->state != T_WAITING || t->wait_n == 0 || !wakes_before(t, best))
                continue;
            /* Check without taking anything yet. */
            int ok = t->wait_all;
            for (int k = 0; k < t->wait_n; k++) {
                int avail = obj_available(t->wait_objs[k], t);
                if (t->wait_all && !avail) { ok = 0; break; }
                if (!t->wait_all && avail) { ok = 1; break; }
            }
            if (ok)
                best = t;
        }
        if (!best)
            return;
        try_satisfy(best);
        make_ready(best);
    }
}

void kevent_signal(KObj *ev)
{
    ev->ev.signaled = 1;
    wake_waiters();
    if (ev->ev.reset == KRESET_PULSE)  /* a pulse wakes whoever waits right now, then resets */
        ev->ev.signaled = 0;
}

/* ------------------------------------------------------------------ */
/* Timers and the clock                                                 */

void kernel_add_timer(uint64_t period, TimerFn fn, void *arg)
{
    if (ntimers == MAX_TIMERS) {
        fprintf(stderr, "hle: too many timers\n");
        exit(3);
    }
    timers[ntimers++] = (Timer){ now_ticks + period, period, fn, arg };
}

static void vblank(void *arg)
{
    (void)arg;
    frame_count++;
    gsp_vblank();
    hid_vblank(frame_count);
    gpu_frame(frame_count);
    if (config.live_path && frame_count % config.live_every == 0) {
        /* A live view (runner --live): written to a side file, then renamed,
           so a viewer never reads half a picture. */
        char tmp[1024];
        snprintf(tmp, sizeof tmp, "%s.tmp", config.live_path);
        if (gsp_dump_screens(tmp))
            rename(tmp, config.live_path);
    }
}

void kernel_start_vblank(void)
{
    if (vblank_started)
        return;
    vblank_started = 1;
    kernel_add_timer(TICKS_PER_SEC / 60, vblank, NULL);
}

static void report_threads(void)
{
    for (int i = 0; i < nthreads; i++) {
        KThread *t = threads[i];
        const char *state = t->state == T_DEAD ? "finished" : t->state == T_READY ? "ready" : "waiting";
        fprintf(stderr, "    thread %" PRIu32 " (entry %08" PRIx32 ", priority 0x%02x): %s",
                t->id, t->entry, t->prio, state);
        if (t->state == T_WAITING) {
            fprintf(stderr, " in %s", t->wait_what ? t->wait_what : "?");
            for (int k = 0; k < t->wait_n; k++)
                fprintf(stderr, "%s%s", k ? ", " : " on ", t->wait_objs[k]->name);
            if (t->wait_arbiter)
                fprintf(stderr, " on address %08" PRIx32, t->wait_addr);
        }
        fprintf(stderr, "  (lr = %08" PRIx32 ")\n", t->ctx.r[14]);
        if (t->state == T_READY)
            fprintf(stderr, "        r0-r3 = %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
                    "  r4-r7 = %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "  sp = %08" PRIx32 "\n",
                    t->ctx.r[0], t->ctx.r[1], t->ctx.r[2], t->ctx.r[3],
                    t->ctx.r[4], t->ctx.r[5], t->ctx.r[6], t->ctx.r[7], t->ctx.r[13]);
    }
}

/* Fire due timers and expired waits. */
static void run_due(void)
{
    for (int i = 0; i < ntimers; i++)
        while (timers[i].due <= now_ticks) {
            timers[i].due += timers[i].period;
            timers[i].fn(timers[i].arg);
        }
    for (int i = 0; i < nthreads; i++) {
        KThread *t = threads[i];
        if (t->state == T_WAITING && t->deadline <= now_ticks) {
            t->ctx.r[0] = t->timeout_result;
            make_ready(t);
        }
    }
    if (config.max_seconds && now_ticks > config.max_seconds * TICKS_PER_SEC) {
        fprintf(stderr, "hle: stopping: still running after %" PRIu32 " s of virtual time (frame %" PRIu32 ")\n",
                config.max_seconds, frame_count);
        report_threads();
        if (config.on_exit)
            config.on_exit();       /* e.g. save the screens: what was it showing? */
        exit(6);
    }
}

/* Nobody can run: jump the clock to the next thing that can happen. */
static void idle_advance(void)
{
    uint64_t next = NO_DEADLINE;
    for (int i = 0; i < ntimers; i++)
        if (timers[i].due < next)
            next = timers[i].due;
    for (int i = 0; i < nthreads; i++)
        if (threads[i]->state == T_WAITING && threads[i]->deadline < next)
            next = threads[i]->deadline;
    if (next == NO_DEADLINE) {
        int alive = 0;
        for (int i = 0; i < nthreads; i++)
            alive += threads[i]->state != T_DEAD;
        if (!alive) {
            fprintf(stderr, "hle: every thread has finished\n");
            exit(0);
        }
        fprintf(stderr, "hle: deadlock: no thread can run, and nothing is scheduled to wake one\n");
        report_threads();
        exit(4);
    }
    if (next > now_ticks)
        now_ticks = next;
    run_due();
}

/* ------------------------------------------------------------------ */
/* Scheduling                                                           */

static KThread *pick_ready(void)
{
    KThread *best = NULL;
    for (int i = 0; i < nthreads; i++) {
        KThread *t = threads[i];
        if (t->state == T_READY &&
            (!best || t->prio < best->prio || (t->prio == best->prio && t->ready_seq < best->ready_seq)))
            best = t;
    }
    return best;
}

static KThread *next_to_run(void)
{
    for (;;) {
        KThread *next = pick_ready();
        if (next)
            return next;
        idle_advance();
    }
}

/* Hand the CPU to `next` and return once this thread has it back. */
static void switch_to(KThread *me, KThread *next)
{
    if (next == me)
        return;
    cur = next;
    next->ctx.excl_valid = 0;       /* a context switch clears the exclusive monitor */
    pthread_cond_signal(&next->cv);
    while (cur != me)
        pthread_cond_wait(&me->cv, &big_lock);
}

static void reschedule(void)
{
    switch_to(cur, next_to_run());
}

/* Block the current thread until it is woken or the deadline passes. */
static void block(uint64_t deadline, uint32_t timeout_result, const char *what)
{
    cur->state = T_WAITING;
    cur->wait_seq = ++seq;
    cur->deadline = deadline;
    cur->timeout_result = timeout_result;
    cur->wait_what = what;
    reschedule();
}

static uint64_t deadline_after(int64_t ns)
{
    return ns < 0 ? NO_DEADLINE : now_ticks + ns_to_ticks(ns);
}

void k_svc_enter(void)
{
    now_ticks += SVC_COST;
    run_due();
}

void k_svc_leave(void)
{
    reschedule();                   /* a woken higher-priority thread goes first */
}

/* RECOMP_TICK_EDGES loop iterations have run without an svc (see
   RECOMP_BACKEDGE). At a few instructions per iteration and ~268 per
   microsecond, call it 20 us; then do what an svc does: fire due timers,
   and let a thread they woke preempt this one. */
#define TICK_COST (20 * (TICKS_PER_SEC / 1000000))

static void k_tick(Ctx *ctx)
{
    (void)ctx;
    now_ticks += TICK_COST;
    run_due();
    reschedule();
}

/* ------------------------------------------------------------------ */
/* Threads                                                              */

static void run_guest(KThread *t)
{
    if (setjmp(t->exit_jmp) == 0) {
        GuestFn fn = recomp_find(t->entry);
        if (!fn) {
            fprintf(stderr, "hle: thread entry %08" PRIx32 " is not a recompiled function\n", t->entry);
            exit(3);
        }
        fn(&t->ctx);
    }
}

/* The thread is done (returned or svcExitThread): wake its joiners and
   hand the CPU on. Called with the lock held; releases it. */
static void thread_finish(KThread *t)
{
    t->state = T_DEAD;
    tls_owner[(t->ctx.tls - TLS_BASE) / 0x200u] = NULL;    /* its TLS block is free again */
    wake_waiters();
    KThread *next = next_to_run();
    cur = next;
    pthread_cond_signal(&next->cv);
    pthread_mutex_unlock(&big_lock);
}

static void *host_thread_main(void *arg)
{
    KThread *t = arg;
    pthread_mutex_lock(&big_lock);
    while (cur != t)
        pthread_cond_wait(&t->cv, &big_lock);
    run_guest(t);
    thread_finish(t);
    return NULL;
}

static KThread *thread_new(uint32_t entry, int prio)
{
    int slot = 0;
    while (slot < MAX_TLS && tls_owner[slot])
        slot++;
    if (nthreads == MAX_THREADS || slot == MAX_TLS) {
        fprintf(stderr, "hle: too many threads (%d alive)\n", slot);
        exit(3);
    }
    KThread *t = calloc(1, sizeof *t);
    if (!t) {
        perror("thread_new");
        exit(1);
    }
    t->index = nthreads;
    t->id = 0x20 + (uint32_t)nthreads;
    t->entry = entry;
    t->prio = prio;
    t->ctx.mem = mem;
    t->ctx.tls = TLS_BASE + 0x200u * (uint32_t)slot;
    tls_owner[slot] = t;
    memset(mem + t->ctx.tls, 0, 0x200);
    t->ctx.r[14] = EXIT_ADDR;
    t->deadline = NO_DEADLINE;
    pthread_cond_init(&t->cv, NULL);
    char name[16];
    snprintf(name, sizeof name, "thread %" PRIu32, t->id);
    t->obj = kobj_new(KO_THREAD, name);
    t->obj->thread = t;
    threads[nthreads++] = t;
    make_ready(t);
    return t;
}

uint32_t k_create_thread(uint32_t entry, uint32_t arg, uint32_t stack_top, int prio, uint32_t *handle)
{
    if (prio < 0 || prio > 0x3F)
        return RES_OUT_OF_RANGE;
    KThread *t = thread_new(entry, prio);
    t->ctx.r[0] = arg;
    t->ctx.r[13] = stack_top;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, HOST_STACK);
    if (pthread_create(&t->host, &attr, host_thread_main, t) != 0) {
        perror("pthread_create");
        exit(1);
    }
    pthread_attr_destroy(&attr);
    if (hle_trace)
        fprintf(stderr, "    -> thread %" PRIu32 " at %08" PRIx32 ", priority 0x%02x\n", t->id, entry, prio);
    *handle = handle_new(t->obj);
    return RES_OK;
}

void k_exit_thread(void)
{
    longjmp(cur->exit_jmp, 1);
}

void k_exit_process(void)
{
    if (config.on_exit)
        config.on_exit();
    fflush(stdout);
    fprintf(stderr, "hle: guest exited via svcExitProcess (frame %" PRIu32 ", %.3f s virtual)\n",
            frame_count, (double)now_ticks / TICKS_PER_SEC);
    exit(0);
}

uint32_t k_thread_id(KObj *o)
{
    return o ? o->thread->id : cur->id;
}

int k_thread_priority(KObj *o)
{
    return o->thread->prio;
}

void k_set_thread_priority(KObj *o, int prio)
{
    o->thread->prio = prio;
}

/* ------------------------------------------------------------------ */
/* Blocking operations                                                  */

void k_sleep(int64_t ns)
{
    if (ns <= 0) {                  /* yield: go behind the other threads of our priority */
        cur->ready_seq = ++seq;
        reschedule();
        return;
    }
    cur->wait_n = 0;
    block(deadline_after(ns), RES_OK, "svcSleepThread");
}

void k_wait(KObj **objs, int n, int wait_all, int64_t timeout_ns)
{
    if (n > MAX_WAIT) {
        fprintf(stderr, "hle: WaitSynchronizationN on %d handles\n", n);
        exit(3);
    }
    for (int i = 0; i < n; i++)
        if (objs[i]->type != KO_EVENT && objs[i]->type != KO_MUTEX &&
            objs[i]->type != KO_SEMAPHORE && objs[i]->type != KO_THREAD) {
            fprintf(stderr, "hle: waiting on a %s object (type %d) is not implemented\n",
                    objs[i]->name, objs[i]->type);
            exit(3);
        }
    memcpy(cur->wait_objs, objs, (size_t)n * sizeof *objs);
    cur->wait_n = n;
    cur->wait_all = wait_all;
    if (n && try_satisfy(cur)) {
        cur->wait_n = 0;
        return;
    }
    if (timeout_ns == 0) {
        cur->wait_n = 0;
        cur->ctx.r[0] = RES_TIMEOUT;
        return;
    }
    block(deadline_after(timeout_ns), RES_TIMEOUT, n > 1 ? "WaitSynchronizationN" : "WaitSynchronization");
}

/* Arbitration types (svcArbitrateAddress). */
enum { ARB_SIGNAL, ARB_WAIT_LT, ARB_DEC_WAIT_LT, ARB_WAIT_LT_TIMEOUT, ARB_DEC_WAIT_LT_TIMEOUT };

void k_arbitrate(KObj *arb, uint32_t addr, uint32_t type, int32_t value, int64_t timeout_ns)
{
    cur->ctx.r[0] = RES_OK;
    if (type == ARB_SIGNAL) {
        /* Wake `value` waiters on this address (all if negative), best first. */
        for (int woken = 0; value < 0 || woken < value; woken++) {
            KThread *best = NULL;
            for (int i = 0; i < nthreads; i++) {
                KThread *t = threads[i];
                if (t->state == T_WAITING && t->wait_arbiter == arb && t->wait_addr == addr &&
                    wakes_before(t, best))
                    best = t;
            }
            if (!best)
                break;
            best->ctx.r[0] = RES_OK;
            make_ready(best);
        }
        return;
    }
    if (type > ARB_DEC_WAIT_LT_TIMEOUT) {
        fprintf(stderr, "hle: ArbitrateAddress type %" PRIu32 "\n", type);
        exit(3);
    }
    int32_t v;
    memcpy(&v, mem + addr, 4);
    if (v >= value)
        return;
    if (type == ARB_DEC_WAIT_LT || type == ARB_DEC_WAIT_LT_TIMEOUT) {
        int32_t dec = v - 1;
        memcpy(mem + addr, &dec, 4);
    }
    int timed = type == ARB_WAIT_LT_TIMEOUT || type == ARB_DEC_WAIT_LT_TIMEOUT;
    cur->wait_n = 0;
    cur->wait_arbiter = arb;
    cur->wait_addr = addr;
    block(timed ? deadline_after(timeout_ns) : NO_DEADLINE, RES_TIMEOUT, "ArbitrateAddress");
}

uint32_t k_release_mutex(KObj *m)
{
    if (m->mtx.owner != cur)
        return RES_NOT_OWNER;
    if (--m->mtx.count == 0) {
        m->mtx.owner = NULL;
        wake_waiters();
    }
    return RES_OK;
}

uint32_t k_release_semaphore(KObj *s, int32_t count, int32_t *previous)
{
    if (count < 0 || s->sem.count + count > s->sem.max)
        return RES_OUT_OF_RANGE;
    *previous = s->sem.count;
    s->sem.count += count;
    wake_waiters();
    return RES_OK;
}

/* ------------------------------------------------------------------ */

void hle_svc(Ctx *ctx, uint32_t number);    /* svc.c */

void hle_run(uint8_t *guest_mem, const HleConfig *cfg)
{
    mem = guest_mem;
    config = *cfg;
    hle_trace = getenv("HLE_TRACE") != NULL;
    recomp_svc_handler = hle_svc;
    recomp_tick_handler = k_tick;
    config_mem_init(mem);
    gpu_init(mem);

    pthread_mutex_lock(&big_lock);
    KThread *main_thread = thread_new(cfg->entry, 0x30);
    main_thread->ctx.r[13] = 0x10000000u;      /* top of the main thread's stack */
    main_thread->host = pthread_self();
    cur = main_thread;
    run_guest(main_thread);
    thread_finish(main_thread);
    pthread_mutex_lock(&big_lock);  /* the other threads carry on; ExitProcess ends the run */
    for (;;)
        pthread_cond_wait(&main_thread->cv, &big_lock);
}
