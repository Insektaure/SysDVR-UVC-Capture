#pragma once
#include <switch.h>

// ============================================================================
// Logging Configuration
// ============================================================================

// Enable via make DEFINES="-DUDP_LOGGING" or "-DFILE_LOGGING"
#define LOGGING_ENABLED (UDP_LOGGING || FILE_LOGGING)

#if LOGGING_ENABLED
    void LogFunctionImpl(const char* fmt, ...);

    #define LOG(...) do { LogFunctionImpl(__VA_ARGS__); } while (0)
    #define LOGGING_STACK_BOOST 0x1000
#else
    // Arguments still evaluated (and so "used"): code that computes values
    // only for its log lines compiles the same without logging. Cheap, and
    // the optimizer drops what has no side effect.
    static inline void LogNop(const char* fmt, ...) { (void)fmt; }
    #define LOG(...) LogNop(__VA_ARGS__)
    #define LOGGING_STACK_BOOST 0
#endif

// ============================================================================
// Helper Macros
// ============================================================================

#define R_RET_ON_FAIL(x) do { Result rc = x; if (R_FAILED(rc)) return rc; } while (0)
#define R_THROW(x) do { Result r = x; if (R_FAILED(r)) { fatalThrow(r); } } while(0)

// ============================================================================
// Version Information
// ============================================================================

#define SYSDVR_UVC_VERSION "1.0.1"
#define SYSDVR_UVC_VERSION_NUM 0x010001

// ============================================================================
// Core API
// ============================================================================

/**
 * Initialize core subsystems (capture, USB)
 * @return Result code
 */
Result CoreInit(void);

/**
 * Open the log (FILE_LOGGING) and say the process started, before the
 * startup wait. Needs sm. No-op without logging.
 */
void CoreEarlyLog(void);

/**
 * Cleanup core subsystems
 */
void CoreExit(void);

/**
 * Launch a thread with the specified parameters
 */
void LaunchThread(Thread* t, ThreadFunc f, void* arg, void* stackLocation, u32 stackSize, u32 prio);

/**
 * Wait for a thread to complete
 */
void JoinThread(Thread* t);

/**
 * Get the console serial number
 */
const char* CoreGetSerialNumber(void);

/**
 * Whether a service is registered, without blocking: smGetService on an
 * unregistered name waits forever under Atmosphère (sm defers the request
 * until the service registers). Uses Atmosphère's AtmosphereHasService.
 */
bool CoreHasService(const char* name);
