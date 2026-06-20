#include "pipeline.h"
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

std::vector<StreamItem> buildStream(size_t chunksPerWorkload, size_t chunkSize) {
    Chunk telemetry = generateTelemetry(chunksPerWorkload * chunkSize);
    Chunk json      = generateJSON     (chunksPerWorkload * chunkSize);
    Chunk binary    = generateBinary   (chunksPerWorkload * chunkSize);
    Chunk nibble    = generateNibble   (chunksPerWorkload * chunkSize);

    std::vector<StreamItem> stream;
    stream.reserve(chunksPerWorkload * 4);

    for (size_t i = 0; i < chunksPerWorkload; ++i) {
        size_t off = i * chunkSize;
        stream.push_back({ Chunk(telemetry.begin()+off, telemetry.begin()+off+chunkSize), "Telemetry" });
        stream.push_back({ Chunk(json.begin()     +off, json.begin()     +off+chunkSize), "JSON"      });
        stream.push_back({ Chunk(binary.begin()   +off, binary.begin()   +off+chunkSize), "Binary"    });
        stream.push_back({ Chunk(nibble.begin()   +off, nibble.begin()   +off+chunkSize), "Nibble"    });
    }
    return stream;
}

struct WorkloadSummary {
    int    count        = 0;
    double avgRatio     = 0;
    double avgLatencyMs = 0;
    double throughput   = 0;
    double avgCpuMs     = 0;
    long   peakRssKb    = 0;
    int    useDelta     = 0;
    int    useBitPack   = 0;
    int    useNone      = 0;
    int    useLZ4       = 0;
    int    useZSTD      = 0;
};

std::map<std::string, WorkloadSummary> summarise(
    const std::vector<ChunkResult>& results)
{
    std::map<std::string, std::vector<const ChunkResult*>> byType;
    for (const auto& r : results)
        byType[r.workloadType].push_back(&r);

    std::map<std::string, WorkloadSummary> out;
    for (auto& [type, chunks] : byType) {
        WorkloadSummary s;
        s.count = static_cast<int>(chunks.size());
        size_t totalOrig = 0, totalComp = 0;
        double totalLat  = 0;
        double totalCpu  = 0;
        long peakRss = 0;

        for (const auto* r : chunks) {
            totalOrig += r->originalSize;
            totalComp += r->compressedSize;
            totalLat  += r->latencyMs;
            totalCpu  += r->cpuTimeMs;
            peakRss    = std::max(peakRss, r->peakRssKb);
            if (r->decision.preprocess == Preprocess::DELTA)   ++s.useDelta;
            if (r->decision.preprocess == Preprocess::BITPACK) ++s.useBitPack;
            if (r->decision.preprocess == Preprocess::NONE)    ++s.useNone;
            if (r->decision.algorithm  == Algorithm::LZ4)      ++s.useLZ4;
            if (r->decision.algorithm  == Algorithm::ZSTD)     ++s.useZSTD;
        }
        s.avgRatio     = static_cast<double>(totalComp) / totalOrig;
        s.avgLatencyMs = totalLat / s.count;
        s.throughput   = (totalOrig / (1024.0 * 1024.0)) / (totalLat / 1000.0);
        s.avgCpuMs     = totalCpu / s.count;
        s.peakRssKb    = peakRss;
        out[type] = s;
    }
    return out;
}

