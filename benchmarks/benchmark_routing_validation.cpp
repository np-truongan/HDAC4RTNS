#include "generators.h"
#include "heuristics.h"
#include "strategies.h"
#include "engine.h"
#include "resource_stats.h"
#include "types.h"

#include <iostream>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <string>
#include <numeric>
#include <cmath>
#include <algorithm>

using Clock = std::chrono::high_resolution_clock;

struct ChunkLog {
    size_t      index;
    std::string dataset;
    double      entropy;
    double      smoothness;
    std::string algorithm;
    std::string preprocess;
    size_t      originalSize;
    size_t      compressedSize;
    double      ratio;
    double      latencyMs;
    double      cpuMs;
    long        peakRssKb;
};

std::vector<ChunkLog> runAdaptive(
    const Chunk&       data,
    const std::string& datasetName,
    size_t             chunkSize,
    const EngineConfig& cfg)
{
    std::vector<ChunkLog> log;
    size_t idx = 0;

    for (size_t offset = 0; offset < data.size(); offset += chunkSize) {
        size_t len = std::min(chunkSize, data.size() - offset);
        Chunk chunk(data.begin() + offset,
                    data.begin() + offset + len);

        Features f = extractFeatures(chunk);
        Decision d = decide(f, cfg);

        auto meas = measureCompression([&]() {
            Chunk processed = chunk;
            if (d.preprocess == Preprocess::DELTA)
                processed = deltaEncode(chunk);

            switch (d.algorithm) {
                case Algorithm::LZ4:  return compressLZ4(processed);
                case Algorithm::ZSTD: return compressZSTD(processed);
                case Algorithm::GZIP: return compressGZIP(processed);
            }
            return Chunk{};
        });

        if (meas.compressed.empty()) continue;

        log.push_back({
            idx++,
            datasetName,
            f.entropy, f.smoothness,
            toString(d.algorithm), toString(d.preprocess),
            len, meas.compressed.size(),
            static_cast<double>(meas.compressed.size()) / len,
            meas.wallMs,
            meas.cpuMs,
            meas.peakRssKb
        });
    }

    return log;
}

StaticResult runStatic(
    const Chunk& data,
    size_t chunkSize,
    Algorithm algo)
{
    size_t totalOrig = 0, totalComp = 0;
    double totalLatency = 0;
    double totalCpu = 0;
    long peakRss = 0;
    int count = 0;

    for (size_t offset = 0; offset < data.size(); offset += chunkSize) {
        size_t len = std::min(chunkSize, data.size() - offset);
        Chunk chunk(data.begin() + offset,
                    data.begin() + offset + len);

        auto meas = measureCompression([&]() {
            switch (algo) {
                case Algorithm::LZ4:  return compressLZ4(chunk);
                case Algorithm::ZSTD: return compressZSTD(chunk);
                case Algorithm::GZIP: return compressGZIP(chunk);
            }
            return Chunk{};
        });

        if (meas.compressed.empty()) continue;

        totalOrig    += len;
        totalComp    += meas.compressed.size();
        totalLatency += meas.wallMs;
        totalCpu     += meas.cpuMs;
        peakRss       = std::max(peakRss, meas.peakRssKb);
        ++count;
    }

    return {
        static_cast<double>(totalComp) / totalOrig,
        totalLatency / count,
        (totalOrig / (1024.0 * 1024.0)) / (totalLatency / 1000.0),
        totalCpu / count,
        peakRss
    };
}

StaticResult aggregate(const std::vector<ChunkLog>& log) {
    size_t totalOrig = 0, totalComp = 0;
    double totalLatency = 0;
    double totalCpu = 0;
    long peakRss = 0;

    for (const auto& e : log) {
        totalOrig    += e.originalSize;
        totalComp    += e.compressedSize;
        totalLatency += e.latencyMs;
        totalCpu     += e.cpuMs;
        if (e.peakRssKb > peakRss) peakRss = e.peakRssKb;
    }

    double n = static_cast<double>(log.size());
    return {
        static_cast<double>(totalComp) / totalOrig,
        totalLatency / n,
        (totalOrig / (1024.0 * 1024.0)) / (totalLatency / 1000.0),
        totalCpu / n,
        peakRss
    };
}

