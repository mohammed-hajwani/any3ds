/* dsp::DSP: the audio DSP, emulated at the level of what it does for the
 * app (no DSP code runs; the "component" the app loads is accepted and
 * ignored). Sources: 3dbrew's DSP_Services, DSP:* and DSP_Memory_Region
 * pages; libctru's ndsp is the client the tests drive.
 *
 * - The app talks to the DSP through pipe 2 (audio). Writing state 0
 *   (initialise) makes the DSP answer on the same pipe with the DSP
 *   addresses of 15 structures it keeps in two 0x8000-byte regions,
 *   0x1FF50000 and 0x1FF70000 (a DSP address is a 16-bit word address:
 *   process address = 0x1FF40000 + 2 * DSP address, +0x20000 for the
 *   second region). Their layout is up to the DSP firmware, so it is ours.
 * - Every audio frame (160 samples at 32728.5 Hz, so every 1310720 ticks of
 *   the 268 MHz system clock) the DSP reads the app's configuration from
 *   the region with the newer frame count, plays its inputs for one frame,
 *   writes their status to the other region and raises the pipe interrupt.
 * - Configuration fields are only fresh when their dirty flag is set (the
 *   app writes the two regions alternately), so the DSP keeps each input's
 *   state itself and applies flagged changes, then clears the flags.
 * - Addresses, sample counts and positions are 32-bit values the 16-bit DSP
 *   stores with their two halves swapped.
 *
 * Step 1 is silent: inputs are consumed at the right rate (so code waiting
 * for a buffer to finish carries on) but no samples are produced. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"
#include "service.h"

#define REGION0         0x1FF50000u
#define REGION1         0x1FF70000u
#define REGION_DSP_WORD 0x8000u         /* DSP word address of REGION0 */
#define FRAME_TICKS     1310720u        /* 160 samples at TICKS_PER_SEC / 8192 Hz */
#define FRAME_SAMPLES   160
#define NINPUTS         24

/* Our layout of the 15 structures inside each region (byte offsets), in
   the order 3dbrew lists them and the pipe reports them. */
enum {
    OFF_FRAME_COUNT  = 0x0000,  /* u16 */
    OFF_INPUT_CONFIG = 0x0020,  /* 24 x 192 bytes */
    OFF_INPUT_STATUS = 0x1220,  /* 24 x 12 */
    OFF_ADPCM_COEFS  = 0x1340,  /* 24 x 32 */
    OFF_DSP_CONFIG   = 0x1640,
    OFF_DSP_STATUS   = 0x1840,
    OFF_OUTPUT       = 0x1880,  /* 160 stereo frames, left then right */
    OFF_INTERMEDIATE = 0x1B00,
    OFF_COMPRESSOR   = 0x2F00,
    OFF_DEBUG        = 0x3300,
    OFF_UNKNOWN1     = 0x3400,
    OFF_UNKNOWN2     = 0x3500,
    OFF_UNKNOWN3     = 0x3600,
    OFF_BIQUAD1      = 0x3700,
    OFF_BIQUAD2      = 0x3740,
};
static const uint16_t structures[15] = {
    OFF_FRAME_COUNT, OFF_INPUT_CONFIG, OFF_INPUT_STATUS, OFF_ADPCM_COEFS, OFF_DSP_CONFIG,
    OFF_DSP_STATUS, OFF_OUTPUT, OFF_INTERMEDIATE, OFF_COMPRESSOR, OFF_DEBUG,
    OFF_UNKNOWN1, OFF_UNKNOWN2, OFF_UNKNOWN3, OFF_BIQUAD1, OFF_BIQUAD2,
};

/* Input configuration (192 bytes): the fields we use. */
enum {
    IC_DIRTY = 0, IC_GAIN = 4, IC_RATE = 52, IC_QUEUE_DIRTY = 74, IC_QUEUE = 76,
    IC_PLAY = 160, IC_SYNC = 162, IC_PADDR = 172, IC_COUNT = 176, IC_FORMAT = 180,
    IC_FLAGS = 188, IC_ID = 190, IC_SIZE = 192,
};
/* A queued buffer (20 bytes) inside the configuration. */
enum { QB_PADDR = 0, QB_COUNT = 4, QB_LOOP = 15, QB_ID = 16, QB_SIZE = 20 };

