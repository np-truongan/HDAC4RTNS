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
#include <functional>
#include <algorithm>
#include <limits>

#include <lz4.h>
#include <zstd.h>
#include <zlib.h>

using Clock = std::chrono::high_resolution_clock;

struct ParetoPoint {
    std::string algorithm;
    std::string config;
    int         level;
    std::string workload;
    double      avgRatio;
    double      throughputMBps;
    double      avgLatencyMs;
    double      avgCpuMs;
    long        peakRssKb;
};

static Chunk compressZSTD_level(const Chunk& data, int level) {
    size_t maxSize = ZSTD_compressBound(data.size());
    Chunk out(maxSize);
    size_t n = ZSTD_compress(out.data(), maxSize,
                             data.data(), data.size(), level);
    if (ZSTD_isError(n)) return {};
    out.resize(n);
    return out;
}

static Chunk compressGzip_level(const Chunk& data, int level) {
    z_stream zs{};
    if (deflateInit2(&zs, level, Z_DEFLATED, 15|16, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK)
        return {};
    uLongf bound = deflateBound(&zs, static_cast<uLong>(data.size()));
    Chunk out(bound);
    zs.next_in   = const_cast<Bytef*>(
        reinterpret_cast<const Bytef*>(data.data()));
    zs.avail_in  = static_cast<uInt>(data.size());
    zs.next_out  = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(bound);
    int ret = deflate(&zs, Z_FINISH);
    deflateEnd(&zs);
    if (ret != Z_STREAM_END) return {};
    out.resize(zs.total_out);
    return out;
}

ParetoPoint runLevel(
    const std::string& algorithm,
    const std::string& config,
    int                level,
    const std::string& workloadName,
    const Chunk&       data,
    size_t             chunkSize,
    std::function<Chunk(const Chunk&, int)> compressFn)
{
    std::vector<double> latencies, cpuTimes;
    size_t totalOrig = 0, totalComp = 0;
    long peakRss = 0;

    for (size_t off = 0; off < data.size(); off += chunkSize) {
        size_t len = std::min(chunkSize, data.size() - off);
        Chunk chunk(data.begin() + off, data.begin() + off + len);

        auto meas = measureCompression([&]() { return compressFn(chunk, level); });
        if (meas.compressed.empty()) continue;

        latencies.push_back(meas.wallMs);
        cpuTimes.push_back(meas.cpuMs);
        peakRss = std::max(peakRss, meas.peakRssKb);
        totalOrig += len;
        totalComp += meas.compressed.size();
    }

    if (latencies.empty())
        return { algorithm, config, level, workloadName, 0, 0, 0, 0, 0 };

    double sumLat    = std::accumulate(latencies.begin(), latencies.end(), 0.0);
    double throughput = (totalOrig / (1024.0 * 1024.0)) / (sumLat / 1000.0);
    double ratio     = static_cast<double>(totalComp) / totalOrig;
    double avgCpu = std::accumulate(cpuTimes.begin(), cpuTimes.end(), 0.0) / cpuTimes.size();

    return { algorithm, config, level, workloadName,
             ratio, throughput, sumLat / latencies.size(), avgCpu, peakRss };
}

ParetoPoint runAdaptive(
    const std::string&  workloadName,
    const Chunk&        data,
    size_t              chunkSize,
    const EngineConfig& cfg)
{
    std::vector<double> latencies, cpuTimes;
    size_t totalOrig = 0, totalComp = 0;
    long peakRss = 0;

    size_t sampleLen = std::min(chunkSize, data.size());
    Chunk  sampleChunk(data.begin(), data.begin() + sampleLen);
    Features sampleF = extractFeatures(sampleChunk);
    Decision sampleD = decide(sampleF, cfg);

    std::string prepLabel;
    switch (sampleD.preprocess) {
        case Preprocess::DELTA:   prepLabel = "Delta+";  break;
        case Preprocess::BITPACK: prepLabel = "BitPack+"; break;
        case Preprocess::NONE:    prepLabel = "";         break;
    }
    std::string algoLabel;
    switch (sampleD.algorithm) {
        case Algorithm::LZ4:  algoLabel = "LZ4";    break;
        case Algorithm::ZSTD: algoLabel = "ZSTD-1"; break;
        case Algorithm::GZIP: algoLabel = "Gzip-1"; break;
    }
    std::string configLabel = "Adaptive (" + prepLabel + algoLabel + ")";

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

    if (latencies.empty())
        return { "Adaptive", configLabel, 0, workloadName, 0, 0, 0, 0, 0 };

    double sumLat    = std::accumulate(latencies.begin(), latencies.end(), 0.0);
    double throughput = (totalOrig / (1024.0 * 1024.0)) / (sumLat / 1000.0);
    double ratio     = static_cast<double>(totalComp) / totalOrig;
    double avgCpu = std::accumulate(cpuTimes.begin(), cpuTimes.end(), 0.0) / cpuTimes.size();

    return { "Adaptive", configLabel, 0, workloadName,
             ratio, throughput, sumLat / latencies.size(), avgCpu, peakRss };
}

void printSpeedMatchedComparison(
    const std::vector<ParetoPoint>& points)
{
    std::cout << "\n--- Speed-Matched Comparison ---\n";
    std::cout << "(For each workload: which static algorithm runs at the\n";
    std::cout << " same speed as Adaptive, and how do their ratios compare?)\n\n";

    std::cout << std::left
              << std::setw(12) << "Workload"
              << std::setw(28) << "Adaptive config"
              << std::setw(14) << "Adapt. ratio"
              << std::setw(14) << "Adapt. MB/s"
              << std::setw(20) << "Closest static"
              << std::setw(14) << "Static ratio"
              << std::setw(14) << "Static MB/s"
              << std::setw(14) << "Ratio gain"
              << std::setw(12) << "CPU gain"
              << "\n";
    std::cout << std::string(150, '-') << "\n";

    const std::vector<std::string> workloads =
        { "Telemetry", "JSON", "Binary", "Nibble" };

    for (const auto& wl : workloads) {
        const ParetoPoint* adaptive = nullptr;
        for (const auto& p : points)
            if (p.workload == wl && p.algorithm == "Adaptive")
                adaptive = &p;
        if (!adaptive) continue;

        const ParetoPoint* closest = nullptr;
        double minDiff = std::numeric_limits<double>::max();

        for (const auto& p : points) {
            if (p.workload != wl || p.algorithm == "Adaptive") continue;
            double diff = std::abs(p.throughputMBps - adaptive->throughputMBps);
            if (diff < minDiff) {
                minDiff  = diff;
                closest  = &p;
            }
        }
        if (!closest) continue;

        double ratioGain = (closest->avgRatio - adaptive->avgRatio)
                           / closest->avgRatio * 100.0;
        double cpuGain = (closest->avgCpuMs - adaptive->avgCpuMs)
                         / closest->avgCpuMs * 100.0;

        std::string ratioStr = (ratioGain >= 0 ? "+" : "") +
            std::to_string(static_cast<int>(std::round(ratioGain))) + "%";
        std::string cpuStr = (cpuGain >= 0 ? "+" : "") +
            std::to_string(static_cast<int>(std::round(cpuGain))) + "%";

        std::cout << std::fixed << std::setprecision(4);
        std::cout << std::left
                  << std::setw(12) << wl
                  << std::setw(28) << adaptive->config
                  << std::setw(14) << adaptive->avgRatio
                  << std::setw(14) << std::setprecision(1)
                  << adaptive->throughputMBps
                  << std::setw(20) << closest->config
                  << std::setw(14) << std::setprecision(4)
                  << closest->avgRatio
                  << std::setw(14) << std::setprecision(1)
                  << closest->throughputMBps
                  << std::setw(14) << ratioStr
                  << std::setw(12) << cpuStr
                  << "\n";
    }
}

void printParetoTable(
    const std::string&              workload,
    const std::vector<ParetoPoint>& points)
{
    std::cout << "\n  Workload: " << workload << "\n";
    std::cout << "  " << std::string(110, '-') << "\n";
    std::cout << std::left
              << "  " << std::setw(28) << "Configuration"
              << std::setw(12) << "Ratio"
              << std::setw(20) << "Throughput (MB/s)"
              << std::setw(14) << "Latency (ms)"
              << std::setw(12) << "CPU (ms)"
              << std::setw(10) << "RSS(KB)"
              << "\n";
    std::cout << "  " << std::string(110, '-') << "\n";

    for (const auto& p : points) {
        std::cout << "  " << std::left
                  << std::setw(28) << p.config
                  << std::setw(12) << std::fixed << std::setprecision(4)
                  << p.avgRatio
                  << std::setw(20) << std::setprecision(1)
                  << p.throughputMBps
                  << std::setw(14) << std::setprecision(4)
                  << p.avgLatencyMs
                  << std::setw(12) << p.avgCpuMs
                  << std::setw(10) << p.peakRssKb
                  << "\n";
    }
}

std::vector<bool> markPareto(const std::vector<ParetoPoint>& points) {
    std::vector<bool> pareto(points.size(), true);
    for (size_t i = 0; i < points.size(); ++i) {
        for (size_t j = 0; j < points.size(); ++j) {
            if (i == j) continue;
            if (points[j].avgRatio       <= points[i].avgRatio &&
                points[j].throughputMBps >= points[i].throughputMBps &&
                (points[j].avgRatio       < points[i].avgRatio ||
                 points[j].throughputMBps > points[i].throughputMBps)) {
                pareto[i] = false;
                break;
            }
        }
    }
    return pareto;
}

int main() {
    const size_t DATA_SIZE  = 1 << 20;
    const size_t CHUNK_SIZE = 4096;

    EngineConfig cfg;

    const std::vector<int> zstdLevels = { 1, 2, 3, 5, 7, 9, 12, 15, 19 };
    const std::vector<int> gzipLevels = { 1, 3, 5, 7, 9 };

    std::cout << "========================================\n";
    std::cout << "  Ratio-Speed Pareto Curve Benchmark\n";
    std::cout << "========================================\n";
    std::cout << "Chunk size  : " << CHUNK_SIZE << " bytes\n";
    std::cout << "ZSTD levels : 1,2,3,5,7,9,12,15,19\n";
    std::cout << "Gzip levels : 1,3,5,7,9\n";
    std::cout << "Adaptive    : labelled with actual per-workload config\n";
    std::cout << "\nPlot: ratio (y) vs throughput (x). Lower-left = better.\n";

    std::vector<std::pair<std::string, Chunk>> datasets = {
        { "Telemetry", generateTelemetry(DATA_SIZE) },
        { "JSON",      generateJSON(DATA_SIZE)      },
        { "Binary",    generateBinary(DATA_SIZE)    },
        { "Nibble",    generateNibble(DATA_SIZE)    }
    };

    std::vector<ParetoPoint> allPoints;

    for (auto& [name, data] : datasets) {
        std::cout << "\n[" << name << "] Running...\n";
        std::vector<ParetoPoint> workloadPoints;

        auto lz4pt = runLevel("LZ4", "LZ4", 0, name, data, CHUNK_SIZE,
            [](const Chunk& c, int) { return compressLZ4(c); });
        workloadPoints.push_back(lz4pt);

        for (int level : zstdLevels) {
            std::string cfg_label = "ZSTD-" + std::to_string(level);
            auto pt = runLevel("ZSTD", cfg_label, level, name, data, CHUNK_SIZE,
                [](const Chunk& c, int l) {
                    return compressZSTD_level(c, l); });
            workloadPoints.push_back(pt);
        }

        for (int level : gzipLevels) {
            std::string cfg_label = "Gzip-" + std::to_string(level);
            auto pt = runLevel("Gzip", cfg_label, level, name, data, CHUNK_SIZE,
                [](const Chunk& c, int l) {
                    return compressGzip_level(c, l); });
            workloadPoints.push_back(pt);
        }

        auto apt = runAdaptive(name, data, CHUNK_SIZE, cfg);
        workloadPoints.push_back(apt);

        printParetoTable(name, workloadPoints);

        auto pareto = markPareto(workloadPoints);
        std::cout << "\n  Pareto-optimal points:\n";
        for (size_t i = 0; i < workloadPoints.size(); ++i) {
            if (pareto[i]) {
                const auto& p = workloadPoints[i];
                std::cout << "    " << std::left << std::setw(28) << p.config
                          << " ratio=" << std::fixed << std::setprecision(4)
                          << p.avgRatio
                          << "  throughput=" << std::setprecision(1)
                          << p.throughputMBps << " MB/s\n";
            }
        }

        for (auto& p : workloadPoints) allPoints.push_back(p);
    }

    printSpeedMatchedComparison(allPoints);

    std::ofstream csv("results/pareto_curve.csv");
    csv << "algorithm,config,level,workload,avg_ratio,"
           "throughput_mbps,avg_latency_ms,avg_cpu_ms,peak_rss_kb\n";

    for (const auto& p : allPoints) {
        csv << p.algorithm      << ","
            << p.config         << ","
            << p.level          << ","
            << p.workload       << ","
            << p.avgRatio       << ","
            << p.throughputMBps << ","
            << p.avgLatencyMs   << ","
            << p.avgCpuMs       << ","
            << p.peakRssKb      << "\n";
    }

    std::cout << "\nResults saved to results/pareto_curve.csv\n";
    return 0;
}