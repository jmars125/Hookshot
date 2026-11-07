/* (linux) waveOut, the audio renderer's output (host/nn_audio.c), on SDL's audio (win32_posix.c): the blocks written
   play in order and are marked WHDR_DONE as the device takes them, as on Windows. */
#ifndef HOOKSHOT_POSIX_MMSYSTEM_H
#define HOOKSHOT_POSIX_MMSYSTEM_H
#include <windows.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef UINT MMRESULT;
typedef void *HWAVEOUT;
#define MMSYSERR_NOERROR 0
#define MMSYSERR_ERROR 1
#define WAVE_MAPPER ((UINT)-1)
#define WAVE_FORMAT_PCM 1
#define CALLBACK_NULL 0
#define WHDR_DONE 0x1
#define WHDR_PREPARED 0x2
#define WHDR_INQUEUE 0x10
typedef struct {
    WORD wFormatTag, nChannels;
    DWORD nSamplesPerSec, nAvgBytesPerSec;
    WORD nBlockAlign, wBitsPerSample, cbSize;
} WAVEFORMATEX;
typedef struct wavehdr_tag {
    LPSTR lpData;
    DWORD dwBufferLength, dwBytesRecorded;
    DWORD_PTR dwUser;
    volatile DWORD dwFlags;
    DWORD dwLoops;
    struct wavehdr_tag *lpNext;
    DWORD_PTR reserved;
} WAVEHDR;
MMRESULT waveOutOpen(HWAVEOUT *out, UINT device, const WAVEFORMATEX *f, DWORD_PTR cb, DWORD_PTR inst, DWORD flags);
MMRESULT waveOutPrepareHeader(HWAVEOUT w, WAVEHDR *h, UINT size);
MMRESULT waveOutUnprepareHeader(HWAVEOUT w, WAVEHDR *h, UINT size);
MMRESULT waveOutWrite(HWAVEOUT w, WAVEHDR *h, UINT size);
MMRESULT waveOutReset(HWAVEOUT w);
MMRESULT waveOutClose(HWAVEOUT w);
#ifdef __cplusplus
}
#endif
#endif
