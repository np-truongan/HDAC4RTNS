// benchmarks/benchmark_chunksize.cpp
//
// Chunk size sweep benchmark.
//
// Addresses the professor's requirement to show how chunk size
// affects compression ratio and latency across all systems and
// workloads. This provides the empirical justification for the
// 4KB default used throughout the network experiments.
//
// Sweep: 512B, 1KB, 2KB, 4KB, 8KB, 16KB, 32KB, 64KB
// Systems: LZ4, ZSTD, Gzip, Adaptive
// Workloads: Telemetry, JSON, Binary, Nibble
// Metric: avg compression ratio, avg latency per chunk (ms),
//         throughput (MB/s), heuristic stability (does the
//         engine make the same decision on every chunk?)
//
// Output: results/chunksize_sweep.csv

#include "generators.h"
#include "heuristics.h"
#include "strategies.h"
#include "engine.h"
#include "stats.h"
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
//  Result for one (system, workload, chunkSize) combination
// ============================================================
struct SweepResult {
    std::string system;
    std::string workload;
    size_t      chunkSize;
    double      avgRatio;
    double      avgLatencyMs;
    double      throughputMBps;
    double      latencyStdev;
    // Adaptive-only: fraction of chunks that got the same
    // decision as the 4KB baseline (measures stability)
    double      decisionConsistency;
};

// ============================================================
//  Run a static algorithm across one dataset at one chunk size
// ============================================================
SweepResult runStatic(
    const std::string&                 systemName,
    const std::string&                 workloadName,
    const Chunk&                       data,
    size_t                             chunkSize,
    std::function<Chunk(const Chunk&)> compressFn)
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
    double stdev = std::sqrt(var / latencies.size());

    double throughput = (totalOrig / (1024.0 * 1024.0)) / (sumLat / 1000.0);

    return {
        systemName, workloadName, chunkSize,
        static_cast<double>(totalComp) / totalOrig,
        avgLat, throughput, stdev,
        1.0   // not applicable for static systems
    };
}