void printFeatureDistribution(
    const std::vector<ChunkLog>& log,
    const std::string& datasetName)
{
    std::vector<double> entropies, smoothnesses;
    for (const auto& e : log) {
        entropies.push_back(e.entropy);
        smoothnesses.push_back(e.smoothness);
    }

    auto minmax_e = std::minmax_element(entropies.begin(), entropies.end());
    auto minmax_s = std::minmax_element(smoothnesses.begin(), smoothnesses.end());

    double avgE = std::accumulate(entropies.begin(), entropies.end(), 0.0)
                  / entropies.size();
    double avgS = std::accumulate(smoothnesses.begin(), smoothnesses.end(), 0.0)
                  / smoothnesses.size();

    std::cout << std::fixed << std::setprecision(4);
    std::cout << "  " << std::left << std::setw(12) << datasetName
              << " entropy:    avg=" << std::setw(7) << avgE
              << "  min=" << std::setw(7) << *minmax_e.first
              << "  max=" << std::setw(7) << *minmax_e.second << "\n";
    std::cout << "  " << std::setw(12) << ""
              << " smoothness: avg=" << std::setw(7) << avgS
              << "  min=" << std::setw(7) << *minmax_s.first
              << "  max=" << std::setw(7) << *minmax_s.second << "\n";
}

struct StrategyUsage {
    int lz4, zstd, delta, bitpack, none;
    int total;
};

StrategyUsage countStrategies(const std::vector<ChunkLog>& log) {
    StrategyUsage u{};
    u.total = static_cast<int>(log.size());
    for (const auto& e : log) {
        if (e.algorithm  == "LZ4")     ++u.lz4;
        if (e.algorithm  == "ZSTD")    ++u.zstd;
        if (e.preprocess == "Delta")   ++u.delta;
        if (e.preprocess == "BitPack") ++u.bitpack;
        if (e.preprocess == "None")    ++u.none;
    }
    return u;
}

void printRow(const std::string& label, const StaticResult& r) {
    std::cout << std::left  << std::setw(20) << label
              << std::right
              << std::setw(12) << r.avgRatio
              << std::setw(16) << r.avgLatencyMs
              << std::setw(18) << r.throughputMBps
              << std::setw(12) << r.avgCpuMs
              << std::setw(10) << r.peakRssKb
              << "\n";
}

