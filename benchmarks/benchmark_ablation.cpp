#include "generators.h"
#include "heuristics.h"
#include "strategies.h"
#include "engine.h"
#include "stats.h"
#include "types.h"

#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>

static constexpr size_t CHUNK_SIZE  = 4096;
static constexpr size_t DATA_SIZE   = 1 << 20;
static constexpr int    N_TRIALS    = 30;

struct AblationVariant {
    Preprocess  prep;
    Algorithm   algo;
    std::string label;
};

static const std::vector<AblationVariant> VARIANTS = {
    { Preprocess::DELTA,   Algorithm::ZSTD, "Delta+ZSTD"   },
    { Preprocess::DELTA,   Algorithm::LZ4,  "Delta+LZ4"    },
    { Preprocess::BITPACK, Algorithm::ZSTD, "BitPack+ZSTD" },
    { Preprocess::BITPACK, Algorithm::LZ4,  "BitPack+LZ4"  },
    { Preprocess::NONE,    Algorithm::ZSTD, "None+ZSTD"    },
    { Preprocess::NONE,    Algorithm::LZ4,  "None+LZ4"     },
};

struct WorkloadDef {
    std::string name;
    Chunk       data;
};

// Returns mean latency in ms over N_TRIALS compress cycles for one chunk.
static TrialStats benchmarkLatency(
    const Chunk&          chunk,
    const Decision&       forced,
    const std::string&    label)
{
    std::vector<double> latencies;
    latencies.reserve(N_TRIALS);

    for (int t = 0; t < N_TRIALS; ++t) {
        auto t0 = std::chrono::high_resolution_clock::now();
        compressWithDecision(chunk, forced);
        auto t1 = std::chrono::high_resolution_clock::now();
        latencies.push_back(
            std::chrono::duration<double, std::milli>(t1 - t0).count());
    }

    return computeStats(label, latencies);
}

int main() {
    std::vector<WorkloadDef> workloads = {
        { "Telemetry", generateTelemetry(DATA_SIZE) },
        { "Nibble",    generateNibble(DATA_SIZE)    },
    };

    std::ofstream csv("results/ablation.csv");
    csv << "workload,variant,mean_ratio,mean_latency_ms,"
           "ci95_lat_low,ci95_lat_high\n";

    std::cout << std::fixed << std::setprecision(4);
    std::cout << std::left  << std::setw(12) << "Workload"
              << std::setw(16) << "Variant"
              << std::setw(10) << "Ratio"
              << std::setw(14) << "Latency(ms)"
              << std::setw(20) << "95% CI"
              << "\n";
    std::cout << std::string(72, '-') << "\n";

    for (const auto& wl : workloads) {
        // Use first chunk only — ratio is deterministic, latency is per-chunk.
        Chunk chunk(wl.data.begin(),
                    wl.data.begin() + std::min(CHUNK_SIZE, wl.data.size()));

        for (const auto& v : VARIANTS) {
            // BitPack only valid when all values in [0,15].
            // Telemetry values are not bounded; skip BitPack variants for it.
            if (v.prep == Preprocess::BITPACK && wl.name == "Telemetry")
                continue;
            // Delta on Nibble is valid as a comparison point; keep it.

            Decision forced;
            forced.preprocess = v.prep;
            forced.algorithm  = v.algo;

            CompressResult r = compressWithDecision(chunk, forced);

            std::string statsLabel =
                wl.name + " | " + v.label + " | latency_ms";
            TrialStats lat = benchmarkLatency(chunk, forced, statsLabel);

            std::cout << std::left  << std::setw(12) << wl.name
                      << std::setw(16) << v.label
                      << std::right
                      << std::setw(10) << r.ratio
                      << std::setw(14) << lat.mean
                      << "  [" << lat.ci95Low << ", " << lat.ci95High << "]\n";

            csv << wl.name   << ","
                << v.label   << ","
                << r.ratio   << ","
                << lat.mean  << ","
                << lat.ci95Low << ","
                << lat.ci95High << "\n";
        }
    }

    std::cout << "\nResults written to results/ablation.csv\n";
    return 0;
}