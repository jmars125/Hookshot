/* (linux) XInput is Windows' alone: no pad answers here, and every controller comes through SDL (host/pad_sdl.c). */
#ifndef HOOKSHOT_POSIX_XINPUT_H
#define HOOKSHOT_POSIX_XINPUT_H
#include <windows.h>
#define XUSER_MAX_COUNT 4
#define XINPUT_GAMEPAD_DPAD_UP 0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN 0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT 0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT 0x0008
#define XINPUT_GAMEPAD_START 0x0010
#define XINPUT_GAMEPAD_BACK 0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB 0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB 0x0080
#define XINPUT_GAMEPAD_LEFT_SHOULDER 0x0100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER 0x0200
#define XINPUT_GAMEPAD_A 0x1000
#define XINPUT_GAMEPAD_B 0x2000
#define XINPUT_GAMEPAD_X 0x4000
#define XINPUT_GAMEPAD_Y 0x8000
typedef struct {
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} XINPUT_GAMEPAD;
typedef struct { DWORD dwPacketNumber; XINPUT_GAMEPAD Gamepad; } XINPUT_STATE;
typedef struct { WORD wLeftMotorSpeed, wRightMotorSpeed; } XINPUT_VIBRATION;
static inline DWORD XInputGetState(DWORD user, XINPUT_STATE *s) { (void)user, (void)s; return ERROR_DEVICE_NOT_CONNECTED; }
static inline DWORD XInputSetState(DWORD user, XINPUT_VIBRATION *v) { (void)user, (void)v; return ERROR_DEVICE_NOT_CONNECTED; }
#endif
