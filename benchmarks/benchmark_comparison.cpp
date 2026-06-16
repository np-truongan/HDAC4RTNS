// benchmarks/benchmark_comparison.cpp
//
// Static vs adaptive comparison benchmark.
//
// Runs all four workloads through four systems:
//   - Static LZ4
//   - Static ZSTD
//   - Static Gzip  (baseline-only, eliminated from adaptive

//   - Adaptive framework
//
// Reports per-workload and aggregate metrics for each system.
// Output: results/static_comparison.csv

#include "generators.h"
#include "heuristics.h"
#include "strategies.h"
#include "engine.h"
#include "pipeline.h"
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
#include <functional>

using Clock = std::chrono::high_resolution_clock;

// ============================================================
//  Per-workload aggregate result for one system
// ============================================================
struct WorkloadResult {
    std::string system;
    std::string workload;
    double      avgRatio;
    double      avgLatencyMs;
    double      throughputMBps;
    double      jitterMs;
    size_t      totalOriginal;
    size_t      totalCompressed;
};

// ============================================================
//  Run a static algorithm across one dataset, return result
// ============================================================
WorkloadResult runStatic(
    const std::string&                          systemName,
    const std::string&                          workloadName,
    const Chunk&                                data,
    size_t                                      chunkSize,
    std::function<Chunk(const Chunk&)>          compressFn)
{
    std::vector<double> latencies;
    size_t totalOrig = 0, totalComp = 0;

    for (size_t off = 0; off < data.size(); off += chunkSize) {
        size_t len = std::min(chunkSize, data.size() - off);
        Chunk chunk(data.begin() + off, data.begin() + off + len);

        auto t0 = Clock::now();
        Chunk compressed = compressFn(chunk);
        auto t1 = Clock::now();

        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        latencies.push_back(ms);
        totalOrig += len;
        totalComp += compressed.size();
    }

    double sumLat = std::accumulate(latencies.begin(), latencies.end(), 0.0);
    double avgLat = sumLat / latencies.size();

    double var = 0;
    for (double l : latencies) var += (l - avgLat) * (l - avgLat);
    double jitter = std::sqrt(var / latencies.size());

    double throughput = (totalOrig / (1024.0 * 1024.0)) / (sumLat / 1000.0);

    return {
        systemName, workloadName,
        static_cast<double>(totalComp) / totalOrig,
        avgLat, throughput, jitter,
        totalOrig, totalComp
    };
}

// ============================================================
//  Run the adaptive framework across one dataset
// ============================================================
WorkloadResult runAdaptive(
    const std::string& workloadName,
    const Chunk&       data,
    size_t             chunkSize,
    const EngineConfig& cfg)
{
    std::vector<double> latencies;
    size_t totalOrig = 0, totalComp = 0;

    for (size_t off = 0; off < data.size(); off += chunkSize) {
        size_t len = std::min(chunkSize, data.size() - off);
        Chunk chunk(data.begin() + off, data.begin() + off + len);

        Features f = extractFeatures(chunk);
        Decision d = decide(f, cfg);

        auto t0 = Clock::now();

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

        Chunk compressed;
        switch (d.algorithm) {
            case Algorithm::LZ4:  compressed = compressLZ4(processed);  break;
            case Algorithm::ZSTD: compressed = compressZSTD(processed); break;
            case Algorithm::GZIP: compressed = compressGZIP(processed); break;
        }

        auto t1 = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        latencies.push_back(ms);
        totalOrig += len;
        totalComp += compressed.size();
    }

    double sumLat = std::accumulate(latencies.begin(), latencies.end(), 0.0);
    double avgLat = sumLat / latencies.size();

    double var = 0;
    for (double l : latencies) var += (l - avgLat) * (l - avgLat);
    double jitter = std::sqrt(var / latencies.size());
    double throughput = (totalOrig / (1024.0 * 1024.0)) / (sumLat / 1000.0);

    return {
        "Adaptive", workloadName,
        static_cast<double>(totalComp) / totalOrig,
        avgLat, throughput, jitter,
        totalOrig, totalComp
    };
}

// ============================================================
//  Print one section of the comparison table
// ============================================================
void printTable(
    const std::string&               workload,
    const std::vector<WorkloadResult>& rows)
{
    std::cout << "\n  Workload: " << workload << "\n";
    std::cout << "  " << std::string(72, '-') << "\n";
    std::cout << std::left
              << "  " << std::setw(12) << "System"
              << std::setw(12) << "Ratio"
              << std::setw(16) << "Avg Lat (ms)"
              << std::setw(18) << "Throughput MB/s"
              << std::setw(12) << "Jitter (ms)"
              << "\n";
    std::cout << "  " << std::string(72, '-') << "\n";

    for (const auto& r : rows) {
        std::cout << "  " << std::left
                  << std::setw(12) << r.system
                  << std::setw(12) << std::fixed << std::setprecision(4)
                  << r.avgRatio
                  << std::setw(16) << r.avgLatencyMs
                  << std::setw(18) << r.throughputMBps
                  << std::setw(12) << r.jitterMs
                  << "\n";
    }
}

