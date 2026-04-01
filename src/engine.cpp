#include "engine.h"

// ============================================================
//  Decision engine
//
//  Rules (in priority order):
//
//  1. High smoothness → Delta preprocessing.
//     Rationale: strong sequential correlation means delta
//     encoding reduces entropy significantly before compression.
//
//  2. Very low entropy → BitPack preprocessing eligible.
//     Rationale: small value ranges benefit from packing.
//     NOTE: caller must verify the value-range precondition
//     before actually invoking bitPackEncode().
//
//  3. Preprocessing-aware algorithm selection:
//     - If any preprocessing is applied, ZSTD is preferred
//       because the transformed data has increased redundancy
//       that ZSTD exploits better than LZ4.
//     - Without preprocessing, high entropy → LZ4 (fast,
//       minimal overhead for near-incompressible data),
//       low entropy → ZSTD (better ratio worth the cost).
//
//  This ordering addresses the Week 4 limitation where
//  preprocessing and algorithm selection were independent.
// ============================================================
Decision decide(const Features& f, const EngineConfig& cfg) {
    Decision d;

    // Step 1: determine preprocessing
    if (f.smoothness > cfg.smoothnessThreshold) {
        d.preprocess = Preprocess::DELTA;
    } else if (f.entropy < cfg.bitpackThreshold) {
        d.preprocess = Preprocess::BITPACK;
    } else {
        d.preprocess = Preprocess::NONE;
    }

    // Step 2: select algorithm, accounting for preprocessing
    if (d.preprocess != Preprocess::NONE) {
        // preprocessing increases redundancy → ZSTD exploits it better
        d.algorithm = Algorithm::ZSTD;
    } else {
        // no preprocessing: route on raw entropy
        if (f.entropy > cfg.entropyThreshold)
            d.algorithm = Algorithm::LZ4;   // fast path for high-entropy
        else
            d.algorithm = Algorithm::ZSTD;  // ratio path for low-entropy
    }

    return d;
}