/* Dirty flags (as libctru's ndsp sets them, mirroring the official library). */
#define DIRTY_EMBEDDED  0x00000010u     /* start the embedded buffer */
#define DIRTY_PLAY      0x00010000u
#define DIRTY_RATE      0x00040000u
#define DIRTY_QUEUE     0x00080000u
#define DIRTY_SYNC      0x10000000u
#define DIRTY_RESET     0x20000000u

typedef struct {
    uint32_t paddr, count;
    uint16_t id;
    int looping;
} Buffer;

typedef struct {
    int playing;                /* play status 1 */
    int has_buffer;
    Buffer cur, queue[4];
    int queued, next_slot;      /* queue: FIFO; next_slot: the app's ring index to take next */
    double pos, rate;
    uint16_t sync, last_id, format;
    int started_next;           /* a buffer after the first started this frame */
} Input;

static Input inputs[NINPUTS];
static KObj *pipe_event, *sem_event, *binary_event;
static uint8_t pipe[64];
static uint32_t pipe_len;
static uint8_t binary[256];     /* the binary pipe (3): codec replies */
static uint32_t binary_len;
static int running, timer_added;
static uint16_t recv0;          /* reply register 0: 1 once a shutdown/sleep completed */
static int recv0_ready;

static uint8_t *at(uint32_t region, uint32_t off) { return kernel_mem() + region + off; }
static uint16_t r16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static void w16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static uint32_t r32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void w32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t rot(uint32_t v) { return v << 16 | v >> 16; }     /* the DSP's halfword order */
static float rf32(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }

static void raise_pipe_interrupt(void)
{
    if (pipe_event)
        kevent_signal(pipe_event);
}

static void reset_input(Input *in)
{
    uint16_t sync = in->sync;
    memset(in, 0, sizeof *in);
    in->sync = sync;
    in->rate = 1.0;
}

/* Apply the flagged changes in one input's configuration, then clear the flags. */
static void configure(Input *in, uint8_t *cfg)
{
    uint32_t dirty = r32(cfg + IC_DIRTY);
    if (dirty & DIRTY_RESET)
        reset_input(in);
    if (dirty & DIRTY_RATE)
        in->rate = rf32(cfg + IC_RATE);
    if (dirty & DIRTY_SYNC)
        in->sync = r16(cfg + IC_SYNC);
    if (dirty & DIRTY_EMBEDDED) {
        in->cur = (Buffer){ rot(r32(cfg + IC_PADDR)), rot(r32(cfg + IC_COUNT)), r16(cfg + IC_ID),
                            (r16(cfg + IC_FLAGS) >> 1) & 1 };
        in->format = r16(cfg + IC_FORMAT);
        in->has_buffer = 1;
        in->pos = 0;
        in->queued = 0;
        in->next_slot = 0;
    }
    if (dirty & DIRTY_QUEUE) {
        uint16_t bits = r16(cfg + IC_QUEUE_DIRTY);
        for (int k = 0; k < 4; k++) {
            int slot = (in->next_slot + k) & 3;
            if (!(bits >> slot & 1) || in->queued == 4)
                continue;
            const uint8_t *q = cfg + IC_QUEUE + QB_SIZE * slot;
            in->queue[in->queued++] = (Buffer){ rot(r32(q + QB_PADDR)), rot(r32(q + QB_COUNT)), r16(q + QB_ID),
                                                q[QB_LOOP] != 0 };
            bits &= (uint16_t)~(1u << slot);
            in->next_slot = (slot + 1) & 3;
        }
        w16(cfg + IC_QUEUE_DIRTY, bits);
    }
    if (dirty & DIRTY_PLAY)
        in->playing = (r16(cfg + IC_PLAY) & 0xFF) == 1;
    w32(cfg + IC_DIRTY, 0);
}

/* Play one frame of an input: consume rate * 160 samples, moving through
   its buffers back to back. */