// ============================================================
//  Print "adaptive vs best static" summary
// ============================================================
void printAdaptiveSummary(
    const std::map<std::string,
    std::vector<WorkloadResult>>& byWorkload)
{
    std::cout << "\n--- Adaptive vs Best Static Baseline ---\n";
    std::cout << std::left
              << std::setw(12) << "Workload"
              << std::setw(14) << "Adaptive"
              << std::setw(14) << "Best Static"
              << std::setw(12) << "Best System"
              << std::setw(12) << "Ratio Gain"
              << "\n";
    std::cout << std::string(64, '-') << "\n";

    for (const auto& [workload, rows] : byWorkload) {
        const WorkloadResult* adaptive = nullptr;
        const WorkloadResult* bestStatic = nullptr;

        for (const auto& r : rows) {
            if (r.system == "Adaptive") {
                adaptive = &r;
            } else {
                if (!bestStatic || r.avgRatio < bestStatic->avgRatio)
                    bestStatic = &r;
            }
        }

        if (!adaptive || !bestStatic) continue;

        double gain = (bestStatic->avgRatio - adaptive->avgRatio)
                      / bestStatic->avgRatio * 100.0;

        std::cout << std::left
                  << std::setw(12) << workload
                  << std::setw(14) << std::fixed << std::setprecision(4)
                  << adaptive->avgRatio
                  << std::setw(14) << bestStatic->avgRatio
                  << std::setw(12) << bestStatic->system
                  << std::setw(12) << std::setprecision(1)
                  << (gain >= 0
                      ? "+" + std::to_string(static_cast<int>(gain)) + "%"
                      : std::to_string(static_cast<int>(gain)) + "%")
                  << "\n";
    }
}

// ============================================================
//  Main
// ============================================================
int main() {
    const size_t DATA_SIZE  = 1 << 20;   // 1 MB per workload
    const size_t CHUNK_SIZE = 4096;

    EngineConfig cfg;

    std::cout << "========================================\n";
    std::cout << "  Static vs Adaptive Comparison\n";
    std::cout << "========================================\n";
    std::cout << "Data size  : 1 MB per workload\n";
    std::cout << "Chunk size : " << CHUNK_SIZE << " bytes\n";
    std::cout << "Systems    : Static LZ4, Static ZSTD, Static Gzip, Adaptive\n";
    std::cout << "Workloads  : Telemetry, JSON, Binary, Nibble\n";
    std::cout << "\nNote: Gzip included as historical baseline only.\n"
              << "      Eliminated from adaptive framework on latency grounds.\n"
              << "      due to latency being 10-100x higher than LZ4/ZSTD.\n";

    // --------------------------------------------------------
    //  Generate datasets
    // --------------------------------------------------------
    std::vector<std::pair<std::string, Chunk>> datasets = {
        { "Telemetry", generateTelemetry(DATA_SIZE) },
        { "JSON",      generateJSON(DATA_SIZE)      },
        { "Binary",    generateBinary(DATA_SIZE)    },
        { "Nibble",    generateNibble(DATA_SIZE)    }
    };

    // --------------------------------------------------------
    //  Run all systems across all workloads
    // --------------------------------------------------------
    std::map<std::string, std::vector<WorkloadResult>> byWorkload;
    std::vector<WorkloadResult> allResults;

    for (auto& [name, data] : datasets) {
        std::vector<WorkloadResult> rows;

        rows.push_back(runStatic("LZ4",  name, data, CHUNK_SIZE, compressLZ4));
        rows.push_back(runStatic("ZSTD", name, data, CHUNK_SIZE, compressZSTD));
        rows.push_back(runStatic("Gzip", name, data, CHUNK_SIZE, compressGZIP));
        rows.push_back(runAdaptive(name, data, CHUNK_SIZE, cfg));

        byWorkload[name] = rows;
        for (const auto& r : rows) allResults.push_back(r);
    }

    // --------------------------------------------------------
    //  Print per-workload comparison tables
    // --------------------------------------------------------
    std::cout << "\n--- Per-Workload Comparison ---\n";
    for (auto& [workload, rows] : byWorkload)
        printTable(workload, rows);

    // --------------------------------------------------------
    //  Adaptive vs best static summary
    // --------------------------------------------------------
    printAdaptiveSummary(byWorkload);

    // --------------------------------------------------------
    //  Save CSV
    // --------------------------------------------------------
    std::ofstream csv("results/static_comparison.csv");
    csv << "system,workload,avg_ratio,avg_latency_ms,"
           "throughput_mbps,jitter_ms,total_original,total_compressed\n";

    for (const auto& r : allResults) {
        csv << r.system          << ","
            << r.workload        << ","
            << r.avgRatio        << ","
            << r.avgLatencyMs    << ","
            << r.throughputMBps  << ","
            << r.jitterMs        << ","
            << r.totalOriginal   << ","
            << r.totalCompressed << "\n";
    }

    std::cout << "\nResults saved to results/static_comparison.csv\n";
    return 0;
}