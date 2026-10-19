#include "vitap_present.hpp"
#include "vitap_log.hpp"
#include <cstring>

namespace ams::vitap {

    constinit Counters g_counters = {};
    constinit std::atomic<u64> g_app_pid{0};

    namespace {

        /* IGraphicBufferProducer transaction codes */
        constexpr u32 Code_QueueBuffer = 7;
        constexpr u32 Code_SetPreallocatedBuffer = 14;

        constinit os::SdkMutex g_lock;
        constinit UvcTapPresent g_latest = {};
        constinit os::SystemEventType g_present_event = {};

        /* The parcel parsers follow Switch Frame Tap's applet_mitm_service.cpp
         * (GPL-2.0, Tomas Uribe), which established the layouts on hardware.
         *
         * A parcel starts with writeInterfaceToken: [u32 strict_mode_policy]
         * [u32 len][UTF-16 name, len+1 units, padded to 4] - for
         * "android.gui.IGraphicBufferProducer" the first argument lands at
         * parcel offset 96. Parsed rather than hardcoded. Returns the offset
         * of the first argument, or 0. */
        size_t FirstArgOffset(const u8 *p, size_t sz) {
            if (p == nullptr || sz < 24) { return 0; }
            u32 data_off = 0, len = 0;
            std::memcpy(std::addressof(data_off), p + 4, sizeof(data_off));
            if (static_cast<size_t>(data_off) + 8 > sz) { return 0; }
            std::memcpy(std::addressof(len), p + data_off + 4, sizeof(len));
            if (len == 0 || len > 128) { return 0; }
            size_t off = static_cast<size_t>(data_off) + 8 + (static_cast<size_t>(len) + 1) * 2;
            off = (off + 3) & ~static_cast<size_t>(3);
            return off + 4 <= sz ? off : 0;
        }

        s32 ParseSlot(const u8 *p, size_t sz) {
            const size_t off = FirstArgOffset(p, sz);
            if (off == 0) { return -1; }
            s32 slot = 0;
            std::memcpy(std::addressof(slot), p + off, sizeof(slot));
            return slot;
        }

        /* queueBuffer: after the slot, a flattened BqBufferInput (libnx
         * buffer_producer.h): int32 length, int32 fd count, then { s64
         * timestamp; s32 isAutoTimestamp; rect crop (+12); s32 scalingMode;
         * u32 transform (+32), stickyTransform, unk, swapInterval;
         * NvMultiFence fence (+48: u32 count, then 4x { u32 id; u32 value }) }. */
        bool ParseQueueInput(const u8 *p, size_t sz, UvcTapPresent *r) {
            size_t off = FirstArgOffset(p, sz);
            if (off == 0) { return false; }
            off += 4;   /* the slot */
            if (off + 8 + 84 > sz) { return false; }
            u32 flen = 0;
            std::memcpy(std::addressof(flen), p + off, sizeof(flen));
            if (flen < 84 || flen > 0x100) { return false; }
            const u8 *in = p + off + 8;
            u32 cnt = 0;
            std::memcpy(std::addressof(cnt), in + 48, sizeof(cnt));
            if (cnt > 4) { return false; }
            std::memcpy(std::addressof(r->transform), in + 32, 4);
            std::memcpy(r->crop, in + 12, 16);
            for (u32 k = 0; k < 4; ++k) {
                u32 id = 0, v = 0;
                std::memcpy(std::addressof(id), in + 52 + 8 * k, 4);
                std::memcpy(std::addressof(v), in + 56 + 8 * k, 4);
                r->fence[k] = k < cnt ? (static_cast<u64>(id) << 32) | v : 0;
            }
            r->fence_count = cnt;
            return true;
        }

        void NoteSlot(const sm::MitmProcessInfo &ci, s32 slot) {
            if (slot < 0 || slot >= 32) { return; }
            std::scoped_lock lk(g_lock);
            if (g_latest.process_id != ci.process_id.value) {
                g_latest.process_id = ci.process_id.value;
                g_latest.program_id = ci.program_id.value;
                g_latest.slot_mask = 0;
            }
            g_latest.slot_mask |= 1u << slot;
        }

    }

    void InitializePresentStore() {
        R_ABORT_UNLESS(os::CreateSystemEvent(std::addressof(g_present_event), os::EventClearMode_ManualClear, true));
    }