static void play(Input *in)
{
    in->started_next = 0;
    if (!in->playing || !in->has_buffer)
        return;
    in->pos += in->rate * FRAME_SAMPLES;
    while (in->has_buffer && in->pos >= in->cur.count) {
        if (in->cur.looping && in->cur.count) {
            in->pos -= in->cur.count;
            continue;
        }
        in->pos -= in->cur.count;
        in->last_id = in->cur.id;
        if (in->queued) {
            in->cur = in->queue[0];
            memmove(&in->queue[0], &in->queue[1], (size_t)--in->queued * sizeof in->queue[0]);
            in->started_next = 1;
        } else {
            in->has_buffer = 0;
            in->pos = 0;
            in->started_next = 1;       /* report that the queue ran dry */
        }
    }
}

/* Input status (12 bytes): enabled, a buffer after the first started, sync
   count, position in the current buffer, current buffer id, last finished. */
static void write_status(const Input *in, uint8_t *st)
{
    st[0] = (uint8_t)(in->playing && in->has_buffer);
    st[1] = (uint8_t)in->started_next;
    w16(st + 2, in->sync);
    w32(st + 4, rot((uint32_t)in->pos));
    w16(st + 8, in->has_buffer ? in->cur.id : 0);
    w16(st + 10, in->last_id);
}

static void dsp_frame(void *arg)
{
    (void)arg;
    if (!running)
        return;
    uint16_t c0 = r16(at(REGION0, OFF_FRAME_COUNT)), c1 = r16(at(REGION1, OFF_FRAME_COUNT));
    if (!c0 && !c1)
        return;                         /* the app hasn't written a frame yet */
    uint32_t cur = (int16_t)(c1 - c0) > 0 ? REGION1 : REGION0, other = cur == REGION0 ? REGION1 : REGION0;
    for (int i = 0; i < NINPUTS; i++) {
        /* The app may have written both regions since our last frame (the
           official library runs its frame twice per DSP frame here), so the
           older region's changes are applied too, before the current one's.
           Applied changes have their dirty flags cleared, so nothing is
           applied twice. */
        configure(&inputs[i], at(other, OFF_INPUT_CONFIG + IC_SIZE * i));
        configure(&inputs[i], at(cur, OFF_INPUT_CONFIG + IC_SIZE * i));
        play(&inputs[i]);
        write_status(&inputs[i], at(other, OFF_INPUT_STATUS + 12 * i));
        write_status(&inputs[i], at(cur, OFF_INPUT_STATUS + 12 * i));
    }
    memset(at(other, OFF_DSP_STATUS), 0, 0x20);                     /* no dropped frames */
    memset(at(other, OFF_OUTPUT), 0, FRAME_SAMPLES * 4);            /* silent (step 1) */
    raise_pipe_interrupt();
}

/* The AAC decoder (codec 1). 3dbrew doesn't document its messages, so this
 * layout is read from the client in Pokémon X (0x2c8504 init, 0x2c8a0c
 * decode, 0x2c8e98 reply): requests are u16 codec, u16 command, u32
 * result, then arguments; replies echo the request with the result and,
 * for a decode, u32 sample-rate index (into 48000, 44100, 32000, 24000,
 * 22050, 16000, 12000, 11025, 8000) at +8, u32 channels at +12, u32 bytes
 * of input consumed at +16.
 *   0 init      (four arguments, unused here)
 *   1 decode    +8 input address, +12 size (one ADTS frame), +16 / +20 the
 *               left and right outputs (1024 16-bit samples each), all physical
 *   2 shutdown, others: acknowledged
 * We decode to silence: the frame's ADTS header (ISO 14496-3, a public
 * format) gives its length, rate and channels, which is all the game reads.
 * No sound comes out yet anyway. */
#define CODEC_AAC        1
#define AAC_SAMPLES      1024

