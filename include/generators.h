#pragma once

#include "types.h"
#include <cstddef>

// ============================================================
//  Three workload generators used throughout all benchmarks.
//
//  Each returns a Chunk of exactly `size` bytes with the
//  statistical profile described below.
// ============================================================

// High smoothness (~0.99), low entropy (~2–3 bits).
// Models sensor readings with small incremental changes.
Chunk generateTelemetry(size_t size, unsigned seed = 42);

// Moderate entropy (~3–4 bits), high repetition.
// Models JSON-like messages with repeated field names/structure.
Chunk generateJSON(size_t size);

// High entropy (~7.9 bits), near-uniform byte distribution.
// Models binary log data or encrypted payloads.
Chunk generateBinary(size_t size, unsigned seed = 123);

// Low entropy (~2–3 bits), all values in [0, 15].
// Models 4-bit sensor readings or quantized telemetry.
// This is the canonical input for bit-packing preprocessing.
Chunk generateNibble(size_t size, unsigned seed = 77);