void showBitPackBenefit(size_t chunks, size_t chunkSize) {
    std::cout << "\n--- BitPack Preprocessing Benefit (Nibble data) ---\n";
    std::cout << "  Comparing: BitPack+ZSTD  vs  raw ZSTD  vs  raw LZ4\n\n";

    Chunk nibble = generateNibble(chunks * chunkSize);

    size_t origTotal = 0, bpZstd = 0, rawZstd = 0, rawLz4 = 0;
    double totalCpuBp = 0, totalCpuZstd = 0, totalCpuLz4 = 0;
    long peakRssBp = 0, peakRssZstd = 0, peakRssLz4 = 0;

    for (size_t i = 0; i < chunks; ++i) {
        size_t off = i * chunkSize;
        Chunk chunk(nibble.begin() + off, nibble.begin() + off + chunkSize);

        auto measBp = measureCompression([&]() {
            Chunk packed = bitPackEncode(chunk);
            return compressZSTD(packed);
        });
        auto measZstd = measureCompression([&]() { return compressZSTD(chunk); });
        auto measLz4  = measureCompression([&]() { return compressLZ4(chunk); });

        origTotal += chunk.size();
        bpZstd    += measBp.compressed.size();
        rawZstd   += measZstd.compressed.size();
        rawLz4    += measLz4.compressed.size();
        totalCpuBp += measBp.cpuMs;
        totalCpuZstd += measZstd.cpuMs;
        totalCpuLz4 += measLz4.cpuMs;
        peakRssBp = std::max(peakRssBp, measBp.peakRssKb);
        peakRssZstd = std::max(peakRssZstd, measZstd.peakRssKb);
        peakRssLz4 = std::max(peakRssLz4, measLz4.peakRssKb);
    }

    double bpRatio   = static_cast<double>(bpZstd)  / origTotal;
    double zstdRatio = static_cast<double>(rawZstd)  / origTotal;
    double lz4Ratio  = static_cast<double>(rawLz4)   / origTotal;

    double avgCpuBp = totalCpuBp / chunks;
    double avgCpuZstd = totalCpuZstd / chunks;
    double avgCpuLz4 = totalCpuLz4 / chunks;

    std::cout << std::fixed << std::setprecision(4);
    std::cout << "  BitPack + ZSTD : ratio = " << bpRatio
              << "  (" << bpZstd  << " / " << origTotal << " bytes)"
              << "  CPU avg = " << avgCpuBp << " ms"
              << "  RSS peak = " << peakRssBp << " KB\n";
    std::cout << "  Raw ZSTD       : ratio = " << zstdRatio
              << "  (" << rawZstd << " / " << origTotal << " bytes)"
              << "  CPU avg = " << avgCpuZstd << " ms"
              << "  RSS peak = " << peakRssZstd << " KB\n";
    std::cout << "  Raw LZ4        : ratio = " << lz4Ratio
              << "  (" << rawLz4  << " / " << origTotal << " bytes)"
              << "  CPU avg = " << avgCpuLz4 << " ms"
              << "  RSS peak = " << peakRssLz4 << " KB\n";

    double gainVsZstd = (1.0 - bpRatio / zstdRatio) * 100.0;
    double gainVsLz4  = (1.0 - bpRatio / lz4Ratio)  * 100.0;
    std::cout << "\n  BitPack gain vs raw ZSTD : "
              << std::setprecision(1) << gainVsZstd << "%\n";
    std::cout << "  BitPack gain vs raw LZ4  : "
              << gainVsLz4 << "%\n";
}

