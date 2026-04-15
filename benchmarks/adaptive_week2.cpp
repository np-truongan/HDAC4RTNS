// benchmarks/adaptive_week2.cpp
//
// Week 2 deliverable: first adaptive compression benchmark.
//
// Scope (per Week 2 plan):
//   - Single dataset: structured JSON (the Week 2 report used
//     this exclusively before expanding to all three in Week 3)
//   - Per-chunk log: entropy, smoothness, decision, compressed size
//   - Aggregate summary comparing adaptive vs static LZ4/ZSTD
//   - Output: results/adaptive_week2.csv
//
// The heuristic probes and decision engine are live here for
// the first time driving real compression calls.
// ============================================================

#include "generators.h"
#include "heuristics.h"
#include "strategies.h"
#include "engine.h"
#include "types.h"

#include <iostream>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <string>
#include <numeric>
#include <cmath>

using Clock = std::chrono::high_resolution_clock;

// ============================================================
//  Per-chunk record
// ============================================================
struct ChunkLog {
    size_t      index;
    double      entropy;
    double      smoothness;
    std::string algorithm;
    std::string preprocess;
    size_t      originalSize;
    size_t      compressedSize;
    double      ratio;
    double      latencyMs;
};

// ============================================================
//  Run adaptive compression on a dataset, logging every chunk
// ============================================================
std::vector<ChunkLog> runAdaptive(
    const Chunk& data,
    size_t chunkSize,
    const EngineConfig& cfg)
{
    std::vector<ChunkLog> log;
    size_t chunkIndex = 0;

    for (size_t offset = 0; offset < data.size(); offset += chunkSize) {
        size_t len = std::min(chunkSize, data.size() - offset);
        Chunk chunk(data.begin() + offset,
                    data.begin() + offset + len);

        // -- Heuristic analysis --
        Features f = extractFeatures(chunk);
        Decision d = decide(f, cfg);

        auto t0 = Clock::now();

        // -- Preprocessing --
        Chunk processed = chunk;
        if (d.preprocess == Preprocess::DELTA) {
            processed = deltaEncode(chunk);
        }
        // BitPack not yet activated in Week 2 scope

        // -- Compression --
        Chunk compressed;
        switch (d.algorithm) {
            case Algorithm::LZ4:  compressed = compressLZ4(processed);  break;
            case Algorithm::ZSTD: compressed = compressZSTD(processed); break;
            case Algorithm::GZIP: compressed = compressGZIP(processed); break;
        }

        auto t1 = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        ChunkLog entry;
        entry.index          = chunkIndex++;
        entry.entropy        = f.entropy;
        entry.smoothness     = f.smoothness;
        entry.algorithm      = toString(d.algorithm);
        entry.preprocess     = toString(d.preprocess);
        entry.originalSize   = len;
        entry.compressedSize = compressed.size();
        entry.ratio          = static_cast<double>(compressed.size()) / len;
        entry.latencyMs      = ms;

        log.push_back(entry);
    }

    return log;
}

// ============================================================
//  Run a single static algorithm on a dataset (for comparison)
// ============================================================
struct StaticResult {
    double avgRatio;
    double avgLatencyMs;
    double throughputMBps;
};

StaticResult runStatic(
    const Chunk& data,
    size_t chunkSize,
    Algorithm algo)
{
    size_t totalOriginal   = 0;
    size_t totalCompressed = 0;
    double totalLatency    = 0;
    int    count           = 0;

    for (size_t offset = 0; offset < data.size(); offset += chunkSize) {
        size_t len = std::min(chunkSize, data.size() - offset);
        Chunk chunk(data.begin() + offset,
                    data.begin() + offset + len);

        auto t0 = Clock::now();

        Chunk compressed;
        switch (algo) {
            case Algorithm::LZ4:  compressed = compressLZ4(chunk);  break;
            case Algorithm::ZSTD: compressed = compressZSTD(chunk); break;
            case Algorithm::GZIP: compressed = compressGZIP(chunk); break;
        }

        auto t1 = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        totalOriginal   += len;
        totalCompressed += compressed.size();
        totalLatency    += ms;
        ++count;
    }

    double avgRatio    = static_cast<double>(totalCompressed) / totalOriginal;
    double avgLatency  = totalLatency / count;
    double throughput  = (totalOriginal / (1024.0 * 1024.0)) /
                         (totalLatency  / 1000.0);

    return { avgRatio, avgLatency, throughput };
}

// ============================================================
//  Print per-chunk log to stdout (first N chunks only)
// ============================================================
void printChunkLog(const std::vector<ChunkLog>& log, size_t maxRows = 10) {
    std::cout << std::fixed << std::setprecision(4);
    std::cout << "\n"
              << std::setw(6)  << "Chunk"
              << std::setw(10) << "Entropy"
              << std::setw(12) << "Smoothness"
              << std::setw(8)  << "Algo"
              << std::setw(10) << "Preproc"
              << std::setw(10) << "Ratio"
              << std::setw(12) << "Latency ms"
              << "\n";
    std::cout << std::string(68, '-') << "\n";

    size_t rows = std::min(log.size(), maxRows);
    for (size_t i = 0; i < rows; ++i) {
        const auto& e = log[i];
        std::cout << std::setw(6)  << e.index
                  << std::setw(10) << e.entropy
                  << std::setw(12) << e.smoothness
                  << std::setw(8)  << e.algorithm
                  << std::setw(10) << e.preprocess
                  << std::setw(10) << e.ratio
                  << std::setw(12) << e.latencyMs
                  << "\n";
    }

    if (log.size() > maxRows)
        std::cout << "  ... (" << log.size() - maxRows
                  << " more chunks, see CSV)\n";
}

