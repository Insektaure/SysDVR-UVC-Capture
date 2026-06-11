#pragma once
#include <switch.h>

// Mostly taken from libnx but edited to allow multiple sessions to grc:d

/// Initialize grc:d.
Result grcdServiceOpen(Service* out);

/// Exit grc:d.
void grcdServiceClose(Service* svc);

/// Begins streaming. This must not be called more than once, even from a different service session.
Result grcdServiceBegin(Service* svc);

/**
 * @brief Retrieves stream data from the continuous recorder (video/audio from the running game).
 * @note This will block until data is available. Hangs if no game with video capture is running.
 * @param[in] stream GrcStream (video or audio)
 * @param[out] buffer Output buffer
 * @param[in] size Max size of output buffer
 * @param[out] num_frames Number of frames (optional)
 * @param[out] data_size Actual output data size
 * @param[out] start_timestamp Start timestamp
 */
Result grcdServiceTransfer(Service* srv, GrcStream stream, void* buffer, size_t size,
                           u32* num_frames, u32* data_size, u64* start_timestamp);
