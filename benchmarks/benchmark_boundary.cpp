// benchmark_boundary.cpp
// Evaluates engine behaviour at the smooth/noisy boundary.
// Run AFTER engine.cpp thresholds are fixed.
// NOTE: do NOT use this generator during threshold calibration.

#include "generators.h"
#include "heuristics.h"
#include "strategies.h"
#include "engine.h"
#include "stats.h"

#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <chrono>

static constexpr size_t   CHUNK_SIZE  = 4096;
static constexpr unsigned FIXED_SEED  = 42;
static constexpr size_t   DATA_SIZE   = 1UL << 20;   // 1 MB

// ============================================================
//  Time a compression decision end-to-end, return latency in µs
// ============================================================
static double measureLatencyUs(const Chunk& data, const Decision& d) {
    auto t0 = std::chrono::high_resolution_clock::now();
    compressWithDecision(data, d);
    auto t1 = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::micro>(t1 - t0).count();
}

// ============================================================
//  Main
// ============================================================
int main() {
    std::cout << "========================================\n";
    std::cout << "  Benchmark: Boundary Telemetry\n";
    std::cout << "========================================\n\n";

    EngineConfig cfg{};   // default thresholds: entropy=6.5, smooth=0.7

    float noise_levels[] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f};

    std::ofstream csv("results/boundary_results.csv");
    csv << "noise_scale,smoothness,entropy,"
           "algorithm,preprocess,"
           "chosen_ratio,alt_ratio,ratio_delta,"
           "chosen_latency_us,alt_latency_us\n";

    std::cout << std::fixed << std::setprecision(4);
    std::cout << std::left  << std::setw(12) << "Noise"
              << std::setw(12) << "Smoothness"
              << std::setw(10) << "Entropy"
              << std::setw(10) << "Algorithm"
              << std::setw(10) << "Preprocess"
              << std::setw(14) << "ChosenRatio"
              << std::setw(14) << "AltRatio"
              << std::setw(14) << "LatChosen(µs)"
              << std::setw(14) << "LatAlt(µs)"
              << "\n";
    std::cout << std::string(108, '-') << "\n";

    for (float noise : noise_levels) {
        Chunk data = generateBoundaryTelemetry(DATA_SIZE, FIXED_SEED, noise);

        // Use first chunk only for per-decision comparison
        Chunk chunk(data.begin(), data.begin() + CHUNK_SIZE);

        Features f = extractFeatures(chunk);
        Decision d = decide(f, cfg);
        Decision alt = alternativeDecision(f, cfg, d);

        CompressResult chosen = compressWithDecision(chunk, d);
        CompressResult altRes = compressWithDecision(chunk, alt);

        double chosenLat = measureLatencyUs(chunk, d);
        double altLat    = measureLatencyUs(chunk, alt);

        std::cout << std::setw(12) << noise
                  << std::setw(12) << f.smoothness
                  << std::setw(10) << f.entropy
                  << std::setw(10) << toString(d.algorithm)
                  << std::setw(10) << toString(d.preprocess)
                  << std::setw(14) << chosen.ratio
                  << std::setw(14) << altRes.ratio
                  << std::setw(14) << chosenLat
                  << std::setw(14) << altLat
                  << "\n";

        csv << noise            << ","
            << f.smoothness     << ","
            << f.entropy        << ","
            << toString(d.algorithm)   << ","
            << toString(d.preprocess)  << ","
            << chosen.ratio     << ","
            << altRes.ratio     << ","
            << (altRes.ratio - chosen.ratio) << ","
            << chosenLat        << ","
            << altLat           << "\n";
    }

    std::cout << "\nResults saved to results/boundary_results.csv\n";

    // --------------------------------------------------------
    //  Sensitivity extension (see comment block in original):
    //  report what fraction of (entropy, smoothness) threshold
    //  combinations change the Boundary workload's routing.
    // --------------------------------------------------------
    std::cout << "\n--- Threshold sensitivity for Boundary workload ---\n";

    std::vector<double> entropyThresholds  = {5.5, 6.0, 6.5, 7.0, 7.5};
    std::vector<double> smoothThresholds   = {0.5, 0.6, 0.7, 0.8, 0.9};
    static constexpr double BITPACK_THRESH = 4.0;

    // Use mid-range noise (0.3) as the representative boundary chunk
    Chunk boundaryData = generateBoundaryTelemetry(DATA_SIZE, FIXED_SEED, 0.3f);
    Chunk boundaryChunk(boundaryData.begin(),
                        boundaryData.begin() + CHUNK_SIZE);
    Features bf = extractFeatures(boundaryChunk);

    int total = 0, changed = 0;
    Decision baseline = decide(bf, cfg);

    for (double et : entropyThresholds) {
        for (double st : smoothThresholds) {
            EngineConfig sweep{et, st, BITPACK_THRESH};
            Decision swept = decide(bf, sweep);
            if (swept.algorithm != baseline.algorithm ||
                swept.preprocess != baseline.preprocess)
                ++changed;
            ++total;
        }
    }

    std::cout << "Baseline decision : "
              << toString(baseline.algorithm) << " + "
              << toString(baseline.preprocess) << "\n";
    std::cout << "Combinations tested : " << total   << "\n";
    std::cout << "Routing changes     : " << changed << "\n";
    std::cout << "Sensitivity         : "
              << std::setprecision(1)
              << (100.0 * changed / total) << "%\n";

    return 0;
}