    void OnBinderTransaction(const sm::MitmProcessInfo &ci, u32 code, const void *parcel, size_t size) {
        const u8 *p = static_cast<const u8 *>(parcel);
        if (code == Code_SetPreallocatedBuffer) {
            if (IsRunningApplication(ci.process_id)) { NoteSlot(ci, ParseSlot(p, size)); }
            return;
        }
        if (code != Code_QueueBuffer) { return; }
        if (!IsRunningApplication(ci.process_id)) { g_counters.other_presents.fetch_add(1, std::memory_order_relaxed); return; }

        UvcTapPresent r = {};
        r.slot = ParseSlot(p, size);
        if (!ParseQueueInput(p, size, std::addressof(r))) {
            g_counters.parse_failures.fetch_add(1, std::memory_order_relaxed);
            r.fence_count = 0;
        }
        {
            std::scoped_lock lk(g_lock);
            if (g_latest.process_id != ci.process_id.value) { g_latest.slot_mask = 0; }
            r.seq = g_latest.seq + 1;
            r.process_id = ci.process_id.value;
            r.program_id = ci.program_id.value;
            r.tick = armGetSystemTick();
            r.slot_mask = g_latest.slot_mask | ((r.slot >= 0 && r.slot < 32) ? 1u << r.slot : 0);
            g_latest = r;
        }
        g_counters.presents.fetch_add(1, std::memory_order_relaxed);
        os::SignalSystemEvent(std::addressof(g_present_event));
    }

    void GetLatestPresent(UvcTapPresent *out) {
        std::scoped_lock lk(g_lock);
        *out = g_latest;
    }

    void GetStats(UvcTapStats *out) {
        out->app_pid          = g_app_pid.load(std::memory_order_relaxed);
        out->displays_wrapped = g_counters.displays.load(std::memory_order_relaxed);
        out->binders_wrapped  = g_counters.binders.load(std::memory_order_relaxed);
        out->presents         = g_counters.presents.load(std::memory_order_relaxed);
        out->parse_failures   = g_counters.parse_failures.load(std::memory_order_relaxed);
        out->other_presents   = g_counters.other_presents.load(std::memory_order_relaxed);
    }

    os::NativeHandle GetPresentEventHandle() {
        return os::GetReadableHandleOfSystemEvent(std::addressof(g_present_event));
    }

    namespace {

        alignas(os::ThreadStackAlignment) constinit u8 g_watch_stack[8_KB];
        constinit os::ThreadType g_watch_thread;

        void AppWatchThread(void *) {
            u64 last_pid = ~UINT64_C(0), last_presents = 0, last_log_ms = 0;
            for (;;) {
                os::ProcessId pid{};
                const u64 now = R_SUCCEEDED(pm::dmnt::GetApplicationProcessId(std::addressof(pid))) ? pid.value : 0;
                g_app_pid.store(now, std::memory_order_relaxed);

                const u64 ms = armTicksToNs(armGetSystemTick()) / 1000000;
                const u64 presents = g_counters.presents.load(std::memory_order_relaxed);
                if (now != last_pid) {
                    LogLine("application pid %llu", static_cast<unsigned long long>(now));
                    last_pid = now;
                }
                /* every 10 s while frames come in: rate, last record, counters.
                 * No %f: newlib's float formatting allocates, and a stratosphere
                 * module has no newlib heap (SFT sound probe 2 fatal). */
                if (ms - last_log_ms >= 10000 && presents != last_presents) {
                    UvcTapPresent r;
                    GetLatestPresent(std::addressof(r));
                    const u64 dt = ms - last_log_ms;
                    const u64 rate10 = last_log_ms == 0 ? 0 : (presents - last_presents) * 10000 / dt;
                    LogLine("presents %llu (%llu.%llu/s) program %016llx slot %d mask 0x%x fences %u xf 0x%x crop (%d,%d)-(%d,%d) | wrapped %llu/%llu, parse fail %llu, other %llu",
                            static_cast<unsigned long long>(presents),
                            static_cast<unsigned long long>(rate10 / 10), static_cast<unsigned long long>(rate10 % 10),
                            static_cast<unsigned long long>(r.program_id), r.slot, r.slot_mask, r.fence_count, r.transform,
                            r.crop[0], r.crop[1], r.crop[2], r.crop[3],
                            static_cast<unsigned long long>(g_counters.displays.load()), static_cast<unsigned long long>(g_counters.binders.load()),
                            static_cast<unsigned long long>(g_counters.parse_failures.load()), static_cast<unsigned long long>(g_counters.other_presents.load()));
                    last_presents = presents;
                    last_log_ms = ms;
                }
                os::SleepThread(TimeSpan::FromMilliSeconds(100));
            }
        }

    }

    void StartAppWatch() {
        R_ABORT_UNLESS(os::CreateThread(std::addressof(g_watch_thread), AppWatchThread, nullptr, g_watch_stack, sizeof(g_watch_stack),
                                        os::GetThreadPriority(os::GetCurrentThread())));
        os::SetThreadNamePointer(std::addressof(g_watch_thread), "vi-tap.Watch");
        os::StartThread(std::addressof(g_watch_thread));
    }

}