static uint32_t aac_request(const uint8_t *req, uint8_t *reply)
{
    if (r16(req + 2) != 1)
        return 0;
    uint32_t src = gpu_phys_to_virt(r32(req + 8)), size = r32(req + 12);
    if (!src || size < 7)
        return 1;
    const uint8_t *h = kernel_mem() + src;
    if (h[0] != 0xFF || (h[1] & 0xF0) != 0xF0)     /* ADTS sync word */
        return 1;
    uint32_t rate = (h[2] >> 2) & 0xF, channels = ((h[2] & 1) << 2) | (h[3] >> 6);
    uint32_t length = ((uint32_t)(h[3] & 3) << 11) | (uint32_t)h[4] << 3 | h[5] >> 5;
    if (rate < 3 || rate > 11 || !channels || length > size)
        return 1;
    for (int ch = 0; ch < 2; ch++) {
        uint32_t out = gpu_phys_to_virt(r32(req + 16 + 4 * ch));
        if (out)
            memset(kernel_mem() + out, 0, AAC_SAMPLES * 2);
    }
    w32(reply + 8, rate - 3);           /* ADTS index 3 is 48000 Hz */
    w32(reply + 12, channels);
    w32(reply + 16, length);
    return 0;
}

/* WriteProcessPipe on the binary pipe: a request for one of the DSP's
   codecs (3dbrew: DSP:WriteProcessPipe; u16 codec, u16 command, u32
   result, 24 bytes of arguments). The reply goes on the same pipe, with
   the interrupt. Other codecs answer result 1 ("failed"): an app is left to
   go on without (a movie without its soundtrack, say). */
static void binary_pipe_request(const uint8_t *req, uint32_t size)
{
    if (size < 8 || binary_len + size > sizeof binary) {
        fprintf(stderr, "hle: DSP binary pipe: a %" PRIu32 "-byte request is not modelled\n", size);
        exit(3);
    }
    uint8_t *reply = binary + binary_len;
    memcpy(reply, req, size);
    w32(reply + 4, r16(req) == CODEC_AAC ? aac_request(req, reply) : 1);
    binary_len += size;
    if (binary_event)
        kevent_signal(binary_event);
}

/* WriteProcessPipe on the audio pipe: the first halfword is the new state. */
static void audio_pipe_command(uint16_t state)
{
    switch (state) {
    case 0:                             /* initialise */
    case 2:                             /* wake up */
        if (state == 0)
            for (int i = 0; i < NINPUTS; i++)
                reset_input(&inputs[i]);
        pipe_len = 0;
        w16(pipe + pipe_len, 15);
        pipe_len += 2;
        for (int i = 0; i < 15; i++, pipe_len += 2)
            w16(pipe + pipe_len, (uint16_t)(REGION_DSP_WORD + structures[i] / 2));
        running = 1;
        if (!timer_added) {
            kernel_add_timer(FRAME_TICKS, dsp_frame, NULL);
            timer_added = 1;
        }
        raise_pipe_interrupt();
        return;
    case 1:                             /* shut down */
    case 3:                             /* sleep */
        running = 0;
        recv0 = 1;
        recv0_ready = 1;
        return;
    default:
        fprintf(stderr, "hle: DSP audio pipe: unknown state %u\n", state);
        exit(3);
    }
}

