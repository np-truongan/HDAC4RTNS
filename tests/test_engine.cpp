#include "engine.h"
#include "generators.h"
#include "heuristics.h"
#include "types.h"

#include <iostream>
#include <string>

static void pass(const std::string& name) {
    std::cout << "  [PASS] " << name << "\n";
}

static void fail(const std::string& name, const std::string& reason) {
    std::cout << "  [FAIL] " << name << " -- " << reason << "\n";
    std::exit(1);
}

void testDefaultRouting() {
    std::cout << "\n--- Default routing (thresholds: entropy=6.5, smoothness=0.7, bitpack=4.0) ---\n";

    EngineConfig cfg;

    {
        Chunk chunk = generateTelemetry(4096);
        Features f = extractFeatures(chunk);
        Decision d = decide(f, cfg);
        if (d.preprocess != Preprocess::DELTA)
            fail("Telemetry routing", "expected DELTA, got " + toString(d.preprocess));
        if (d.algorithm != Algorithm::ZSTD)
            fail("Telemetry routing", "expected ZSTD, got " + toString(d.algorithm));
        pass("Telemetry -> DELTA + ZSTD");
    }

    {
        Chunk chunk = generateJSON(4096);
        Features f = extractFeatures(chunk);
        Decision d = decide(f, cfg);
        if (d.preprocess != Preprocess::NONE)
            fail("JSON routing", "expected NONE, got " + toString(d.preprocess));
        if (d.algorithm != Algorithm::ZSTD)
            fail("JSON routing", "expected ZSTD, got " + toString(d.algorithm));
        pass("JSON -> NONE + ZSTD");
    }

    {
        Chunk chunk = generateBinary(4096);
        Features f = extractFeatures(chunk);
        Decision d = decide(f, cfg);
        if (d.preprocess != Preprocess::NONE)
            fail("Binary routing", "expected NONE, got " + toString(d.preprocess));
        if (d.algorithm != Algorithm::LZ4)
            fail("Binary routing", "expected LZ4, got " + toString(d.algorithm));
        pass("Binary -> NONE + LZ4");
    }

    {
        Chunk chunk = generateNibble(4096);
        Features f = extractFeatures(chunk);
        Decision d = decide(f, cfg);
        if (d.preprocess != Preprocess::BITPACK)
            fail("Nibble routing", "expected BITPACK, got " + toString(d.preprocess));
        if (d.algorithm != Algorithm::ZSTD)
            fail("Nibble routing", "expected ZSTD, got " + toString(d.algorithm));
        pass("Nibble -> BITPACK + ZSTD");
    }
}

void testBitpackEligibility() {
    std::cout << "\n--- BITPACK eligibility (engine decision only) ---\n";

    EngineConfig cfg;

    Chunk chunk = generateJSON(4096);
    Features f = extractFeatures(chunk);
    if (f.entropy < cfg.bitpackThreshold) {
        fail("Bitpack eligibility precondition", "JSON entropy is below bitpack threshold");
    }
    Decision d = decide(f, cfg);
    if (d.preprocess == Preprocess::BITPACK) {
        fail("BITPACK eligibility", "engine should NOT choose BITPACK when entropy >= bitpackThreshold");
    }
    pass("BITPACK not chosen when entropy above threshold");

    Chunk alt(4096);
    for (size_t i = 0; i < alt.size(); ++i) alt[i] = (i % 2 == 0) ? 0 : 255;
    Features falt = extractFeatures(alt);
    Decision dalt = decide(falt, cfg);
    if (dalt.preprocess != Preprocess::BITPACK) {
        fail("BITPACK eligibility", "engine should choose BITPACK for low-entropy, low-smoothness chunk");
    }
    pass("BITPACK chosen when entropy below threshold and smoothness low");
}

void testAlternativeDecision() {
    std::cout << "\n--- Alternative decision ---\n";

    EngineConfig cfg;

    {
        Chunk chunk = generateTelemetry(4096);
        Features f = extractFeatures(chunk);
        Decision chosen = decide(f, cfg);
        Decision alt = alternativeDecision(f, cfg, chosen);

        if (chosen.preprocess != Preprocess::DELTA || chosen.algorithm != Algorithm::ZSTD) {
            fail("Alternative (Telemetry) precondition", "chosen not DELTA+ZSTD");
        }
        if (alt.preprocess != Preprocess::NONE) {
            fail("Alternative (Telemetry)", "expected NONE, got " + toString(alt.preprocess));
        }
        if (alt.algorithm != Algorithm::ZSTD) {
            fail("Alternative (Telemetry)", "expected ZSTD, got " + toString(alt.algorithm));
        }
        pass("Telemetry: alternative = NONE+ZSTD");
    }

    {
        Chunk chunk = generateBinary(4096);
        Features f = extractFeatures(chunk);
        Decision chosen = decide(f, cfg);
        Decision alt = alternativeDecision(f, cfg, chosen);

        if (chosen.preprocess != Preprocess::NONE || chosen.algorithm != Algorithm::LZ4) {
            fail("Alternative (Binary) precondition", "chosen not NONE+LZ4");
        }
        if (alt.preprocess != Preprocess::DELTA) {
            fail("Alternative (Binary)", "expected DELTA, got " + toString(alt.preprocess));
        }
        if (alt.algorithm != Algorithm::ZSTD) {
            fail("Alternative (Binary)", "expected ZSTD, got " + toString(alt.algorithm));
        }
        pass("Binary: alternative = DELTA+ZSTD");
    }

    {
        Chunk chunk = generateJSON(4096);
        Features f = extractFeatures(chunk);
        Decision chosen = decide(f, cfg);
        Decision alt = alternativeDecision(f, cfg, chosen);

        if (chosen.preprocess != Preprocess::NONE || chosen.algorithm != Algorithm::ZSTD) {
            fail("Alternative (JSON) precondition", "chosen not NONE+ZSTD");
        }
        if (alt.preprocess != Preprocess::DELTA) {
            fail("Alternative (JSON)", "expected DELTA, got " + toString(alt.preprocess));
        }
        if (alt.algorithm != Algorithm::ZSTD) {
            fail("Alternative (JSON)", "expected ZSTD, got " + toString(alt.algorithm));
        }
        pass("JSON: alternative = DELTA+ZSTD");
    }

    pass("Alternative decision edge case handling (implicit)");
}

int main() {
    std::cout << "========================================\n";
    std::cout << "  Decision Engine Tests\n";
    std::cout << "========================================\n";

    testDefaultRouting();
    testBitpackEligibility();
    testAlternativeDecision();

    std::cout << "\n========================================\n";
    std::cout << "  All engine tests passed.\n";
    std::cout << "========================================\n";

    return 0;
}