/* Authored memory, clock and API spies; input_actual.inc contains the unmodified production path. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
#include "input_constants.inc"

typedef void *HWND;
typedef uint32_t DWORD;
typedef intptr_t LONG_PTR;
typedef struct { uint64_t x[32]; } Ctx;
enum { GWL_EXSTYLE = -20, WS_EX_TOOLWINDOW = 0x80, WS_EX_NOACTIVATE = 0x8000000,
       VK_MENU = 0x12, VK_RETURN = 0x0d, VK_LEFT = 0x25, VK_UP, VK_RIGHT, VK_DOWN,
       VK_LBUTTON = 1, VK_RBUTTON = 2 };
static int front, menu_background, bootstrap_polls, pad_merges, key_queries, mouse_queries, focus_queries, commands;
static int64_t now_ns = 10000000000LL, sampling;
static int key_bind[HOS_SW_N][2], pad_raw_now;
static unsigned char *memory;
static char last_log[256];
volatile int64_t hos_hid_last_read;
volatile int hos_nvn_sync_log_on;
static void *at(uint64_t address) { CHECK(address <= 512 - 8); return memory + address; }
#define GP(a) at(a)
#define G32(a) (*(uint32_t *)at(a))
#define G64(a) (*(uint64_t *)at(a))
#define ARG(n) (c->x[n])
#define RET(value) (c->x[0] = (uint64_t)(value))

int rx_option(int id, int otherwise) { return id == 4 ? menu_background : id == 1 ? 0 : otherwise; }
int64_t hos_now_ns(void) { return now_ns; }
void hos_log(const char *format, ...) {
    va_list args; va_start(args, format); vsnprintf(last_log, sizeof last_log, format, args); va_end(args);
}
HWND GetForegroundWindow(void) { focus_queries++; return (HWND)(uintptr_t)(front ? 1 : 2); }
DWORD GetWindowThreadProcessId(HWND window, DWORD *pid) { *pid = window == (HWND)(uintptr_t)1 ? 10 : 20; return 1; }
DWORD GetCurrentProcessId(void) { return 10; }
LONG_PTR GetWindowLongPtrA(HWND window, int index) { (void)window; (void)index; return 0; }
int GetClassNameA(HWND window, char *text, int size) { (void)window; return snprintf(text, (size_t)size, "fixture"); }
short GetAsyncKeyState(int key) { key_queries++; return key == 'P' || key == 'D' ? (short)0x8000 : 0; }
int hos_sdl_pads(uint64_t *buttons, int32_t stick[4]) {
    bootstrap_polls++; *buttons = 1ULL << B_Y; stick[0] = -31000; return 1;
}
void pads_read(uint64_t *buttons, int32_t stick[4]) {
    /* The physical backend boundary is a spy; actual routing merges its result with the script below. */
    pad_merges++; *buttons |= 1ULL << B_B; stick[0] = -12000; stick[2] = 8000;
}
void binds_load(void) { key_bind[B_X][0] = 'P'; }
int mouse_stick(int32_t *x, int32_t *y, int held) { (void)x; (void)y; (void)held; mouse_queries++; return 0; }
void rx_request_frame_dump(void) { commands++; }
void rx_set_screenshot_ms(int64_t ms) { (void)ms; commands++; }
void rx_request_rdoc_capture(int frames) { (void)frames; commands++; }
void rx_flash_dark_arm(void) { commands++; }

#include "input_actual.inc"

static void packet(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx) {
    static int64_t expected_sampling;
    Ctx context = {{0}};
    context.x[0] = 64; context.x[1] = 2; context.x[2] = 16; context.x[30] = 0x1234;
    G32(16) = NPAD_NO1;
    memset(memory + 64, 0xa5, 2 * STATE_SIZE + 8);
    npad_states(&context);
    CHECK(context.x[0] == 2);
    expected_sampling++;
    for (int i = 0; i < 2; i++) {
        const uint64_t address = 64 + (uint64_t)i * STATE_SIZE;
        CHECK(G64(address) == (uint64_t)(expected_sampling - i));
        CHECK(G64(address + 8) == buttons);
        CHECK((int32_t)G32(address + 16) == lx && (int32_t)G32(address + 20) == ly);
        CHECK((int32_t)G32(address + 24) == rx && G32(address + 28) == 0);
        CHECK(G32(address + 32) == ATTR_CONNECTED && G32(address + 36) == 0);
    }
    for (int i = 0; i < 8; i++) CHECK(memory[64 + 2 * STATE_SIZE + i] == 0xa5);
    CHECK(commands == 0);
}

static void append_live(const char *text) {
    const char *path = getenv("HOS_INPUT_LIVE"); CHECK(path);
    FILE *file = fopen(path, "ab"); CHECK(file);
    CHECK(fputs(text, file) >= 0 && fclose(file) == 0);
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    CHECK(!pads_in_background(NULL, 0));   /* out of the box: the window in front only */
    CHECK(pads_in_background(NULL, 1));    /* the menu's row */
    CHECK(pads_in_background("1", 0));     /* HOS_INPUT_BACKGROUND=1 without the menu */
    CHECK(!pads_in_background("0", 1));    /* HOS_INPUT_BACKGROUND=0 wins over the menu */
    memory = calloc(512, 1); CHECK(memory);
    const char *name = argv[1];
    front = !strcmp(name, "foreground") || !strcmp(name, "exclusive-foreground");
    menu_background = !strcmp(name, "pads-menu") || !strcmp(name, "pads-off");
    const int exclusive = !strncmp(name, "exclusive-", 10);
    const int pads = !exclusive && (front || !strcmp(name, "pads-env") || !strcmp(name, "pads-menu"));
    const uint64_t physical = pads ? (1ULL << B_B) | (front ? 1ULL << B_X : 0) : 0;
    if (!strcmp(name, "live-background")) {
        packet(0, 0, 0, 0);
        append_live("0:A+LUP/200"); now_ns += 30000000;
        packet(0, 0, 0, 0); /* incomplete authored line is not consumed */
        append_live("\n"); now_ns += 30000000;
        packet(1ULL << B_A, 0, 32767, 0);
        now_ns += 200000000;
        packet(0, 0, 0, 0); /* consumed once, then released at the actual hold boundary */
    } else if (!strcmp(name, "no-script-background")) {
        packet(0, 0, 0, 0);
    } else {
        packet((1ULL << B_A) | physical, pads ? (front ? 32767 : -12000) : 0, 32767, pads ? 8000 : 0);
        now_ns += 201000000;
        packet(physical, pads ? (front ? 32767 : -12000) : 0, 0, pads ? 8000 : 0);
        if (exclusive) {
            CHECK(pad_merges == 0 && key_queries == 0 && mouse_queries == 0);
            now_ns += 5100000000LL;
            packet(front ? (1ULL << B_B) | (1ULL << B_X) : 0, front ? 32767 : 0, 0, front ? 8000 : 0);
        }
    }
    CHECK(bootstrap_polls == 1); /* production startup poll is allowed, but its Y/stick result is discarded */
    if (!front) {
        CHECK(key_queries == 0 && mouse_queries == 0);
        if (!pads) CHECK(pad_merges == 0);
    }
    if (!strcmp(name, "script-background")) CHECK(strstr(last_log, "game window not in front"));
    if (front) CHECK(pad_merges > 0 && key_queries > 0 && mouse_queries > 0);
    CHECK(focus_queries > 0);
    printf("PASS: actual scripted parse/merge/Npad route %s\n", name);
    free(memory);
    return 0;
}
