#pragma once

#include "types.h"
#include <cstddef>

// ============================================================
//  Framing layer
//
//  Handles reliable send and receive of [FrameHeader + payload]
//  over a blocking TCP socket file descriptor.
//
//  Both functions do repeated read/write calls until the full
//  number of bytes is transferred — necessary because TCP is a
//  stream protocol and a single send/recv may return short.
//
//  Return value: true on success, false on any error or EOF.
// ============================================================

// Send a complete frame (header + compressed payload) to fd.
bool sendFrame(int fd,
               const FrameHeader& header,
               const Chunk& payload);

// Receive a complete frame from fd.
// Fills header and payload. Verifies magic number.
bool recvFrame(int fd,
               FrameHeader& header,
               Chunk& payload);

// ============================================================
//  Timestamp helper
//  Returns current wall-clock time in microseconds.
//  Used by sender to stamp each frame and by receiver to
//  compute end-to-end latency.
// ============================================================
uint64_t nowMicros();