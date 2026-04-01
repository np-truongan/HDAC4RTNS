#pragma once

#include "types.h"

// ============================================================
//  Lightweight statistical probes.
//
//  Both functions operate on a raw byte window and are
//  designed to be cheap enough to run inline in a streaming
//  pipeline without dominating per-chunk latency.
// ============================================================

// Shannon entropy over the byte-frequency distribution.
// Returns a value in [0, 8].  8 = perfectly uniform (random).
double computeEntropy(const Chunk& data);

// Proportion of adjacent byte pairs whose absolute difference
// is <= 2.  Returns a value in [0, 1].  1 = perfectly smooth.
double computeSmoothness(const Chunk& data);

// Convenience wrapper: fills and returns a Features struct.
Features extractFeatures(const Chunk& data);
