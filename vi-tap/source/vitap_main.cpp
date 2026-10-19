/*
 * vi-tap - the always-on half of SysDVR-UVC's native capture
 * (docs/NATIVE-CAPTURE.md §5.1).
 *
 * A vi:u / vi:m mitm that records the running application's presents and
 * serves them as "uvctap". It forwards everything, holds nothing of the game
 * and keeps no state about its clients, so SysDVR-UVC (the capture half) can
 * be killed and restarted by ovlSysmodules at any time. vi-tap itself must
 * never be killed while a game runs: the game's display session goes through
 * it. It ships without toolbox.json for that reason.
 *
 * Install: atmosphere/contents/00FF0000A53BB667/exefs.nsp + flags/boot2.flag
 * Log:     sdmc:/config/sysdvr-uvc/vi-tap.log
 */
#include <stratosphere.hpp>
#include "vitap_log.hpp"
#include "vitap_present.hpp"
#include "vitap_tap_service.hpp"
#include "vitap_vi.hpp"

namespace ams {

    namespace {

        /* System resource-limit group: every custom sysmodule shares a few MB
         * of it with am (SFT #15, fatal 2001-0132 at boot). Keep it small. */
        constexpr size_t MallocBufferSize = 64_KB;
        alignas(os::MemoryPageSize) constinit u8 g_malloc_buffer[MallocBufferSize];

        constinit bool g_pmdmnt_ok = false;

        /* ---- the vi mitm server ------------------------------------------ */
        enum PortIndex {
            PortIndex_Viu,
            PortIndex_Vim,
            PortIndex_Count,
        };

        constexpr sm::ServiceName ViuServiceName = sm::ServiceName::Encode("vi:u");
        constexpr sm::ServiceName VimServiceName = sm::ServiceName::Encode("vi:m");

        struct ViServerOptions {
            static constexpr size_t PointerBufferSize   = 0x1000;
            static constexpr size_t MaxDomains          = 0;
            static constexpr size_t MaxDomainObjects    = 0;
            static constexpr bool CanDeferInvokeRequest = false;
            static constexpr bool CanManageMitmServers  = true;
        };

        /* a game, a homebrew app and their sub-objects (SFT v0.7.1: 8 was tight) */
        constexpr size_t ViMaxSessions = 16;

        class ViServerManager final : public sf::hipc::ServerManager<PortIndex_Count, ViServerOptions, ViMaxSessions> {
            private:
                virtual Result OnNeedsToAccept(int port_index, Server *server) override;
        };

        ViServerManager g_vi_server;

        Result ViServerManager::OnNeedsToAccept(int port_index, Server *server) {
            std::shared_ptr<::Service> fsrv;
            sm::MitmProcessInfo ci;
            server->AcknowledgeMitmSession(std::addressof(fsrv), std::addressof(ci));
            switch (port_index) {
                case PortIndex_Viu:
                    R_RETURN(this->AcceptMitmImpl(server, sf::CreateSharedObjectEmplaced<vitap::IViuRootMitm, vitap::ViuRootMitm>(decltype(fsrv)(fsrv), ci), fsrv));
                case PortIndex_Vim:
                    R_RETURN(this->AcceptMitmImpl(server, sf::CreateSharedObjectEmplaced<vitap::IVimRootMitm, vitap::VimRootMitm>(decltype(fsrv)(fsrv), ci), fsrv));
                AMS_UNREACHABLE_DEFAULT_CASE();
            }
        }

        /* Several threads: a forwarded call that blocks inside vi (a homebrew
         * app's dequeueBuffer while the keyboard applet takes the screen)
         * must not hold the only one, or the next client's ShouldMitm answer
         * never comes (SFT v0.7.1, the keyboard never opened). */
        constexpr size_t ViThreads = 4;     /* main + 3 */
        alignas(os::ThreadStackAlignment) constinit u8 g_vi_stacks[ViThreads - 1][16_KB];
        constinit os::ThreadType g_vi_threads[ViThreads - 1];

        void LoopProcessSafely(sf::hipc::ServerManagerBase &m) {
            /* a request we cannot handle is turned away, never fatal */
            while (auto *h = m.WaitSignaled()) {
                static_cast<void>(m.Process(h));
            }
        }

        void ViLoopThread(void *) { LoopProcessSafely(g_vi_server); }

        /* ---- uvctap: its own server and thread, so a client never queues
         * behind a vi call blocked in vi ------------------------------------- */
        constexpr sm::ServiceName TapServiceName = sm::ServiceName::Encode(UVCTAP_SERVICE_NAME);
        constexpr size_t TapMaxSessions = 4;

