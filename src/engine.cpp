#include "engine.h"

Decision decide(const Features& f, const EngineConfig& cfg) {
    Decision d;
    if (f.smoothness > cfg.smoothnessThreshold) {
        d.preprocess = Preprocess::DELTA;
    } else if (f.entropy < cfg.bitpackThreshold) {
        d.preprocess = Preprocess::BITPACK;
    } else {
        d.preprocess = Preprocess::NONE;
    }

    if (d.preprocess != Preprocess::NONE) {
        d.algorithm = Algorithm::ZSTD;
    } else {
        if (f.entropy > cfg.entropyThreshold)
            d.algorithm = Algorithm::LZ4; 
        else
            d.algorithm = Algorithm::ZSTD;
    }

    return d;
}

Decision alternativeDecision(const Features& f,
                              const EngineConfig& cfg,
                              const Decision& chosen)
{
    Decision alt;

    if (chosen.preprocess == Preprocess::DELTA) {
        alt.preprocess = Preprocess::NONE;
    } else if (chosen.preprocess == Preprocess::BITPACK) {
        alt.preprocess = Preprocess::NONE;
    } else {
        alt.preprocess = Preprocess::DELTA;
    }
    if (alt.preprocess != Preprocess::NONE) {
        alt.algorithm = Algorithm::ZSTD;
    } else {
        alt.algorithm = (f.entropy > cfg.entropyThreshold)
                        ? Algorithm::LZ4
                        : Algorithm::ZSTD;
    }

    if (alt.algorithm == chosen.algorithm &&
        alt.preprocess == chosen.preprocess)
    {
        alt.algorithm = (chosen.algorithm == Algorithm::LZ4)
                        ? Algorithm::ZSTD
                        : Algorithm::LZ4;
    }

    return alt;
}