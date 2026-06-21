#include "generators.h"
#include "heuristics.h"
#include "strategies.h"
#include "engine.h"
#include "pipeline.h"
#include "resource_stats.h"
#include "data_loader.h"
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

WorkloadResult runStatic(
    const std::string&                          systemName,
    const std::string&                          workloadName,
    const Chunk&                                data,
    size_t                                      chunkSize,
    std::function<Chunk(const Chunk&)>          compressFn)
{
    std::vector<double> latencies, cpuTimes;
    size_t totalOrig = 0, totalComp = 0;
    long peakRss = 0;

    for (size_t off = 0; off < data.size(); off += chunkSize) {
        size_t len = std::min(chunkSize, data.size() - off);
        Chunk chunk(data.begin() + off, data.begin() + off + len);

        auto meas = measureCompression([&]() { return compressFn(chunk); });
        if (meas.compressed.empty()) continue;

        latencies.push_back(meas.wallMs);
        cpuTimes.push_back(meas.cpuMs);
        peakRss = std::max(peakRss, meas.peakRssKb);
        totalOrig += len;
        totalComp += meas.compressed.size();
    }

    double sumLat = std::accumulate(latencies.begin(), latencies.end(), 0.0);
    double avgLat = sumLat / latencies.size();

    double var = 0;
    for (double l : latencies) var += (l - avgLat) * (l - avgLat);
    double jitter = std::sqrt(var / latencies.size());

    double throughput = (totalOrig / (1024.0 * 1024.0)) / (sumLat / 1000.0);
    double avgCpu = std::accumulate(cpuTimes.begin(), cpuTimes.end(), 0.0) / cpuTimes.size();

    return {
        systemName, workloadName,
        static_cast<double>(totalComp) / totalOrig,
        avgLat, throughput, jitter, avgCpu, peakRss,
        totalOrig, totalComp
    };
}

WorkloadResult runAdaptive(
    const std::string& workloadName,
    const Chunk&       data,
    size_t             chunkSize,
    const EngineConfig& cfg)
{
    std::vector<double> latencies, cpuTimes;
    size_t totalOrig = 0, totalComp = 0;
    long peakRss = 0;

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

        latencies.push_back(meas.wallMs);
        cpuTimes.push_back(meas.cpuMs);
        peakRss = std::max(peakRss, meas.peakRssKb);
        totalOrig += len;
        totalComp += meas.compressed.size();
    }

    double sumLat = std::accumulate(latencies.begin(), latencies.end(), 0.0);
    double avgLat = sumLat / latencies.size();

    double var = 0;
    for (double l : latencies) var += (l - avgLat) * (l - avgLat);
    double jitter = std::sqrt(var / latencies.size());

    double throughput = (totalOrig / (1024.0 * 1024.0)) / (sumLat / 1000.0);
    double avgCpu = std::accumulate(cpuTimes.begin(), cpuTimes.end(), 0.0) / cpuTimes.size();

    return {
        "Adaptive", workloadName,
        static_cast<double>(totalComp) / totalOrig,
        avgLat, throughput, jitter, avgCpu, peakRss,
        totalOrig, totalComp
    };
}

void printTable(
    const std::string&               workload,
    const std::vector<WorkloadResult>& rows)
{
    std::cout << "\n  Workload: " << workload << "\n";
    std::cout << "  " << std::string(88, '-') << "\n";
    std::cout << std::left
              << "  " << std::setw(12) << "System"
              << std::setw(12) << "Ratio"
              << std::setw(16) << "Avg Lat (ms)"
              << std::setw(18) << "Throughput MB/s"
              << std::setw(12) << "Jitter (ms)"
              << std::setw(12) << "Avg CPU (ms)"
              << std::setw(10) << "RSS (KB)"
              << "\n";
    std::cout << "  " << std::string(88, '-') << "\n";

    for (const auto& r : rows) {
        std::cout << "  " << std::left
                  << std::setw(12) << r.system
                  << std::setw(12) << std::fixed << std::setprecision(4)
                  << r.avgRatio
                  << std::setw(16) << r.avgLatencyMs
                  << std::setw(18) << r.throughputMBps
                  << std::setw(12) << r.jitterMs
                  << std::setw(12) << r.avgCpuMs
                  << std::setw(10) << r.peakRssKb
                  << "\n";
    }
}

void printAdaptiveSummary(
    const std::map<std::string, std::vector<WorkloadResult>>& byWorkload,
    const std::string&                                      label)
{
    std::cout << "\n--- Adaptive vs Best Static Baseline [" << label << "] ---\n";
    std::cout << std::left
              << std::setw(16) << "Workload"
              << std::setw(14) << "Adaptive"
              << std::setw(14) << "Best Static"
              << std::setw(14) << "Best System"
              << std::setw(12) << "Ratio Gain"
              << std::setw(12) << "CPU Gain"
              << "\n";
    std::cout << std::string(82, '-') << "\n";

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

        double ratioGain = (bestStatic->avgRatio - adaptive->avgRatio)
                           / bestStatic->avgRatio * 100.0;
        double cpuGain = (bestStatic->avgCpuMs - adaptive->avgCpuMs)
                         / bestStatic->avgCpuMs * 100.0;

        std::cout << std::left
                  << std::setw(16) << workload
                  << std::setw(14) << std::fixed << std::setprecision(4)
                  << adaptive->avgRatio
                  << std::setw(14) << bestStatic->avgRatio
                  << std::setw(14) << bestStatic->system
                  << std::setw(12) << std::setprecision(1)
                  << (ratioGain >= 0 ? "+" : "") << static_cast<int>(ratioGain) << "%"
                  << std::setw(12)
                  << (cpuGain >= 0 ? "+" : "") << static_cast<int>(cpuGain) << "%"
                  << "\n";
    }
}

