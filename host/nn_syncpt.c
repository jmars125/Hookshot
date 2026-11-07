/* nn::syncpt::sdk: GPU sync points (the console's host1x syncpoints) and the events that wait for them. The SDK and
   the game's movie library import them from a system library the image does not carry, so their calls went to
   address 0. There is no GPU timeline here to wait on (the native NVN layer completes work in order), so every sync
   point counts as reached: a sync point event is a system event, signaled when it is requested. */
#include "hos.h"

void hos_system_event_init(uint64_t key, int autoclear);   /* nn_os_sync.c */
void hos_system_event_signal(uint64_t key);
void imp__ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_15SystemEventTypeE(Ctx *c);

HLE(_ZN2nn6syncpt17GetLibraryVersionEv) { RET(1); }
HLE(_ZN2nn6syncpt3sdk12InitializeExEPvmNS1_14LibraryOptionsE) { RET(0); }

/* sync points: reached */
HLE(_ZN2nn6syncpt3sdk13WaitSyncPointERKNS1_15SyncPointHandleE) { RET(0); }
HLE(_ZN2nn6syncpt3sdk16TryWaitSyncPointERKNS1_15SyncPointHandleE) { RET(1); }
HLE(_ZN2nn6syncpt3sdk18TimedWaitSyncPointERKNS1_15SyncPointHandleENS_8TimeSpanE) { RET(1); }

/* sync point events: system events (the object's storage is the game's) */
HLE(_ZN2nn6syncpt3sdk20CreateSyncPointEventEPNS1_18SyncPointEventTypeE) {
    hos_system_event_init(ARG(0), 0);
    RET(0);
}
HLE(_ZN2nn6syncpt3sdk21DestroySyncPointEventEPNS1_18SyncPointEventTypeE) {}
/* (hookshot) except for the handle nn_video.c gives a buffer an idle video decoder will not fill: its event is
   signalled at the next submission (hos_video_sync_defer). A cancelled event is cleared, as a new request starts over:
   left signalled, the request after it was signalled already, whatever its handle. */
int hos_video_sync_defer(uint64_t event, uint64_t handle);
void hos_video_sync_cancel(uint64_t event);
int hos_video_sync_waiting(uint64_t event);
void imp__ZN2nn2os16ClearSystemEventEPNS0_15SystemEventTypeE(Ctx *c);
HLE(_ZN2nn6syncpt3sdk21RequestSyncPointEventEPNS1_18SyncPointEventTypeERKNS1_15SyncPointHandleE) {
    if (!hos_video_sync_defer(ARG(0), ARG(1) ? G64(ARG(1)) : 0)) hos_system_event_signal(ARG(0));
    RET(0);
}
HLE(_ZN2nn6syncpt3sdk20CancelSyncPointEventEPNS1_18SyncPointEventTypeE) {
    hos_video_sync_cancel(ARG(0));
    imp__ZN2nn2os16ClearSystemEventEPNS0_15SystemEventTypeE(c);   /* (ARG(0): the same event) */
}
HLE(_ZN2nn6syncpt3sdk21TryWaitSyncPointEventEPKNS1_18SyncPointEventTypeE) { RET(!hos_video_sync_waiting(ARG(0))); }
/* InitializeMultiWaitHolder(holder, SyncPointEventType*): as for a system event */
HLE(_ZN2nn6syncpt3sdk25InitializeMultiWaitHolderEPNS_2os19MultiWaitHolderTypeEPNS1_18SyncPointEventTypeE) {
    imp__ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_15SystemEventTypeE(c);
}

/* conversions between sync point handles, NvRm fences and native sync handles: a handle holds nothing to convert */
HLE(_ZN2nn6syncpt3sdk32MakeNvRmFenceFromSyncPointHandleEPjS2_RKNS1_15SyncPointHandleE) {
    if (ARG(0)) G32(ARG(0)) = 0;
    if (ARG(1)) G32(ARG(1)) = 0;
}
HLE(_ZN2nn6syncpt3sdk32MakeSyncPointHandleFromNvRmFenceEjj) { RET(0); }
HLE(_ZN2nn6syncpt3sdk39MakeNativeSyncHandleFromSyncPointHandleERKNS1_15SyncPointHandleE) { RET(0); }
