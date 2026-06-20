#include "generators.h"
#include "heuristics.h"
#include "strategies.h"
#include "engine.h"
#include "resource_stats.h"
#include "stats.h"
#include "types.h"

#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <numeric>

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

int main() {
    std::vector<WorkloadDef> workloads = {
        { "Telemetry", generateTelemetry(DATA_SIZE) },
        { "Nibble",    generateNibble(DATA_SIZE)    },
    };

    std::ofstream csv("results/ablation.csv");
    csv << "workload,variant,mean_ratio,mean_latency_ms,ci95_lat_low,ci95_lat_high,mean_cpu_ms,peak_rss_kb\n";

    std::cout << std::fixed << std::setprecision(4);
    std::cout << std::left  << std::setw(12) << "Workload"
              << std::setw(16) << "Variant"
              << std::setw(10) << "Ratio"
              << std::setw(14) << "Latency(ms)"
              << std::setw(20) << "95% CI"
              << std::setw(12) << "CPU(ms)"
              << std::setw(10) << "RSS(KB)"
              << "\n";
    std::cout << std::string(94, '-') << "\n";

    for (const auto& wl : workloads) {
        Chunk chunk(wl.data.begin(),
                    wl.data.begin() + std::min(CHUNK_SIZE, wl.data.size()));

        for (const auto& v : VARIANTS) {
            if (v.prep == Preprocess::BITPACK && wl.name == "Telemetry")
                continue;

            Decision forced;
            forced.preprocess = v.prep;
            forced.algorithm  = v.algo;

            bool bitpackEligible = true;
            if (forced.preprocess == Preprocess::BITPACK) {
                for (Byte b : chunk)
                    if (b > 15) { bitpackEligible = false; break; }
                if (!bitpackEligible)
                    forced.preprocess = Preprocess::NONE;
            }

            CompressResult compResult = compressWithDecision(chunk, forced);
            double ratio = compResult.ratio;

            std::vector<double> lats, cpus;
            long peakRss = 0;
            for (int t = 0; t < N_TRIALS; ++t) {
                auto meas = measureCompression([&]() {
                    Chunk processed = chunk;
                    if (forced.preprocess == Preprocess::DELTA) {
                        processed = deltaEncode(chunk);
                    } else if (forced.preprocess == Preprocess::BITPACK) {
                        processed = bitPackEncode(chunk);
                    }

                    switch (forced.algorithm) {
                        case Algorithm::LZ4:  return compressLZ4(processed);
                        case Algorithm::ZSTD: return compressZSTD(processed);
                        case Algorithm::GZIP: return compressGZIP(processed);
                    }
                    return Chunk{};
                });
                lats.push_back(meas.wallMs);
                cpus.push_back(meas.cpuMs);
                peakRss = std::max(peakRss, meas.peakRssKb);
            }

            TrialStats lat = computeStats(
                wl.name + " | " + v.label + " | latency_ms", lats);
            double avgCpu = std::accumulate(cpus.begin(), cpus.end(), 0.0) / cpus.size();

            std::cout << std::left  << std::setw(12) << wl.name
                      << std::setw(16) << v.label
                      << std::right
                      << std::setw(10) << ratio
                      << std::setw(14) << lat.mean
                      << "  [" << lat.ci95Low << ", " << lat.ci95High << "]"
                      << std::setw(12) << avgCpu
                      << std::setw(10) << peakRss
                      << "\n";

            csv << wl.name   << ","
                << v.label   << ","
                << ratio     << ","
                << lat.mean  << ","
                << lat.ci95Low << ","
                << lat.ci95High << ","
                << avgCpu    << ","
                << peakRss   << "\n";
        }
    }

    std::cout << "\nResults written to results/ablation.csv\n";
    return 0;
}