std::map<std::string, std::vector<WorkloadResult>> runComparison(
    const std::vector<std::pair<std::string, Chunk>>& datasets,
    size_t                                             chunkSize,
    const EngineConfig&                                cfg,
    const std::string&                                 csvSuffix)
{
    std::map<std::string, std::vector<WorkloadResult>> byWorkload;
    std::vector<WorkloadResult> allResults;

    for (const auto& [name, data] : datasets) {
        std::vector<WorkloadResult> rows;

        rows.push_back(runStatic("LZ4",  name, data, chunkSize, compressLZ4));
        rows.push_back(runStatic("ZSTD", name, data, chunkSize, compressZSTD));
        rows.push_back(runStatic("Gzip", name, data, chunkSize, compressGZIP));
        rows.push_back(runAdaptive(name, data, chunkSize, cfg));

        byWorkload[name] = rows;
        for (const auto& r : rows) allResults.push_back(r);
    }

    std::cout << "\n--- Per-Workload Comparison [" << csvSuffix << "] ---\n";
    for (auto& [workload, rows] : byWorkload)
        printTable(workload, rows);

    printAdaptiveSummary(byWorkload, csvSuffix);

    std::string csvFile = "results/static_comparison_" + csvSuffix + ".csv";
    std::ofstream csv(csvFile);
    if (!csv.is_open()) {
        std::cerr << "Warning: Could not open " << csvFile << "\n";
        return byWorkload;
    }

    csv << "system,workload,avg_ratio,avg_latency_ms,"
           "throughput_mbps,jitter_ms,avg_cpu_ms,peak_rss_kb,total_original,total_compressed\n";

    for (const auto& r : allResults) {
        csv << r.system          << ","
            << r.workload        << ","
            << r.avgRatio        << ","
            << r.avgLatencyMs    << ","
            << r.throughputMBps  << ","
            << r.jitterMs        << ","
            << r.avgCpuMs        << ","
            << r.peakRssKb       << ","
            << r.totalOriginal   << ","
            << r.totalCompressed << "\n";
    }

    std::cout << "\nResults saved to " << csvFile << "\n";
    return byWorkload;
}

int main() {
    const size_t DATA_SIZE  = 1 << 20;
    const size_t CHUNK_SIZE = 4096;

    EngineConfig cfg;

    std::cout << "========================================\n";
    std::cout << "  Static vs Adaptive Comparison\n";
    std::cout << "  (Synthetic + Auto-loaded Real Data)\n";
    std::cout << "========================================\n";
    std::cout << "Synthetic data size : 1 MB per workload\n";
    std::cout << "Real data size      : Original file sizes (no padding)\n";
    std::cout << "Chunk size          : " << CHUNK_SIZE << " bytes\n";
    std::cout << "Systems             : Static LZ4, Static ZSTD, Static Gzip, Adaptive\n\n";

    std::cout << "========================================\n";
    std::cout << "  RUN 1: SYNTHETIC WORKLOADS\n";
    std::cout << "========================================\n";

    std::vector<std::pair<std::string, Chunk>> syntheticDatasets = {
        {"Synth_Telemetry", generateTelemetry(DATA_SIZE)},
        {"Synth_JSON",      generateJSON(DATA_SIZE)},
        {"Synth_Binary",    generateBinary(DATA_SIZE)},
        {"Synth_Nibble",    generateNibble(DATA_SIZE)}
    };

    auto synthResults = runComparison(syntheticDatasets, CHUNK_SIZE, cfg, "synthetic");

    std::cout << "\n========================================\n";
    std::cout << "  RUN 2: REAL WORKLOADS (auto-loaded)\n";
    std::cout << "========================================\n";
    std::cout << "  Note: Real files are NOT padded. Each file retains its original size.\n";
    std::cout << "  This preserves the true statistical properties of the traffic.\n\n";

    // Load real files without padding (targetBytes = 0)
    // Real files keep their original sizes.
    auto realDatasets = loadAllFilesFromDirectory("../data/", 0);

    std::map<std::string, std::vector<WorkloadResult>> realResults;
    if (realDatasets.empty()) {
        std::cerr << "No real datasets found in data/. Skipping RUN 2.\n";
    } else {
        realResults = runComparison(realDatasets, CHUNK_SIZE, cfg, "real");
    }

    std::cout << "\n========================================\n";
    std::cout << "  RESULTS SUMMARY\n";
    std::cout << "========================================\n";
    std::cout << "  Synthetic results   : results/static_comparison_synthetic.csv\n";
    std::cout << "  Real results        : results/static_comparison_real.csv\n";
    std::cout << "\n  Note: Synthetic and real workloads are presented separately.\n";
    std::cout << "  They are not directly comparable because real files are not\n";
    std::cout << "  zero-padded to 1 MB, preserving their true entropy and ratios.\n";

    std::cout << "\nAll runs completed.\n";
    return 0;
}