// ============================================================
//  Run the adaptive framework at one chunk size.
//  Also records per-chunk decisions to measure how chunk size
//  affects heuristic stability — small chunks have noisier
//  entropy estimates and may produce inconsistent decisions.
// ============================================================
SweepResult runAdaptive(
    const std::string&  workloadName,
    const Chunk&        data,
    size_t              chunkSize,
    const EngineConfig& cfg,
    // Reference decision from 4KB chunks for consistency check
    Algorithm           referenceAlgo,
    Preprocess          referencePrep)
{
    std::vector<double> latencies;
    size_t totalOrig = 0, totalComp = 0;
    int    consistent = 0, total = 0;

    for (size_t off = 0; off < data.size(); off += chunkSize) {
        size_t len = std::min(chunkSize, data.size() - off);
        Chunk chunk(data.begin() + off, data.begin() + off + len);

        Features f = extractFeatures(chunk);
        Decision d = decide(f, cfg);

        // Track decision consistency vs 4KB reference
        if (d.algorithm == referenceAlgo &&
            d.preprocess == referencePrep)
            ++consistent;
        ++total;

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
    double stdev = std::sqrt(var / latencies.size());

    double throughput = (totalOrig / (1024.0 * 1024.0)) / (sumLat / 1000.0);
    double consistency = static_cast<double>(consistent) / total;

    return {
        "Adaptive", workloadName, chunkSize,
        static_cast<double>(totalComp) / totalOrig,
        avgLat, throughput, stdev,
        consistency
    };
}

// ============================================================
//  Get the adaptive decision for a workload at 4KB
//  (used as reference for decision consistency measurement)
// ============================================================
std::pair<Algorithm, Preprocess> getReference4KBDecision(
    const Chunk&        data,
    const EngineConfig& cfg)
{
    // Sample the first 4KB chunk
    size_t len = std::min(size_t(4096), data.size());
    Chunk  chunk(data.begin(), data.begin() + len);
    Features f = extractFeatures(chunk);
    Decision d = decide(f, cfg);
    return { d.algorithm, d.preprocess };
}

// ============================================================
//  Print a formatted sweep table for one workload
// ============================================================
void printSweepTable(
    const std::string&             workload,
    const std::vector<SweepResult>& results)
{
    std::cout << "\n  Workload: " << workload << "\n";
    std::cout << "  " << std::string(88, '-') << "\n";
    std::cout << std::left
              << "  " << std::setw(10) << "ChunkSz"
              << std::setw(12) << "System"
              << std::setw(12) << "Ratio"
              << std::setw(16) << "Avg Lat (ms)"
              << std::setw(18) << "Throughput MB/s"
              << std::setw(16) << "Lat Stdev"
              << std::setw(12) << "Consistency"
              << "\n";
    std::cout << "  " << std::string(88, '-') << "\n";

    size_t lastChunk = 0;
    for (const auto& r : results) {
        if (r.chunkSize != lastChunk && lastChunk != 0)
            std::cout << "\n";
        lastChunk = r.chunkSize;

        std::string chunkLabel = std::to_string(r.chunkSize / 1024) + "KB";
        if (r.chunkSize < 1024)
            chunkLabel = std::to_string(r.chunkSize) + "B";

        std::cout << "  " << std::left
                  << std::setw(10) << chunkLabel
                  << std::setw(12) << r.system
                  << std::setw(12) << std::fixed << std::setprecision(4)
                  << r.avgRatio
                  << std::setw(16) << r.avgLatencyMs
                  << std::setw(18) << r.throughputMBps
                  << std::setw(16) << r.latencyStdev;

        if (r.system == "Adaptive")
            std::cout << std::setw(12)
                      << std::setprecision(1)
                      << (r.decisionConsistency * 100.0) << "%";
        else
            std::cout << std::setw(12) << "N/A";

        std::cout << "\n";
    }
}

// ============================================================
//  Main
// ============================================================
int main() {
    // Dataset large enough that all chunk sizes get
    // at least 16 chunks (512B * 16 = 8KB minimum)
    const size_t DATA_SIZE = 1 << 20;   // 1 MB

    // Chunk sizes to sweep
    const std::vector<size_t> chunkSizes = {
        512,          //  0.5 KB
        1024,         //  1   KB
        2048,         //  2   KB
        4096,         //  4   KB  <- default
        8192,         //  8   KB
        16384,        // 16   KB
        32768,        // 32   KB
        65536         // 64   KB
    };

    EngineConfig cfg;

    std::cout << "========================================\n";
    std::cout << "  Chunk Size Sweep Benchmark\n";
    std::cout << "========================================\n";
    std::cout << "Data size  : " << DATA_SIZE / 1024
              << " KB per workload\n";
    std::cout << "Chunk sizes: 512B, 1KB, 2KB, 4KB, 8KB, 16KB, 32KB, 64KB\n";
    std::cout << "Systems    : LZ4, ZSTD, Gzip, Adaptive\n";
    std::cout << "Workloads  : Telemetry, JSON, Binary, Nibble\n";
    std::cout << "Consistency: % of chunks where Adaptive makes the\n"
              << "             same decision as at 4KB (reference)\n";

    // --------------------------------------------------------
    //  Generate datasets once
    // --------------------------------------------------------
    std::vector<std::pair<std::string, Chunk>> datasets = {
        { "Telemetry", generateTelemetry(DATA_SIZE) },
        { "JSON",      generateJSON(DATA_SIZE)      },
        { "Binary",    generateBinary(DATA_SIZE)    },
        { "Nibble",    generateNibble(DATA_SIZE)    }
    };

    // --------------------------------------------------------
    //  Get 4KB reference decisions for each workload
    // --------------------------------------------------------
    std::map<std::string, std::pair<Algorithm, Preprocess>> refDecisions;
    for (const auto& [name, data] : datasets)
        refDecisions[name] = getReference4KBDecision(data, cfg);

    // --------------------------------------------------------
    //  Run sweep
    // --------------------------------------------------------
    // Organise results by workload for printing
    std::map<std::string, std::vector<SweepResult>> byWorkload;
    std::vector<SweepResult> allResults;

    for (size_t cs : chunkSizes) {
        std::string label = (cs < 1024)
            ? std::to_string(cs) + "B"
            : std::to_string(cs / 1024) + "KB";
        std::cout << "\n[Chunk size: " << label << "] Running...";
        std::cout.flush();

        for (const auto& [name, data] : datasets) {
            auto [refAlgo, refPrep] = refDecisions[name];

            auto lz4  = runStatic("LZ4",  name, data, cs, compressLZ4);
            auto zstd = runStatic("ZSTD", name, data, cs, compressZSTD);
            auto gzip = runStatic("Gzip", name, data, cs, compressGZIP);
            auto adap = runAdaptive(name, data, cs, cfg, refAlgo, refPrep);

            byWorkload[name].push_back(lz4);
            byWorkload[name].push_back(zstd);
            byWorkload[name].push_back(gzip);
            byWorkload[name].push_back(adap);

            allResults.push_back(lz4);
            allResults.push_back(zstd);
            allResults.push_back(gzip);
            allResults.push_back(adap);
        }
        std::cout << " done\n";
    }

    // --------------------------------------------------------
    //  Print per-workload tables
    // --------------------------------------------------------
    std::cout << "\n\n========================================\n";
    std::cout << "  Results by Workload\n";
    std::cout << "========================================\n";

    for (auto& [workload, results] : byWorkload)
        printSweepTable(workload, results);

    // --------------------------------------------------------
    //  Print ratio-vs-chunksize summary for Adaptive only
    //  (the key chart for the thesis)
    // --------------------------------------------------------
    std::cout << "\n\n--- Adaptive Ratio vs Chunk Size (all workloads) ---\n";
    std::cout << std::left
              << std::setw(10) << "ChunkSz"
              << std::setw(14) << "Telemetry"
              << std::setw(14) << "JSON"
              << std::setw(14) << "Binary"
              << std::setw(14) << "Nibble"
              << "\n";
    std::cout << std::string(66, '-') << "\n";

    for (size_t cs : chunkSizes) {
        std::string label = (cs < 1024)
            ? std::to_string(cs) + "B"
            : std::to_string(cs / 1024) + "KB";
        std::cout << std::left << std::setw(10) << label;

        // Fixed order
    const std::vector<std::string> workloadOrder = {"Telemetry","JSON","Binary","Nibble"};
    for (const auto& name : workloadOrder) {
        const auto& results = byWorkload.at(name);
            for (const auto& r : results) {
                if (r.chunkSize == cs && r.system == "Adaptive") {
                    std::cout << std::setw(14) << std::fixed
                              << std::setprecision(4) << r.avgRatio;
                }
            }
        }
        std::cout << "\n";
    }

    // --------------------------------------------------------
    //  Print latency-vs-chunksize summary for Adaptive only
    // --------------------------------------------------------
    std::cout << "\n--- Adaptive Avg Latency (ms) vs Chunk Size ---\n";
    std::cout << std::left
              << std::setw(10) << "ChunkSz"
              << std::setw(14) << "Telemetry"
              << std::setw(14) << "JSON"
              << std::setw(14) << "Binary"
              << std::setw(14) << "Nibble"
              << "\n";
    std::cout << std::string(66, '-') << "\n";

    for (size_t cs : chunkSizes) {
        std::string label = (cs < 1024)
            ? std::to_string(cs) + "B"
            : std::to_string(cs / 1024) + "KB";
        std::cout << std::left << std::setw(10) << label;

        // Fixed order
    const std::vector<std::string> workloadOrder = {"Telemetry","JSON","Binary","Nibble"};
    for (const auto& name : workloadOrder) {
        const auto& results = byWorkload.at(name);
            for (const auto& r : results) {
                if (r.chunkSize == cs && r.system == "Adaptive") {
                    std::cout << std::setw(14) << std::fixed
                              << std::setprecision(4) << r.avgLatencyMs;
                }
            }
        }
        std::cout << "\n";
    }

    // --------------------------------------------------------
    //  Save CSV
    // --------------------------------------------------------
    std::ofstream csv("results/chunksize_sweep.csv");
    csv << "system,workload,chunk_size_bytes,avg_ratio,"
           "avg_latency_ms,throughput_mbps,latency_stdev,"
           "decision_consistency\n";

    for (const auto& r : allResults) {
        csv << r.system              << ","
            << r.workload            << ","
            << r.chunkSize           << ","
            << r.avgRatio            << ","
            << r.avgLatencyMs        << ","
            << r.throughputMBps      << ","
            << r.latencyStdev        << ","
            << r.decisionConsistency << "\n";
    }

    std::cout << "\nFull results saved to results/chunksize_sweep.csv\n";
    return 0;
}