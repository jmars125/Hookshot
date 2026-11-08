/* Synthetic device calls around the real nn_audio.c output section. No device or game is opened. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *HWAVEOUT;
typedef char *LPSTR;
typedef unsigned MMRESULT;
typedef struct {
    unsigned wFormatTag, nChannels, nSamplesPerSec, wBitsPerSample, nBlockAlign, nAvgBytesPerSec;
} WAVEFORMATEX;
typedef struct { LPSTR lpData; unsigned dwBufferLength, dwFlags; } WAVEHDR;
#define MMSYSERR_NOERROR 0
#define WHDR_DONE 1
#define WHDR_PREPARED 2
#define WHDR_INQUEUE 16
#define WAVE_MAPPER ((unsigned)-1)
#define WAVE_FORMAT_PCM 1
#define CALLBACK_NULL 0
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", scenario, __LINE__, #x); exit(1); } } while (0)

static const char *scenario;
static int prepare_calls, write_calls, accepted, unprepare_calls, open_calls, logs;
static int fail_prepare, fail_write, fail_unprepare, fail_open;
static int queued[10];
static WAVEHDR *known[10];
static char last_log[512];

static const char *test_getenv(const char *name) { (void)name; return NULL; }
static void hos_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(last_log, sizeof last_log, format, args);
    va_end(args);
    logs++;
}
static int slot(WAVEHDR *h) {
    for (int i = 0; i < 10; i++) if (known[i] == h) return i;
    for (int i = 0; i < 10; i++) if (!known[i]) { known[i] = h; return i; }
    CHECK(0);
    return -1;
}
static MMRESULT test_open(HWAVEOUT *out, unsigned device, const WAVEFORMATEX *f,
                          uintptr_t callback, uintptr_t instance, unsigned flags) {
    (void)device; (void)callback; (void)instance; (void)flags;
    CHECK(f->nChannels == 2 && f->nSamplesPerSec == 48000 && f->wBitsPerSample == 16);
    open_calls++;
    if (fail_open) return 7;
    *out = (void *)(uintptr_t)1;
    return 0;
}
static MMRESULT test_prepare(HWAVEOUT out, WAVEHDR *h, unsigned size) {
    (void)out; (void)size;
    CHECK(!queued[slot(h)]);
    prepare_calls++;
    if (fail_prepare && (fail_prepare < 0 || prepare_calls == 1)) return 7;
    h->dwFlags |= WHDR_PREPARED;
    return 0;
}
static MMRESULT test_write(HWAVEOUT out, WAVEHDR *h, unsigned size) {
    (void)out; (void)size;
    const int i = slot(h);
    CHECK(!queued[i]);
    write_calls++;
    if (!(h->dwFlags & WHDR_PREPARED)) return 34; /* WAVERR_UNPREPARED: no completion follows. */
    if (fail_write && write_calls == 1) return 7; /* MMSYSERR_NOMEM: no buffer was queued. */
    queued[i] = 1;
    h->dwFlags = (h->dwFlags & ~WHDR_DONE) | WHDR_INQUEUE;
    accepted++;
    return 0;
}
static MMRESULT test_unprepare(HWAVEOUT out, WAVEHDR *h, unsigned size) {
    (void)out; (void)size;
    CHECK(!queued[slot(h)]);
    unprepare_calls++;
    if (fail_unprepare && unprepare_calls == 1) return 33; /* WAVERR_STILLPLAYING: do not reuse. */
    h->dwFlags &= ~WHDR_PREPARED;
    return 0;
}
#define getenv test_getenv
#define waveOutOpen test_open
#define waveOutPrepareHeader test_prepare
#define waveOutWrite test_write
#define waveOutUnprepareHeader test_unprepare
#include "audio_output.inc"

static void complete_all(void) {
    for (int i = 0; i < 10; i++) if (queued[i]) {
        queued[i] = 0;
        known[i]->dwFlags = (known[i]->dwFlags & ~WHDR_INQUEUE) | WHDR_DONE;
    }
}
static void frames(int count, int complete) {
    int16_t pcm[FRAME * 2];
    for (int j = 0; j < count; j++) {
        for (int i = 0; i < FRAME * 2; i++) pcm[i] = (int16_t)(j * 31 + i);
        output_frame(pcm);
        if (complete) complete_all();
    }
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    scenario = argv[1];
    fail_prepare = !strcmp(scenario, "prepare");
    fail_write = !strcmp(scenario, "write");
    fail_unprepare = !strcmp(scenario, "unprepare");
    fail_open = !strcmp(scenario, "open");
    if (!strcmp(scenario, "bounded")) fail_prepare = -1;
    output_open();
    CHECK(open_calls == 1);
    if (fail_open) {
        CHECK(!wout);
        frames(40, 1);
        CHECK(!prepare_calls && !write_calls && logs == 1);
        CHECK(strstr(last_log, "waveOutOpen") && strstr(last_log, "7"));
    } else if (fail_prepare > 0 || fail_write) {
        frames(40, 1);
        fprintf(stderr, "ring %s: accepted %d, prepare %d, write %d\n", scenario, accepted, prepare_calls, write_calls);
        CHECK(accepted == 19); /* One failed block, then the same slot works on the next ring. */
        CHECK(fill_block == 0 && fill_frames == 0);
        CHECK(logs == 1);
        if (fail_prepare) CHECK(write_calls == 19); /* Never write an unprepared header. */
    } else if (fail_unprepare) {
        frames(20, 1);
        int16_t before[FRAME * 4];
        memcpy(before, blocks[0], sizeof before);
        frames(1, 0);
        CHECK(fill_frames == 0 && fill_block == 0);
        CHECK(!memcmp(before, blocks[0], sizeof before));
        CHECK(hdr[0].dwFlags == (WHDR_DONE | WHDR_PREPARED));
        frames(2, 1);
        CHECK(accepted == 11 && unprepare_calls == 2 && logs == 1);
    } else if (!strcmp(scenario, "busy")) {
        frames(20, 0);
        int16_t before[FRAME * 4];
        memcpy(before, blocks[0], sizeof before);
        frames(5, 0);
        CHECK(accepted == 10 && fill_frames == 0 && fill_block == 0);
        CHECK(!memcmp(before, blocks[0], sizeof before));
        CHECK(!unprepare_calls);
        complete_all();
        frames(2, 1);
        CHECK(accepted == 11);
    } else if (fail_prepare < 0) {
        frames(80, 1);
        CHECK(prepare_calls == 40 && write_calls == 0 && accepted == 0);
        CHECK(logs > 0 && logs <= 8);
    } else {
        CHECK(!strcmp(scenario, "normal"));
        frames(40, 1);
        CHECK(accepted == 20 && prepare_calls == 20 && write_calls == 20 && unprepare_calls == 10);
        CHECK(fill_block == 0 && fill_frames == 0 && !logs);
        CHECK(blocks[9][0] == 38 * 31 && blocks[9][FRAME * 2] == 39 * 31);
    }
    printf("PASS %s: actual output ring\n", scenario);
    return 0;
}
