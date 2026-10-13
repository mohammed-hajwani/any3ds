/* HLE of the 3DS kernel (Horizon): kernel objects, handles, threads and a
 * scheduler, and a virtual clock. Written from the public 3dbrew docs.
 *
 * Threads: every guest thread runs on its own host thread (it is a deep
 * stack of recompiled C calls, so it needs its own host stack), but only
 * one runs at a time. The running thread holds the kernel lock; it gives
 * the CPU away only inside an svc (when it blocks, yields, or wakes a
 * higher-priority thread). That makes every run deterministic.
 *
 * Time is virtual: each svc costs one microsecond, and when every thread
 * is blocked the clock jumps to the next deadline (a sleep, a timeout, or
 * a periodic timer such as vblank). */
#ifndef HLE_KERNEL_H
#define HLE_KERNEL_H

#include <stdint.h>

#include "context.h"

#define TICKS_PER_SEC 268111856ull      /* the ARM11 system tick */

/* Result codes (as Horizon returns them). */
#define RES_OK              0x00000000u
#define RES_TIMEOUT         0x09401BFEu
#define RES_INVALID_HANDLE  0xD8E007F7u
#define RES_NOT_OWNER       0xD8E0041Fu
#define RES_OUT_OF_RANGE    0xD8E007FDu
#define RES_NOT_FOUND       0xD88007FAu

typedef enum {
    KO_EVENT = 1,
    KO_MUTEX,
    KO_SEMAPHORE,
    KO_THREAD,
    KO_ARBITER,
    KO_SHMEM,       /* a shared-memory block a service hands out */
    KO_SESSION,     /* a client session to an HLE service */
    KO_RESLIMIT,
} KType;

enum { KRESET_ONESHOT = 0, KRESET_STICKY = 1, KRESET_PULSE = 2 };

struct KThread;
struct Service;

typedef struct KObj {
    KType type;
    char name[16];
    struct KObj *next_all;          /* every object ever made, so none leaks */
    void *data;                     /* a session's own state (an open file, ...) */
    union {
        struct { int reset, signaled; } ev;
        struct { struct KThread *owner; int count; } mtx;
        struct { int32_t count, max; } sem;
        struct KThread *thread;
        struct {
            uint32_t size, addr;    /* addr: where the app mapped it, 0 if not */
            uint32_t home;          /* where it goes when the app maps it at 0 */
            const uint8_t *init;    /* contents it starts with (NULL: zeros) */
        } shm;
        const struct Service *service;
    };
} KObj;

/* Objects and handles. */
KObj    *kobj_new(KType type, const char *name);
uint32_t handle_new(KObj *o);
KObj    *handle_get(uint32_t handle);      /* NULL if invalid */
KObj    *kevent_new(int reset, const char *name);
void     kevent_signal(KObj *ev);

/* Shared memory blocks: the service side reads/writes them through this
   (NULL until the app has mapped the block). */
uint8_t *kshm_ptr(KObj *shm);

/* The clock. */
uint64_t kernel_now(void);
typedef void (*TimerFn)(void *arg);
void     kernel_add_timer(uint64_t period, TimerFn fn, void *arg);

/* The 60 Hz display refresh: started by the first service that needs it
   (GSP, HID); each vblank calls the services' hooks. */
void     kernel_start_vblank(void);
uint32_t kernel_frame(void);

/* Guest memory and the running thread. */
uint8_t *kernel_mem(void);
Ctx     *kernel_current(void);

/* Logging: HLE_TRACE=1 logs every svc and IPC request to stderr. */
extern int hle_trace;

/* Start the program (on the calling host thread, as guest thread 0). Does
   not return: the run ends with svcExitProcess, a deadlock report, or the
   virtual-time limit. */
typedef struct {
    uint32_t entry;
    uint32_t max_seconds;           /* virtual-time limit, catches hangs */
    void   (*on_exit)(void);        /* e.g. dump the framebuffer */
} HleConfig;
void hle_run(uint8_t *mem, const HleConfig *config) __attribute__((noreturn));

/* ---- Kernel operations behind the svcs (svc.c) ----
   Calls that can block write their result into the calling thread's r0
   (and r1) themselves: whoever wakes the thread decides the result. */
void     k_svc_enter(void);                 /* each svc: advance the clock, run due timers */
void     k_svc_leave(void);                 /* each svc: let a higher-priority thread in */
uint32_t k_create_thread(uint32_t entry, uint32_t arg, uint32_t stack_top, int prio, uint32_t *handle);
void     k_exit_thread(void) __attribute__((noreturn));
void     k_exit_process(void) __attribute__((noreturn));
void     k_sleep(int64_t ns);
void     k_wait(KObj **objs, int n, int wait_all, int64_t timeout_ns);
void     k_arbitrate(KObj *arbiter, uint32_t addr, uint32_t type, int32_t value, int64_t timeout_ns);
uint32_t k_release_mutex(KObj *mutex);
uint32_t k_release_semaphore(KObj *sem, int32_t count, int32_t *previous);
uint32_t k_thread_id(KObj *thread);         /* thread == NULL: the current thread */
int      k_thread_priority(KObj *thread);
void     k_set_thread_priority(KObj *thread, int prio);
KObj    *k_current_thread_obj(void);
void     k_close_handle(uint32_t handle);

/* Services' vblank hooks (gsp.c, hid.c). */
void gsp_vblank(void);
void hid_vblank(uint32_t frame);

/* For the runner: scripted input (from `frame` on, hold `keys`; HID's
   KEY_* bits), and saving the last frame shown as a .bmp (top screen
   above bottom screen, 400x480). */
void hid_hold_keys(uint32_t frame, uint32_t keys);
void hid_touch(uint32_t frame, int down, uint32_t x, uint32_t y);
int  gsp_dump_screens(const char *path);
void fs_set_sdmc(const char *dir);          /* back the SD card with a host folder */
void fs_set_save(const char *dir);          /* back the app's save data with a host folder */
int  fs_set_romfs(const char *path);        /* the app's RomFS: an image, a .3dsx or a decrypted .cxi/.3ds */
int  apt_set_shared_font(const char *path); /* your own dump of the system font (a .bcfnt) */

/* Fill the kernel config page and the shared page (config_mem.c). */
void config_mem_init(uint8_t *mem);

#endif