// ============================================================
//  Aggregate a chunk log into summary stats
// ============================================================
StaticResult aggregate(const std::vector<ChunkLog>& log) {
    double sumRatio = 0, sumLatency = 0;
    size_t totalOrig = 0, totalComp = 0;

    for (const auto& e : log) {
        sumRatio   += e.ratio;
        sumLatency += e.latencyMs;
        totalOrig  += e.originalSize;
        totalComp  += e.compressedSize;
    }

    double n          = static_cast<double>(log.size());
    double avgRatio   = static_cast<double>(totalComp) / totalOrig;
    double avgLatency = sumLatency / n;
    double throughput = (totalOrig / (1024.0 * 1024.0)) /
                        (sumLatency / 1000.0);

    return { avgRatio, avgLatency, throughput };
}

// ============================================================
//  Main
// ============================================================
int main() {
    const size_t DATA_SIZE  = 1 << 20;   // 1 MB
    const size_t CHUNK_SIZE = 4096;       // 4 KB — matches Week 1 primary chunk

    EngineConfig cfg;   // default thresholds

    // Week 2 scope: structured JSON dataset only
    Chunk jsonData = generateJSON(DATA_SIZE);

    double dataEntropy    = computeEntropy(
        Chunk(jsonData.begin(), jsonData.begin() + CHUNK_SIZE));
    double dataSmoothness = computeSmoothness(
        Chunk(jsonData.begin(), jsonData.begin() + CHUNK_SIZE));

    std::cout << "========================================\n";
    std::cout << "  Week 2: Adaptive Compression — JSON\n";
    std::cout << "========================================\n";
    std::cout << "Dataset entropy   : " << std::fixed
              << std::setprecision(4) << dataEntropy    << "\n";
    std::cout << "Dataset smoothness: " << dataSmoothness << "\n";
    std::cout << "Chunk size        : " << CHUNK_SIZE    << " bytes\n";
    std::cout << "Engine thresholds : entropy > "
              << cfg.entropyThreshold
              << " → LZ4 | smoothness > "
              << cfg.smoothnessThreshold
              << " → Delta\n";

    // --------------------------------------------------------
    //  Run adaptive pipeline
    // --------------------------------------------------------
    std::cout << "\n[Adaptive] Running...\n";
    auto adaptiveLog = runAdaptive(jsonData, CHUNK_SIZE, cfg);

    // Strategy usage counts
    int usedLZ4 = 0, usedZSTD = 0, usedDelta = 0;
    for (const auto& e : adaptiveLog) {
        if (e.algorithm  == "LZ4")   ++usedLZ4;
        if (e.algorithm  == "ZSTD")  ++usedZSTD;
        if (e.preprocess == "Delta") ++usedDelta;
    }

    std::cout << "Strategy usage across "
              << adaptiveLog.size() << " chunks:\n";
    std::cout << "  LZ4  : " << usedLZ4   << " chunks\n";
    std::cout << "  ZSTD : " << usedZSTD  << " chunks\n";
    std::cout << "  Delta: " << usedDelta << " chunks\n";

    printChunkLog(adaptiveLog);

    auto adaptiveAgg = aggregate(adaptiveLog);

    // --------------------------------------------------------
    //  Run static baselines for comparison
    // --------------------------------------------------------
    std::cout << "\n[Static LZ4]  Running...\n";
    auto lz4Result  = runStatic(jsonData, CHUNK_SIZE, Algorithm::LZ4);

    std::cout << "[Static ZSTD] Running...\n";
    auto zstdResult = runStatic(jsonData, CHUNK_SIZE, Algorithm::ZSTD);

    // --------------------------------------------------------
    //  Comparison table
    // --------------------------------------------------------
    std::cout << "\n========================================\n";
    std::cout << "  Summary: JSON Dataset (4KB chunks)\n";
    std::cout << "========================================\n";
    std::cout << std::fixed << std::setprecision(4);
    std::cout << std::left
              << std::setw(18) << "System"
              << std::setw(14) << "Avg Ratio"
              << std::setw(16) << "Avg Latency ms"
              << std::setw(16) << "Throughput MB/s"
              << "\n";
    std::cout << std::string(64, '-') << "\n";

    auto printRow = [](const std::string& name, const StaticResult& r) {
        std::cout << std::left  << std::setw(18) << name
                  << std::right << std::setw(14) << r.avgRatio
                  << std::setw(16) << r.avgLatencyMs
                  << std::setw(16) << r.throughputMBps
                  << "\n";
    };

    printRow("Adaptive",    adaptiveAgg);
    printRow("Static LZ4",  lz4Result);
    printRow("Static ZSTD", zstdResult);

    // --------------------------------------------------------
    //  Save CSV
    // --------------------------------------------------------
    std::ofstream csv("results/adaptive_week2.csv");
    csv << "chunk_index,entropy,smoothness,algorithm,preprocess,"
           "original_bytes,compressed_bytes,ratio,latency_ms\n";

    for (const auto& e : adaptiveLog) {
        csv << e.index          << ","
            << e.entropy        << ","
            << e.smoothness     << ","
            << e.algorithm      << ","
            << e.preprocess     << ","
            << e.originalSize   << ","
            << e.compressedSize << ","
            << e.ratio          << ","
            << e.latencyMs      << "\n";
    }

    std::cout << "\nPer-chunk log saved to results/adaptive_week2.csv\n";
    return 0;
}
