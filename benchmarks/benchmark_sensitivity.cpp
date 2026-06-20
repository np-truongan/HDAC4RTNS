#include "generators.h"
#include "heuristics.h"
#include "strategies.h"
#include "engine.h"
#include "resource_stats.h"
#include "types.h"

#include <iostream>
#include <iomanip>
#include <fstream>
#include <chrono>
#include <vector>
#include <string>
#include <numeric>
#include <cmath>
#include <map>
#include <algorithm>

using Clock = std::chrono::high_resolution_clock;

SweepResult runWithConfig(
    const std::vector<std::pair<std::string, Chunk>>& datasets,
    size_t      chunkSize,
    const EngineConfig& cfg)
{
    size_t totalOrig = 0, totalComp = 0;
    double totalLat  = 0;
    double totalCpu  = 0;
    long peakRss = 0;
    int    count     = 0;

    for (const auto& [name, data] : datasets) {
        for (size_t off = 0; off < data.size(); off += chunkSize) {
            size_t len = std::min(chunkSize, data.size() - off);
            Chunk chunk(data.begin() + off, data.begin() + off + len);

            Features f = extractFeatures(chunk);
            Decision d = decide(f, cfg);

            auto meas = measureCompression([&]() {
                Chunk processed = chunk;
                if (d.preprocess == Preprocess::DELTA) {
                    processed = deltaEncode(chunk);
                } else if (d.preprocess == Preprocess::BITPACK) {
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

            if (meas.compressed.empty()) continue;

            totalOrig += len;
            totalComp += meas.compressed.size();
            totalLat  += meas.wallMs;
            totalCpu  += meas.cpuMs;
            peakRss    = std::max(peakRss, meas.peakRssKb);
            ++count;
        }
    }

    double avgRatio   = static_cast<double>(totalComp) / totalOrig;
    double avgLat     = totalLat / count;
    double throughput = (totalOrig / (1024.0 * 1024.0)) / (totalLat / 1000.0);
    double avgCpu     = totalCpu / count;

    return {
        "",
        "", 
        0, 
        avgRatio,
        avgLat,
        throughput,
        0.0, 
        avgCpu,
        peakRss,
        0.0 
    };
}

int main() {
    const size_t DATA_SIZE  = 1 << 20;
    const size_t CHUNK_SIZE = 4096;

    std::vector<std::pair<std::string, Chunk>> datasets = {
        { "Telemetry", generateTelemetry(DATA_SIZE) },
        { "JSON",      generateJSON(DATA_SIZE)      },
        { "Binary",    generateBinary(DATA_SIZE)    },
        { "Nibble",    generateNibble(DATA_SIZE)    }
    };

    std::cout << "========================================\n";
    std::cout << "  Threshold Sensitivity Analysis\n";
    std::cout << "========================================\n";
    std::cout << "Data size  : 1 MB per workload\n";
    std::cout << "Chunk size : " << CHUNK_SIZE << " bytes\n";
    std::cout << "Default    : entropy=6.5, smoothness=0.7\n";

    std::ofstream csv("results/sensitivity.csv");
    csv << "sweep,threshold_value,avg_ratio,avg_latency_ms,throughput_mbps,avg_cpu_ms,peak_rss_kb\n";

    std::vector<double> entropyValues = { 5.0, 5.5, 6.0, 6.5, 7.0, 7.5 };

    std::cout << "\n--- Sweep 1: Entropy Threshold ---\n";
    std::cout << "(smoothness threshold fixed at 0.7)\n\n";
    std::cout << std::left
              << std::setw(12) << "Entropy T."
              << std::setw(14) << "Avg Ratio"
              << std::setw(16) << "Avg Lat (ms)"
              << std::setw(18) << "Throughput MB/s"
              << std::setw(12) << "Avg CPU (ms)"
              << std::setw(10) << "RSS(KB)"
              << "\n";
    std::cout << std::string(82, '-') << "\n";

    for (double et : entropyValues) {
        EngineConfig cfg;
        cfg.entropyThreshold    = et;
        cfg.smoothnessThreshold = 0.7;

        auto r = runWithConfig(datasets, CHUNK_SIZE, cfg);

        bool isDefault = (et == 6.5);

        std::cout << std::left
                  << std::setw(12) << std::fixed << std::setprecision(1) << et
                  << std::setw(14) << std::setprecision(4) << r.avgRatio
                  << std::setw(16) << r.avgLatencyMs
                  << std::setw(18) << r.throughputMBps
                  << std::setw(12) << r.avgCpuMs
                  << std::setw(10) << r.peakRssKb
                  << (isDefault ? " <- default" : "")
                  << "\n";

        csv << "entropy," << et << ","
            << r.avgRatio << "," << r.avgLatencyMs << ","
            << r.throughputMBps << ","
            << r.avgCpuMs << "," << r.peakRssKb << "\n";
    }

    std::vector<double> smoothnessValues = { 0.5, 0.6, 0.7, 0.8, 0.9 };

    std::cout << "\n--- Sweep 2: Smoothness Threshold ---\n";
    std::cout << "(entropy threshold fixed at 6.5)\n\n";
    std::cout << std::left
              << std::setw(14) << "Smoothness T."
              << std::setw(14) << "Avg Ratio"
              << std::setw(16) << "Avg Lat (ms)"
              << std::setw(18) << "Throughput MB/s"
              << std::setw(12) << "Avg CPU (ms)"
              << std::setw(10) << "RSS(KB)"
              << "\n";
    std::cout << std::string(84, '-') << "\n";

    for (double st : smoothnessValues) {
        EngineConfig cfg;
        cfg.entropyThreshold    = 6.5;
        cfg.smoothnessThreshold = st;

        auto r = runWithConfig(datasets, CHUNK_SIZE, cfg);

        bool isDefault = (st == 0.7);

        std::cout << std::left
                  << std::setw(14) << std::fixed << std::setprecision(1) << st
                  << std::setw(14) << std::setprecision(4) << r.avgRatio
                  << std::setw(16) << r.avgLatencyMs
                  << std::setw(18) << r.throughputMBps
                  << std::setw(12) << r.avgCpuMs
                  << std::setw(10) << r.peakRssKb
                  << (isDefault ? " <- default" : "")
                  << "\n";

        csv << "smoothness," << st << ","
            << r.avgRatio << "," << r.avgLatencyMs << ","
            << r.throughputMBps << ","
            << r.avgCpuMs << "," << r.peakRssKb << "\n";
    }

    std::cout << "\nResults saved to results/sensitivity.csv\n";
    return 0;
}