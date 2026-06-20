#pragma once

#include "types.h"

struct EngineConfig {
    double entropyThreshold    = 6.5;
    double smoothnessThreshold = 0.7;
    double bitpackThreshold    = 4.0;
};

Decision decide(const Features& f, const EngineConfig& cfg = EngineConfig{});

Decision alternativeDecision(const Features& f,
                              const EngineConfig& cfg,
                              const Decision& chosen);