void dsp_handle(KObj *session, uint32_t *cmd)
{
    (void)session;
    uint32_t id = cmd[0] >> 16;
    switch (id) {
    case 0x01:      /* RecvData(register) -> value */
        cmd[0] = ipc_header(id, 2, 0);
        cmd[2] = cmd[1] == 0 ? recv0 : 0;
        if (cmd[1] == 0)
            recv0_ready = 0;
        cmd[1] = RES_OK;
        return;
    case 0x02:      /* RecvDataIsReady(register) -> ready */
        cmd[0] = ipc_header(id, 2, 0);
        cmd[2] = cmd[1] == 0 ? (uint32_t)recv0_ready : 0;
        cmd[1] = RES_OK;
        return;
    case 0x07:      /* SetSemaphore(value): the app finished writing a frame */
    case 0x17:      /* SetSemaphoreMask(mask) */
    case 0x12:      /* UnloadComponent */
    case 0x13:      /* FlushDataCache(address, size, process) */
    case 0x14:      /* InvalidateDataCache(address, size, process) */
        ipc_reply(cmd, RES_OK);
        return;
    case 0x0C:      /* ConvertProcessAddressFromDspDram(address) -> process address */
        cmd[0] = ipc_header(id, 2, 0);
        cmd[2] = (cmd[1] << 1) + 0x1FF40000u;
        cmd[1] = RES_OK;
        return;
    case 0x0D: {    /* WriteProcessPipe(channel, size; static buffer) */
        uint32_t channel = cmd[1], size = cmd[2], addr = cmd[4];
        if (channel == 3) {
            binary_pipe_request(kernel_mem() + addr, size);
            ipc_reply(cmd, RES_OK);
            return;
        }
        if (channel != 2 || size < 2) {
            fprintf(stderr, "hle: DSP WriteProcessPipe(channel %" PRIu32 ", %" PRIu32 " bytes) not modelled\n",
                    channel, size);
            exit(3);
        }
        audio_pipe_command(r16(kernel_mem() + addr));
        ipc_reply(cmd, RES_OK);
        return;
    }
    case 0x0F:      /* GetPipeReadableSize(channel, peer) -> size */
        cmd[0] = ipc_header(id, 2, 0);
        cmd[2] = cmd[1] == 2 ? pipe_len : cmd[1] == 3 ? binary_len : 0;
        cmd[1] = RES_OK;
        return;
    case 0x10: {    /* ReadPipeIfPossible(channel, peer, size) -> size read; static buffer 0 */
        uint32_t channel = cmd[1], want = cmd[3] & 0xFFFF, got = 0;
        uint32_t dest = ipc_static_buffer(0, NULL);
        if (channel == 2 || channel == 3) {
            uint8_t *buf = channel == 2 ? pipe : binary;
            uint32_t *len = channel == 2 ? &pipe_len : &binary_len;
            got = want < *len ? want : *len;
            memcpy(kernel_mem() + dest, buf, got);
            memmove(buf, buf + got, *len - got);
            *len -= got;
        }
        cmd[0] = ipc_header(id, 2, 2);
        cmd[1] = RES_OK;
        cmd[2] = got;
        cmd[3] = IPC_STATIC_BUFFER(got, 0);
        cmd[4] = dest;
        return;
    }
    case 0x11: {    /* LoadComponent(size, program mask, data mask; buffer) -> loaded */
        uint32_t desc = cmd[4], buf = cmd[5];
        cmd[0] = ipc_header(id, 2, 2);
        cmd[1] = RES_OK;
        cmd[2] = 1;                         /* loaded (we never run it) */
        cmd[3] = desc;
        cmd[4] = buf;
        return;
    }
    case 0x15: {    /* RegisterInterruptEvents(interrupt, channel; event, 0 = unregister) */
        uint32_t interrupt = cmd[1], channel = cmd[2];
        KObj *ev = cmd[4] ? handle_get(cmd[4]) : NULL;
        if (interrupt == 2 && channel == 2)
            pipe_event = ev;
        else if (interrupt == 2 && channel == 3)
            binary_event = ev;
        else if (ev)
            fprintf(stderr, "hle: DSP interrupt %" PRIu32 "/%" PRIu32 " registered; it never fires\n",
                    interrupt, channel);
        ipc_reply(cmd, RES_OK);
        return;
    }
    case 0x16:      /* GetSemaphoreEventHandle -> event the app signals after writing a frame */
        if (!sem_event)
            sem_event = kevent_new(KRESET_ONESHOT, "DSP semaphore");
        cmd[0] = ipc_header(id, 1, 2);
        cmd[1] = RES_OK;
        cmd[2] = IPC_COPY_HANDLES(1);
        cmd[3] = handle_new(sem_event);
        return;
    case 0x1F:      /* GetHeadphoneStatus -> 0: not inserted */
    case 0x21:      /* GetIsDspOccupied -> 0 */
        cmd[0] = ipc_header(id, 2, 0);
        cmd[1] = RES_OK;
        cmd[2] = 0;
        return;
    default:
        ipc_unhandled("dsp::DSP", cmd);
    }
}
