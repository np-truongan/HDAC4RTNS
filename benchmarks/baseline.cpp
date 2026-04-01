// benchmarks/baseline.cpp
//
// Week 1 deliverable: baseline performance dataset.
//
// Measures compression ratio, throughput, and latency for
// LZ4, ZSTD, and GZIP across three workload types and four
// chunk sizes.  All measurements use real library calls.
// Output is written to results/baseline.csv.
// ============================================================

#include "generators.h"
#include "strategies.h"
#include "heuristics.h"
#include "types.h"

#include <iostream>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <string>
#include <functional>
#include <numeric>
#include <cmath>
#include <stdexcept>

using Clock = std::chrono::high_resolution_clock;

// ============================================================
//  One measurement: compress all chunks of a dataset and
//  return aggregate stats.
// ============================================================
struct BaselineResult {
    std::string dataset;
    std::string algorithm;
    size_t      chunkSize;
    double      entropy;
    double      avgRatio;
    double      avgLatencyMs;
    double      throughputMBps;
};

BaselineResult runBaseline(
    const Chunk& data,
    const std::string& datasetName,
    size_t chunkSize,
    const std::string& algoName,
    std::function<Chunk(const Chunk&)> compressFn)
{
    size_t totalOriginal   = 0;
    size_t totalCompressed = 0;
    double totalLatencyMs  = 0;
    int    chunks          = 0;

    for (size_t offset = 0; offset < data.size(); offset += chunkSize) {
        size_t len = std::min(chunkSize, data.size() - offset);
        Chunk chunk(data.begin() + offset,
                    data.begin() + offset + len);

        auto t0 = Clock::now();
        Chunk compressed = compressFn(chunk);
        auto t1 = Clock::now();

        if (compressed.empty())
            throw std::runtime_error(
                algoName + " compression failed on " + datasetName);

        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        totalOriginal   += len;
        totalCompressed += compressed.size();
        totalLatencyMs  += ms;
        ++chunks;
    }

    double avgRatio    = static_cast<double>(totalCompressed) / totalOriginal;
    double avgLatency  = totalLatencyMs / chunks;
    double throughput  = (totalOriginal / (1024.0 * 1024.0)) /
                         (totalLatencyMs / 1000.0);

    // Entropy is a property of the dataset, not the chunk size
    double entropy = computeEntropy(
        Chunk(data.begin(), data.begin() + std::min(data.size(), chunkSize)));

    return {
        datasetName, algoName, chunkSize,
        entropy, avgRatio, avgLatency, throughput
    };
}

// ============================================================
//  Main
// ============================================================
int main() {
    const size_t DATA_SIZE = 1 << 20;   // 1 MB per dataset

    std::vector<std::pair<std::string, Chunk>> datasets = {
        { "Telemetry", generateTelemetry(DATA_SIZE) },
        { "JSON",      generateJSON(DATA_SIZE)      },
        { "Binary",    generateBinary(DATA_SIZE)    }
    };

    std::vector<size_t> chunkSizes = { 1024, 4096, 16384, 65536 };

    // algorithm name → compress function
    std::vector<std::pair<std::string, std::function<Chunk(const Chunk&)>>>
    algorithms = {
        { "LZ4",  compressLZ4  },
        { "ZSTD", compressZSTD },
        { "GZIP", compressGZIP }
    };

    // --------------------------------------------------------
    //  Run all combinations
    // --------------------------------------------------------
    std::vector<BaselineResult> allResults;

    for (auto& [dsName, dsData] : datasets) {
        double dsEntropy = computeEntropy(
            Chunk(dsData.begin(), dsData.begin() + 65536));

        std::cout << "\n=== Dataset: " << dsName
                  << " | Entropy: " << std::fixed
                  << std::setprecision(3) << dsEntropy << " ===\n";

        for (size_t cs : chunkSizes) {
            std::cout << "  Chunk " << cs << " bytes:\n";
            for (auto& [algoName, fn] : algorithms) {
                auto r = runBaseline(dsData, dsName, cs, algoName, fn);
                allResults.push_back(r);

                std::cout << "    " << std::setw(5) << algoName
                          << " | ratio: "      << std::setw(6)
                          << std::setprecision(4) << r.avgRatio
                          << " | latency: "    << std::setw(8)
                          << std::setprecision(4) << r.avgLatencyMs << " ms"
                          << " | throughput: " << std::setw(8)
                          << std::setprecision(2) << r.throughputMBps
                          << " MB/s\n";
            }
        }
    }

    // --------------------------------------------------------
    //  Write CSV
    // --------------------------------------------------------
    std::ofstream csv("results/baseline.csv");
    if (!csv.is_open())
        throw std::runtime_error("Cannot open results/baseline.csv");

    csv << "dataset,algorithm,chunk_size,entropy,"
           "avg_ratio,avg_latency_ms,throughput_mbps\n";

    for (const auto& r : allResults) {
        csv << r.dataset      << ","
            << r.algorithm    << ","
            << r.chunkSize    << ","
            << r.entropy      << ","
            << r.avgRatio     << ","
            << r.avgLatencyMs << ","
            << r.throughputMBps << "\n";
    }

    std::cout << "\nBaseline results saved to results/baseline.csv\n";
    return 0;
}
