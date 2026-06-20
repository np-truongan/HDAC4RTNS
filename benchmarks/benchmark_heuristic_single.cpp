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

using Clock = std::chrono::high_resolution_clock;

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
    double      cpuMs;
    long        peakRssKb;
};

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
            chunkIndex++,
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
    size_t totalOriginal = 0, totalCompressed = 0;
    double totalLatency  = 0;
    double totalCpu      = 0;
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

        totalOriginal   += len;
        totalCompressed += meas.compressed.size();
        totalLatency    += meas.wallMs;
        totalCpu        += meas.cpuMs;
        peakRss          = std::max(peakRss, meas.peakRssKb);
        ++count;
    }

    double avgRatio   = static_cast<double>(totalCompressed) / totalOriginal;
    double avgLatency = totalLatency / count;
    double throughput = (totalOriginal / (1024.0 * 1024.0)) /
                        (totalLatency  / 1000.0);
    double avgCpu = totalCpu / count;
    return { avgRatio, avgLatency, throughput, avgCpu, peakRss };
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

    double n          = static_cast<double>(log.size());
    double avgRatio   = static_cast<double>(totalComp) / totalOrig;
    double avgLatency = totalLatency / n;
    double throughput = (totalOrig / (1024.0 * 1024.0)) /
                        (totalLatency / 1000.0);
    double avgCpu = totalCpu / n;
    return { avgRatio, avgLatency, throughput, avgCpu, peakRss };
}

void printChunkLog(const std::vector<ChunkLog>& log, size_t maxRows = 10) {
    std::cout << std::fixed << std::setprecision(4);
    std::cout << "\n"
              << std::setw(6)  << "Chunk"
              << std::setw(10) << "Entropy"
              << std::setw(12) << "Smoothness"
              << std::setw(8)  << "Algo"
              << std::setw(10) << "Preproc"
              << std::setw(10) << "Ratio"
              << std::setw(12) << "Lat (ms)"
              << std::setw(12) << "CPU (ms)"
              << std::setw(10) << "RSS(KB)"
              << "\n";
    std::cout << std::string(90, '-') << "\n";

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
                  << std::setw(12) << e.cpuMs
                  << std::setw(10) << e.peakRssKb
                  << "\n";
    }

    if (log.size() > maxRows)
        std::cout << "  ... (" << log.size() - maxRows
                  << " more chunks, see CSV)\n";
}

int main() {
    const size_t DATA_SIZE  = 1 << 20;
    const size_t CHUNK_SIZE = 4096;

    EngineConfig cfg;
    Chunk jsonData = generateJSON(DATA_SIZE);

    double dataEntropy    = computeEntropy(
        Chunk(jsonData.begin(), jsonData.begin() + CHUNK_SIZE));
    double dataSmoothness = computeSmoothness(
        Chunk(jsonData.begin(), jsonData.begin() + CHUNK_SIZE));

    std::cout << "========================================\n";
    std::cout << "  Heuristic Probe Validation: JSON Workload\n";
    std::cout << "========================================\n";
    std::cout << "Dataset entropy   : " << std::fixed
              << std::setprecision(4) << dataEntropy    << "\n";
    std::cout << "Dataset smoothness: " << dataSmoothness << "\n";
    std::cout << "Chunk size        : " << CHUNK_SIZE    << " bytes\n";
    std::cout << "Engine thresholds : entropy > "
              << cfg.entropyThreshold
              << " -> LZ4 | smoothness > "
              << cfg.smoothnessThreshold
              << " -> Delta\n";

    std::cout << "\n[Adaptive] Running...\n";
    auto adaptiveLog = runAdaptive(jsonData, CHUNK_SIZE, cfg);

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

    std::cout << "\n[Static LZ4]  Running...\n";
    auto lz4Result  = runStatic(jsonData, CHUNK_SIZE, Algorithm::LZ4);
    std::cout << "[Static ZSTD] Running...\n";
    auto zstdResult = runStatic(jsonData, CHUNK_SIZE, Algorithm::ZSTD);

    std::cout << "\n========================================\n";
    std::cout << "  Summary: JSON Dataset (4KB chunks)\n";
    std::cout << "========================================\n";
    std::cout << std::fixed << std::setprecision(4);
    std::cout << std::left
              << std::setw(18) << "System"
              << std::setw(14) << "Avg Ratio"
              << std::setw(16) << "Avg Lat (ms)"
              << std::setw(16) << "Throughput MB/s"
              << std::setw(12) << "Avg CPU (ms)"
              << std::setw(10) << "RSS(KB)"
              << "\n";
    std::cout << std::string(86, '-') << "\n";

    auto printRow = [](const std::string& name, const StaticResult& r) {
        std::cout << std::left  << std::setw(18) << name
                  << std::right << std::setw(14) << r.avgRatio
                  << std::setw(16) << r.avgLatencyMs
                  << std::setw(16) << r.throughputMBps
                  << std::setw(12) << r.avgCpuMs
                  << std::setw(10) << r.peakRssKb
                  << "\n";
    };

    printRow("Adaptive",    adaptiveAgg);
    printRow("Static LZ4",  lz4Result);
    printRow("Static ZSTD", zstdResult);

    std::ofstream csv("results/heuristic_single_workload.csv");
    csv << "chunk_index,entropy,smoothness,algorithm,preprocess,"
           "original_bytes,compressed_bytes,ratio,latency_ms,cpu_ms,peak_rss_kb\n";

    for (const auto& e : adaptiveLog) {
        csv << e.index          << ","
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

    std::cout << "\nPer-chunk log saved to results/heuristic_single_workload.csv\n";
    return 0;
}