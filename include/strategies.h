#pragma once

#include "types.h"

// ============================================================
//  Preprocessing transformations
//
//  Both functions are lossless and have inverse operations.
//  encode() must be paired with decode() before any data is
//  used — correctness tests in tests/test_strategies.cpp
//  verify round-trip fidelity for every encoder.
// ============================================================

// Delta encoding: store differences between adjacent bytes.
// Best applied to smoothly-varying data (telemetry).
Chunk deltaEncode(const Chunk& data);
Chunk deltaDecode(const Chunk& data);

// Bit-packing: pack pairs of 4-bit nibbles into single bytes.
// Requires that all values in `data` fit in [0, 15].
// Returns a chunk of ceil(data.size() / 2) bytes.
Chunk bitPackEncode(const Chunk& data);
Chunk bitPackDecode(const Chunk& encoded, size_t originalSize);

// ============================================================
//  Compression algorithms  (real library calls — no mocks)
//
//  Each function compresses `data` and returns the compressed
//  bytes.  Returns an empty Chunk on failure.
// ============================================================
Chunk compressLZ4 (const Chunk& data);
Chunk compressZSTD(const Chunk& data);
Chunk compressGZIP(const Chunk& data);

// ============================================================
//  Decompression (needed for correctness verification)
// ============================================================
Chunk decompressLZ4 (const Chunk& compressed, size_t originalSize);
Chunk decompressZSTD(const Chunk& compressed);
Chunk decompressGZIP(const Chunk& compressed, size_t originalSize);
