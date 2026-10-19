/*
 * vi-tap log: sdmc:/config/sysdvr-uvc/vi-tap.log (previous boot kept as
 * vi-tap.prev.log), capped at 1 MB. Only in a logging build
 * (VITAP_LOG=1 tools/build-vi-tap.sh); otherwise LogInit/LogLine do nothing.
 *
 * NEVER call LogLine from an IPC handler (vi or uvctap server threads): fs is
 * IPC too, and it reuses the thread's TLS message buffer the handler still
 * needs for its reply (SFT v0.7.10: a game's vi call never returned and
 * nn::vi aborted). Handlers only bump counters; the watch thread logs.
 */
#pragma once
#include <stratosphere.hpp>

namespace ams::vitap {

    void LogInit();
    void LogLine(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

}
