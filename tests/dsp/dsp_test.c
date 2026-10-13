/* M12 DSP test: libctru's ndsp, the sound library homebrew uses, driven
 * against our DSP HLE (hle/dsp.c). Every check follows from how the DSP is
 * documented to behave (3dbrew: DSP_Memory_Region), not from our own
 * numbers: an audio frame is 160 samples at the DSP's rate, buffers queued
 * on a channel play back to back, a rate multiplier scales how fast they
 * are consumed, a looping buffer never finishes.
 *
 * ndspInit loads a "DSP component" from sdmc:/3ds/dspfirm.cdc; the HLE never
 * runs it, so the test SD card (tests/dsp/sdmc) holds a placeholder.
 *
 * Output goes to stderr, routed through svcOutputDebugString; it must match
 * dsp_test.expected. */
#include <3ds.h>
#include <stdio.h>
#include <string.h>

#define say(...) fprintf(stderr, __VA_ARGS__)

#define NBUF 3
static ndspWaveBuf bufs[NBUF];
static volatile int done_at[NBUF];
static volatile u32 frames;

/* Called by ndsp's thread once per audio frame, after it has read the
   DSP's status for that frame. */
static void on_frame(void *arg)
{
    (void)arg;
    frames++;
    for (int i = 0; i < NBUF; i++)
        if (done_at[i] < 0 && bufs[i].status == NDSP_WBUF_DONE)
            done_at[i] = (int)frames;
}

static void wait_frames(u32 n)
{
    u32 until = frames + n;
    while (frames < until)
        svcSleepThread(1000000);
}

static int wait_done(int i, u32 limit)
{
    u32 until = frames + limit;
    while (done_at[i] < 0 && frames < until)
        svcSleepThread(1000000);
    return done_at[i];
}

static s16 *samples;

static void queue(int i, int channel, u32 offset, u32 count, bool looping)
{
    memset(&bufs[i], 0, sizeof bufs[i]);
    bufs[i].data_pcm16 = samples + offset;
    bufs[i].nsamples = count;
    bufs[i].looping = looping;
    done_at[i] = -1;
    ndspChnWaveBufAdd(channel, &bufs[i]);
}

static void setup(int channel, float rate_hz)
{
    ndspChnReset(channel);
    ndspChnSetFormat(channel, NDSP_FORMAT_MONO_PCM16);
    ndspChnSetInterp(channel, NDSP_INTERP_NONE);
    ndspChnSetRate(channel, rate_hz);
    float mix[12] = { 1.0f, 1.0f };
    ndspChnSetMix(channel, mix);
}

int main(void)
{
    consoleDebugInit(debugDevice_SVC);
    samples = linearAlloc(4096 * sizeof(s16));
    for (int i = 0; i < 4096; i++)
        samples[i] = (s16)(i * 7);
    DSP_FlushDataCache(samples, 4096 * sizeof(s16));

    Result rc = ndspInit();
    say("ndspInit: %s\n", R_SUCCEEDED(rc) ? "ok" : "FAILED");
    if (R_FAILED(rc))
        return 1;
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspSetCallback(on_frame, NULL);

    /* 1. Three buffers back to back at the DSP's own rate: 800, 480 and
          1600 samples are exactly 5, 3 and 10 frames. */
    setup(0, NDSP_SAMPLE_RATE);
    queue(0, 0, 0, 800, false);
    queue(1, 0, 800, 480, false);
    queue(2, 0, 1280, 1600, false);
    int d0 = wait_done(0, 100), d1 = wait_done(1, 100), d2 = wait_done(2, 100);
    say("back to back: buffer 0 %s\n", d0 >= 5 ? "finished" : "did NOT finish in time");
    say("back to back: buffer 1 finished %d frames after buffer 0 (expected 3)\n", d1 - d0);
    say("back to back: buffer 2 finished %d frames after buffer 1 (expected 10)\n", d2 - d1);
    wait_frames(2);
    say("back to back: channel playing afterwards: %s\n", ndspChnIsPlaying(0) ? "yes" : "no");

    /* 2. Half the DSP's rate: 800 samples take 10 frames, not 5. */
    setup(0, NDSP_SAMPLE_RATE / 2);
    u32 start = frames;
    queue(0, 0, 0, 800, false);
    queue(1, 0, 800, 160, false);
    int h0 = wait_done(0, 100), h1 = wait_done(1, 100);
    say("half rate: 800 samples %s\n", h0 >= 0 && (u32)h0 - start >= 10 ? "took at least 10 frames" : "finished TOO SOON");
    say("half rate: then 160 samples took %d frames (expected 2)\n", h1 - h0);

    /* 3. A looping buffer never finishes; its position stays inside it. */
    setup(1, NDSP_SAMPLE_RATE);
    queue(0, 1, 0, 1000, true);
    wait_frames(30);
    u32 pos = ndspChnGetSamplePos(1);
    say("looping: finished: %s, playing: %s, position inside the buffer: %s\n",
        done_at[0] >= 0 ? "YES" : "no", ndspChnIsPlaying(1) ? "yes" : "no", pos < 1000 ? "yes" : "NO");

    /* 4. Clearing the queue stops the channel. */
    ndspChnWaveBufClear(1);
    wait_frames(3);
    say("cleared: playing: %s\n", ndspChnIsPlaying(1) ? "YES" : "no");

    ndspExit();
    say("ndspExit: returned\n");
    linearFree(samples);
    return 0;
}
