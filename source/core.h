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
    #define LOG(...) do { } while (0)
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

#define SYSDVR_UVC_VERSION "1.0.0"
#define SYSDVR_UVC_VERSION_NUM 0x010000

// ============================================================================
// Core API
// ============================================================================

/**
 * Initialize core subsystems (capture, USB)
 * @return Result code
 */
Result CoreInit(void);

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