        sf::hipc::ServerManager<1, sf::hipc::DefaultServerManagerOptions, TapMaxSessions> g_tap_server;
        constinit sf::UnmanagedServiceObject<vitap::ITapService, vitap::TapService> g_tap_service;

        alignas(os::ThreadStackAlignment) constinit u8 g_tap_stack[8_KB];
        constinit os::ThreadType g_tap_thread;

        void TapLoopThread(void *) { LoopProcessSafely(g_tap_server); }

        void StartThread(os::ThreadType *t, os::ThreadFunction f, void *stack, size_t size, const char *name) {
            R_ABORT_UNLESS(os::CreateThread(t, f, nullptr, stack, size, os::GetThreadPriority(os::GetCurrentThread())));
            os::SetThreadNamePointer(t, name);
            os::StartThread(t);
        }

    }

    namespace init {

        void InitializeSystemModule() {
            R_ABORT_UNLESS(sm::Initialize());
            fs::InitializeForSystem();
            fs::SetEnabledAutoAbort(false);
            /* not R_ABORT_UNLESS: a boot-time fatal is the one failure this
             * module must never have. Without pm:dmnt vi:m wraps app-range
             * programs only and presents are not recorded. */
            g_pmdmnt_ok = R_SUCCEEDED(::pmdmntInitialize());
            /* no ams::CheckApiVersion(): it aborts on an Atmosphère older
             * than these headers, before the log is open (SFT v0.7.7) */
        }

        void FinalizeSystemModule() { /* ... */ }

        void Startup() {
            init::InitializeAllocator(g_malloc_buffer, sizeof(g_malloc_buffer));
        }

    }

    void Main() {
        os::SetThreadNamePointer(os::GetCurrentThread(), "vi-tap.Main");
        vitap::LogInit();
        {
            const auto api = exosphere::GetApiInfo();
            const u32 hv = hosversionGet();
            vitap::LogLine("vi-tap: protocol %u, Atmosphere %u.%u.%u, firmware %u.%u.%u (built with Atmosphere %u.%u.%u)", UVCTAP_VERSION,
                           api.GetMajorVersion(), api.GetMinorVersion(), api.GetMicroVersion(), HOSVER_MAJOR(hv), HOSVER_MINOR(hv), HOSVER_MICRO(hv),
                           ATMOSPHERE_RELEASE_VERSION_MAJOR, ATMOSPHERE_RELEASE_VERSION_MINOR, ATMOSPHERE_RELEASE_VERSION_MICRO);
        }

        vitap::InitializePresentStore();
        if (g_pmdmnt_ok) { vitap::StartAppWatch(); }
        else { vitap::LogLine("pm:dmnt unavailable - no application pid, presents are not recorded"); }

        /* uvctap first: a failure here must not leave a half-registered mitm */
        if (const Result rc = g_tap_server.RegisterObjectForServer(g_tap_service.GetShared(), TapServiceName, TapMaxSessions); R_FAILED(rc)) {
            vitap::LogLine("uvctap: register failed rc=0x%x - not starting the mitm", rc.GetValue());
            return;
        }
        StartThread(std::addressof(g_tap_thread), TapLoopThread, g_tap_stack, sizeof(g_tap_stack), "vi-tap.Tap");

        /* Another vi:u mitm installed (Switch Frame Tap...) makes this fail:
         * log and run without it rather than fatal at boot. */
        if (const Result rc = g_vi_server.RegisterMitmServer<vitap::ViuRootMitm>(PortIndex_Viu, ViuServiceName); R_FAILED(rc)) {
            vitap::LogLine("vi:u: mitm install failed rc=0x%x (another vi mitm installed?) - games will not be seen", rc.GetValue());
        } else {
            vitap::LogLine("vi:u: mitm installed");
        }
        if (const Result rc = g_vi_server.RegisterMitmServer<vitap::VimRootMitm>(PortIndex_Vim, VimServiceName); R_FAILED(rc)) {
            vitap::LogLine("vi:m: mitm install failed rc=0x%x - homebrew will not be seen", rc.GetValue());
        } else {
            vitap::LogLine("vi:m: mitm installed (applications only)");
        }

        for (size_t i = 0; i < ViThreads - 1; ++i) {
            StartThread(std::addressof(g_vi_threads[i]), ViLoopThread, g_vi_stacks[i], sizeof(g_vi_stacks[i]), "vi-tap.Vi");
        }
        LoopProcessSafely(g_vi_server);
    }

}