int main() {
    const size_t CHUNKS_PER_WORKLOAD = 64;
    const size_t CHUNK_SIZE          = 4096;

    EngineConfig cfg;

    std::cout << "========================================\n";
    std::cout << "  Preprocessing Validation Benchmark\n";
    std::cout << "========================================\n";
    std::cout << "Workloads  : Telemetry, JSON, Binary, Nibble\n";
    std::cout << "Chunk size : " << CHUNK_SIZE << " bytes\n";
    std::cout << "Thresholds : smoothness > " << cfg.smoothnessThreshold
              << " -> Delta\n";
    std::cout << "             entropy < "    << cfg.bitpackThreshold
              << "    -> BitPack (if values in [0,15])\n";
    std::cout << "             entropy > "    << cfg.entropyThreshold
              << "    -> LZ4 (no preprocessing)\n";

    showBitPackBenefit(CHUNKS_PER_WORKLOAD, CHUNK_SIZE);

    std::cout << "\n[Pipeline] Building interleaved stream...\n";
    auto stream = buildStream(CHUNKS_PER_WORKLOAD, CHUNK_SIZE);

    std::cout << "[Pipeline] Starting async producer-consumer ("
              << stream.size() << " chunks)...\n";

    auto wallStart = Clock::now();
    Pipeline pipeline(cfg);
    pipeline.start();
    for (auto& item : stream)
        pipeline.push(std::move(item));
    pipeline.finish();

    double wallMs = std::chrono::duration<double, std::milli>(
        Clock::now() - wallStart).count();

    const auto& results = pipeline.getResults();
    std::cout << "[Pipeline] Done in " << std::fixed
              << std::setprecision(2) << wallMs << " ms.\n";

    std::cout << "\n--- Strategy Activation (all four paths) ---\n";
    std::cout << std::left
              << std::setw(12) << "Workload"
              << std::setw(10) << "Chunks"
              << std::setw(10) << "Delta"
              << std::setw(10) << "BitPack"
              << std::setw(10) << "None"
              << std::setw(8)  << "LZ4"
              << std::setw(8)  << "ZSTD"
              << std::setw(12) << "Avg Ratio"
              << std::setw(12) << "Avg CPU(ms)"
              << std::setw(10) << "RSS(KB)"
              << "\n";
    std::cout << std::string(100, '-') << "\n";

    auto summary = summarise(results);

    struct Expected { std::string workload, preproc, algo; };
    std::vector<Expected> expected = {
        { "Telemetry", "Delta",   "ZSTD" },
        { "JSON",      "None",    "ZSTD" },
        { "Binary",    "None",    "LZ4"  },
        { "Nibble",    "BitPack", "ZSTD" }
    };

    bool allCorrect = true;
    for (const auto& [type, s] : summary) {
        std::cout << std::left
                  << std::setw(12) << type
                  << std::setw(10) << s.count
                  << std::setw(10) << s.useDelta
                  << std::setw(10) << s.useBitPack
                  << std::setw(10) << s.useNone
                  << std::setw(8)  << s.useLZ4
                  << std::setw(8)  << s.useZSTD
                  << std::setw(12) << std::fixed << std::setprecision(4)
                  << s.avgRatio
                  << std::setw(12) << s.avgCpuMs
                  << std::setw(10) << s.peakRssKb;

        for (const auto& e : expected) {
            if (e.workload != type) continue;
            bool ok = true;
            if (e.preproc == "Delta"   && s.useDelta   != s.count) ok = false;
            if (e.preproc == "BitPack" && s.useBitPack != s.count) ok = false;
            if (e.preproc == "None"    && s.useNone    != s.count) ok = false;
            if (e.algo    == "LZ4"     && s.useLZ4     != s.count) ok = false;
            if (e.algo    == "ZSTD"    && s.useZSTD    != s.count) ok = false;
            std::cout << (ok ? "  [OK]" : "  [WARN]");
            if (!ok) allCorrect = false;
        }
        std::cout << "\n";
    }

    if (allCorrect)
        std::cout << "\n  All four preprocessing paths activated correctly.\n";

    std::cout << "\n--- Per-Workload Performance ---\n";
    std::cout << std::left
              << std::setw(12) << "Workload"
              << std::setw(14) << "Avg Ratio"
              << std::setw(16) << "Avg Lat (ms)"
              << std::setw(18) << "Throughput MB/s"
              << std::setw(12) << "Avg CPU(ms)"
              << std::setw(10) << "RSS(KB)"
              << "\n";
    std::cout << std::string(82, '-') << "\n";

    for (const auto& [type, s] : summary) {
        std::cout << std::left
                  << std::setw(12) << type
                  << std::setw(14) << std::fixed << std::setprecision(4)
                  << s.avgRatio
                  << std::setw(16) << s.avgLatencyMs
                  << std::setw(18) << s.throughput
                  << std::setw(12) << s.avgCpuMs
                  << std::setw(10) << s.peakRssKb
                  << "\n";
    }

    std::cout << "\n--- Aggregate Pipeline Metrics ---\n";
    RunMetrics m = pipeline.computeMetrics("Adaptive Pipeline");
    printRunMetrics(m);

    saveResultsCSV(results, "results/preprocessing_validation.csv");
    std::cout << "\nFull results saved to results/preprocessing_validation.csv\n";
    return 0;
}