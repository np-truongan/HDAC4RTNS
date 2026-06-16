#pragma once

#include "types.h"

struct EngineConfig {
    double entropyThreshold    = 6.5;
    double smoothnessThreshold = 0.7;
    double bitpackThreshold    = 4.0;
};

Decision decide(const Features& f, const EngineConfig& cfg = EngineConfig{});

// Returns the compression decision the engine would NOT have taken,
// used offline to measure the ratio cost of misclassification.
Decision alternativeDecision(const Features& f,
                              const EngineConfig& cfg,
                              const Decision& chosen);