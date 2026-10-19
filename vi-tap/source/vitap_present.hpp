/*
 * The present store: what the running application's binder traffic says
 * about its last frame, for the uvctap service. Written from the vi server
 * threads (cheap parse, a short lock, no IPC), read by uvctap.
 */
#pragma once
#include <stratosphere.hpp>
#include <atomic>
#include "uvctap.h"

namespace ams::vitap {

    struct Counters {
        std::atomic<u64> displays{0}, binders{0}, presents{0}, parse_failures{0}, other_presents{0};
    };
    extern Counters g_counters;

    /* the running application per pm:dmnt, refreshed every 100 ms by the
     * watch thread. ShouldMitm must never ask pm itself: sm waits on our
     * answer synchronously and pm may be waiting on sm (deadlock, SFT v0.1.1). */
    extern std::atomic<u64> g_app_pid;

    inline bool IsRunningApplication(os::ProcessId pid) {
        const u64 ap = g_app_pid.load(std::memory_order_relaxed);
        return ap != 0 && ap == pid.value;
    }

    void InitializePresentStore();

    /* one IHOSBinderDriver transaction of a wrapped process */
    void OnBinderTransaction(const sm::MitmProcessInfo &ci, u32 code, const void *parcel, size_t size);

    void GetLatestPresent(UvcTapPresent *out);
    void GetStats(UvcTapStats *out);
    os::NativeHandle GetPresentEventHandle();

    /* the pm:dmnt watcher; also writes the periodic log lines */
    void StartAppWatch();

}
