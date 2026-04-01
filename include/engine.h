#pragma once

#include "types.h"

// ============================================================
//  Threshold configuration
//
//  All decision boundaries live here in one place so the
//  sensitivity analysis benchmark can sweep them cleanly
//  without touching engine logic.
// ============================================================
struct EngineConfig {
    double entropyThreshold    = 6.5;  // above → LZ4, below → ZSTD
    double smoothnessThreshold = 0.7;  // above → apply Delta
    double bitpackThreshold    = 4.0;  // entropy below this → BitPack eligible
};

// ============================================================
//  Decision engine
//
//  Maps (Features, EngineConfig) → Decision.
//  The decision is preprocessing-aware: if delta or bitpack
//  is selected, the algorithm choice accounts for the fact
//  that preprocessing increases redundancy.
// ============================================================
Decision decide(const Features& f, const EngineConfig& cfg = EngineConfig{});
