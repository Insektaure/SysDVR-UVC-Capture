#include "vitap_log.hpp"
#include <cstdarg>
#include <cstdio>

namespace ams::vitap {

    namespace {

        constexpr const char *Dir      = "sdmc:/config/sysdvr-uvc";
        constexpr const char *LogPath  = "sdmc:/config/sysdvr-uvc/vi-tap.log";
        constexpr const char *PrevPath = "sdmc:/config/sysdvr-uvc/vi-tap.prev.log";
        constexpr s64 LogCap = 1024 * 1024;

        constinit os::SdkMutex g_lock;
        constinit bool g_ready = false;

    }

    void LogInit() {
        std::scoped_lock lk(g_lock);
        if (R_FAILED(fs::MountSdCard("sdmc"))) { return; }
        static_cast<void>(fs::CreateDirectory("sdmc:/config"));
        static_cast<void>(fs::CreateDirectory(Dir));
        static_cast<void>(fs::DeleteFile(PrevPath));
        static_cast<void>(fs::RenameFile(LogPath, PrevPath));
        g_ready = R_SUCCEEDED(fs::CreateFile(LogPath, 0));
    }

    void LogLine(const char *fmt, ...) {
        char line[256];
        const u64 ms = armTicksToNs(armGetSystemTick()) / 1000000;
        int n = std::snprintf(line, sizeof(line), "[%6llu.%03llu] ", static_cast<unsigned long long>(ms / 1000), static_cast<unsigned long long>(ms % 1000));
        std::va_list va;
        va_start(va, fmt);
        n += std::vsnprintf(line + n, sizeof(line) - n - 1, fmt, va);
        va_end(va);
        if (n > static_cast<int>(sizeof(line)) - 2) { n = sizeof(line) - 2; }
        line[n++] = '\n';

        std::scoped_lock lk(g_lock);
        if (!g_ready) { return; }
        fs::FileHandle f;
        if (R_FAILED(fs::OpenFile(std::addressof(f), LogPath, fs::OpenMode_Write | fs::OpenMode_AllowAppend))) {
            /* deleted while we run (vi-tap is never restarted): start a new one */
            static_cast<void>(fs::CreateDirectory("sdmc:/config"));
            static_cast<void>(fs::CreateDirectory(Dir));
            if (R_FAILED(fs::CreateFile(LogPath, 0))) { return; }
            if (R_FAILED(fs::OpenFile(std::addressof(f), LogPath, fs::OpenMode_Write | fs::OpenMode_AllowAppend))) { return; }
        }
        s64 size = 0;
        static_cast<void>(fs::GetFileSize(std::addressof(size), f));
        if (size < LogCap) { static_cast<void>(fs::WriteFile(f, size, line, n, fs::WriteOption::Flush)); }
        fs::CloseFile(f);
    }

}