int main() {
    const size_t DATA_SIZE  = 1 << 20;
    const size_t CHUNK_SIZE = 4096;

    EngineConfig cfg;

    std::vector<std::pair<std::string, Chunk>> datasets = {
        { "Telemetry", generateTelemetry(DATA_SIZE) },
        { "JSON",      generateJSON(DATA_SIZE)      },
        { "Binary",    generateBinary(DATA_SIZE)    },
        { "Nibble",    generateNibble(DATA_SIZE)    }
    };

    std::cout << "========================================\n";
    std::cout << "  Decision Engine Routing Validation\n";
    std::cout << "========================================\n";
    std::cout << "Chunk size : " << CHUNK_SIZE << " bytes\n";
    std::cout << "Thresholds : entropy  > " << cfg.entropyThreshold
              << " → LZ4\n";
    std::cout << "             smoothness > " << cfg.smoothnessThreshold
              << " → Delta\n";
    std::cout << "             entropy  < " << cfg.bitpackThreshold
              << " → BitPack eligible\n";

    std::cout << "\n--- Heuristic Feature Distributions ---\n";
    std::cout << "(justifies threshold choices)\n\n";

    std::vector<std::vector<ChunkLog>> allLogs;

    for (auto& [name, data] : datasets) {
        auto log = runAdaptive(data, name, CHUNK_SIZE, cfg);
        printFeatureDistribution(log, name);
        allLogs.push_back(log);
    }

    std::cout << "\n--- Strategy Activation Verification ---\n";
    std::cout << "(expected: Delta on Telemetry, LZ4 on Binary, ZSTD on JSON)\n\n";

    bool allCorrect = true;
    const char* dsNames[] = { "Telemetry", "JSON", "Binary" };

    for (int i = 0; i < 3; ++i) {
        auto u = countStrategies(allLogs[i]);
        double deltaPct   = 100.0 * u.delta   / u.total;
        double lz4Pct     = 100.0 * u.lz4     / u.total;
        double zstdPct    = 100.0 * u.zstd    / u.total;

        std::cout << "  " << std::left << std::setw(12) << dsNames[i]
                  << " LZ4="    << std::setw(6) << std::fixed
                  << std::setprecision(1) << lz4Pct   << "%"
                  << " ZSTD="   << std::setw(6) << zstdPct  << "%"
                  << " Delta="  << std::setw(6) << deltaPct << "%";

        bool ok = true;
        if (i == 0 && u.delta == 0)  { ok = false; std::cout << "  [WARN] Delta not activating on Telemetry!"; }
        if (i == 1 && u.delta > 0)   { ok = false; std::cout << "  [WARN] Delta firing on JSON unexpectedly!"; }
        if (i == 2 && u.lz4 == 0)    { ok = false; std::cout << "  [WARN] LZ4 not routing Binary!"; }
        if (ok) std::cout << "  [OK]";
        if (!ok) allCorrect = false;

        std::cout << "\n";
    }

    if (allCorrect)
        std::cout << "\n  All strategy activations correct.\n";

    std::cout << "\n--- Per-Dataset Performance Comparison ---\n";

    for (int i = 0; i < 3; ++i) {
        auto& [name, data] = datasets[i];
        auto& log          = allLogs[i];

        auto adaptiveResult = aggregate(log);
        auto lz4Result      = runStatic(data, CHUNK_SIZE, Algorithm::LZ4);
        auto zstdResult     = runStatic(data, CHUNK_SIZE, Algorithm::ZSTD);

        std::cout << "\n  Dataset: " << name << "\n";
        std::cout << "  " << std::string(86, '-') << "\n";
        std::cout << "  " << std::left << std::setw(20) << "System"
                  << std::right
                  << std::setw(12) << "Avg Ratio"
                  << std::setw(16) << "Avg Lat (ms)"
                  << std::setw(18) << "Throughput MB/s"
                  << std::setw(12) << "Avg CPU (ms)"
                  << std::setw(10) << "RSS(KB)"
                  << "\n";
        std::cout << "  " << std::string(86, '-') << "\n";

        printRow("  Adaptive",    adaptiveResult);
        printRow("  Static LZ4",  lz4Result);
        printRow("  Static ZSTD", zstdResult);
    }

    std::ofstream csv("results/routing_validation.csv");
    csv << "dataset,chunk_index,entropy,smoothness,"
           "algorithm,preprocess,original_bytes,"
           "compressed_bytes,ratio,latency_ms,cpu_ms,peak_rss_kb\n";

    for (const auto& log : allLogs) {
        for (const auto& e : log) {
            csv << e.dataset        << ","
                << e.index          << ","
                << e.entropy        << ","
                << e.smoothness     << ","
                << e.algorithm      << ","
                << e.preprocess     << ","
                << e.originalSize   << ","
                << e.compressedSize << ","
                << e.ratio          << ","
                << e.latencyMs      << ","
                << e.cpuMs          << ","
                << e.peakRssKb      << "\n";
        }
    }

    std::cout << "\nPer-chunk log saved to results/routing_validation.csv\n";
    return 0;
}