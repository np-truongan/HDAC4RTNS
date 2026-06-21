#include "generators.h"
#include "heuristics.h"
#include "strategies.h"
#include "engine.h"
#include "resource_stats.h"
#include "stats.h"

#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <chrono>

static constexpr size_t   CHUNK_SIZE  = 4096;
static constexpr unsigned FIXED_SEED  = 42;
static constexpr size_t   DATA_SIZE   = 1UL << 20;

int main() {
    std::cout << "========================================\n";
    std::cout << "  Benchmark: Boundary Telemetry (Noise Robustness)\n";
    std::cout << "========================================\n\n";

    EngineConfig cfg{};

    // Sweep noise from 0.0 to 0.9 in steps of 0.1
    std::vector<float> noise_levels;
    for (int i = 0; i <= 9; ++i) {
        noise_levels.push_back(i / 10.0f);
    }

    std::ofstream csv("results/boundary_results.csv");
    csv << "noise_scale,smoothness,entropy,"
           "algorithm,preprocess,"
           "chosen_ratio,alt_ratio,ratio_delta,"
           "chosen_latency_us,alt_latency_us,"
           "chosen_cpu_ms,alt_cpu_ms,chosen_rss_kb,alt_rss_kb\n";

    std::cout << std::fixed << std::setprecision(4);
    std::cout << std::left  << std::setw(12) << "Noise"
              << std::setw(12) << "Smoothness"
              << std::setw(10) << "Entropy"
              << std::setw(10) << "Algorithm"
              << std::setw(10) << "Preprocess"
              << std::setw(14) << "ChosenRatio"
              << std::setw(14) << "AltRatio"
              << std::setw(14) << "RatioDelta"
              << std::setw(14) << "LatChosen(µs)"
              << std::setw(14) << "LatAlt(µs)"
              << std::setw(12) << "CPU(ms)"
              << std::setw(10) << "RSS(KB)"
              << "\n";
    std::cout << std::string(136, '-') << "\n";

    for (float noise : noise_levels) {
        Chunk data = generateBoundaryTelemetry(DATA_SIZE, FIXED_SEED, noise);
        Chunk chunk(data.begin(), data.begin() + CHUNK_SIZE);

        Features f = extractFeatures(chunk);
        Decision d = decide(f, cfg);
        Decision alt = alternativeDecision(f, cfg, d);

        auto chosenMeas = measureCompression([&]() {
            Chunk processed = chunk;
            if (d.preprocess == Preprocess::DELTA)
                processed = deltaEncode(chunk);
            else if (d.preprocess == Preprocess::BITPACK) {
                bool eligible = true;
                for (Byte b : processed)
                    if (b > 15) { eligible = false; break; }
                if (eligible)
                    processed = bitPackEncode(processed);
                else
                    d.preprocess = Preprocess::NONE;
            }
            switch (d.algorithm) {
                case Algorithm::LZ4:  return compressLZ4(processed);
                case Algorithm::ZSTD: return compressZSTD(processed);
                case Algorithm::GZIP: return compressGZIP(processed);
            }
            return Chunk{};
        });

        auto altMeas = measureCompression([&]() {
            Chunk processed = chunk;
            if (alt.preprocess == Preprocess::DELTA)
                processed = deltaEncode(chunk);
            else if (alt.preprocess == Preprocess::BITPACK) {
                bool eligible = true;
                for (Byte b : processed)
                    if (b > 15) { eligible = false; break; }
                if (eligible)
                    processed = bitPackEncode(processed);
                else
                    alt.preprocess = Preprocess::NONE;
            }
            switch (alt.algorithm) {
                case Algorithm::LZ4:  return compressLZ4(processed);
                case Algorithm::ZSTD: return compressZSTD(processed);
                case Algorithm::GZIP: return compressGZIP(processed);
            }
            return Chunk{};
        });

        CompressResult chosen = compressWithDecision(chunk, d);
        CompressResult altRes = compressWithDecision(chunk, alt);

        double chosenLatUs = chosenMeas.wallMs * 1000.0;
        double altLatUs    = altMeas.wallMs * 1000.0;
        double delta = altRes.ratio - chosen.ratio;

        std::cout << std::setw(12) << noise
                  << std::setw(12) << f.smoothness
                  << std::setw(10) << f.entropy
                  << std::setw(10) << toString(d.algorithm)
                  << std::setw(10) << toString(d.preprocess)
                  << std::setw(14) << chosen.ratio
                  << std::setw(14) << altRes.ratio
                  << std::setw(14) << delta
                  << std::setw(14) << chosenLatUs
                  << std::setw(14) << altLatUs
                  << std::setw(12) << chosenMeas.cpuMs
                  << std::setw(10) << chosenMeas.peakRssKb
                  << "\n";

        csv << noise            << ","
            << f.smoothness     << ","
            << f.entropy        << ","
            << toString(d.algorithm)   << ","
            << toString(d.preprocess)  << ","
            << chosen.ratio     << ","
            << altRes.ratio     << ","
            << delta << ","
            << chosenLatUs      << ","
            << altLatUs         << ","
            << chosenMeas.cpuMs << ","
            << altMeas.cpuMs    << ","
            << chosenMeas.peakRssKb << ","
            << altMeas.peakRssKb << "\n";
    }

    std::cout << "\nResults saved to results/boundary_results.csv\n";

    std::cout << "\n--- Threshold sensitivity for Boundary workload ---\n";

    std::vector<double> entropyThresholds  = {5.5, 6.0, 6.5, 7.0, 7.5};
    std::vector<double> smoothThresholds   = {0.5, 0.6, 0.7, 0.8, 0.9};
    static constexpr double BITPACK_THRESH = 4.0;

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