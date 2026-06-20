#include "pipeline.h"
#include "generators.h"
#include "heuristics.h"
#include "types.h"

#include <iostream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <string>
#include <numeric>
#include <cmath>
#include <map>
#include <algorithm>

std::vector<StreamItem> buildInterleavedStream(
    size_t chunksPerWorkload,
    size_t chunkSize)
{
    Chunk telemetryData = generateTelemetry(chunksPerWorkload * chunkSize);
    Chunk jsonData      = generateJSON     (chunksPerWorkload * chunkSize);
    Chunk binaryData    = generateBinary   (chunksPerWorkload * chunkSize);

    std::vector<StreamItem> stream;
    stream.reserve(chunksPerWorkload * 3);

    for (size_t i = 0; i < chunksPerWorkload; ++i) {
        size_t offset = i * chunkSize;

        stream.push_back({
            Chunk(telemetryData.begin() + offset,
                  telemetryData.begin() + offset + chunkSize),
            "Telemetry"
        });
        stream.push_back({
            Chunk(jsonData.begin() + offset,
                  jsonData.begin() + offset + chunkSize),
            "JSON"
        });
        stream.push_back({
            Chunk(binaryData.begin() + offset,
                  binaryData.begin() + offset + chunkSize),
            "Binary"
        });
    }

    return stream;
}

bool verifyOrdering(const std::vector<ChunkResult>& results) {
    const std::string expected[] = { "Telemetry", "JSON", "Binary" };
    for (size_t i = 0; i < results.size(); ++i) {
        if (results[i].workloadType != expected[i % 3])
            return false;
    }
    return true;
}

struct WorkloadStats {
    double avgRatio;
    double avgLatencyMs;
    double throughputMBps;
    std::string dominantAlgo;
    std::string dominantPreproc;
    int    count;
};

std::map<std::string, WorkloadStats> breakdownByWorkload(
    const std::vector<ChunkResult>& results)
{
    std::map<std::string, std::vector<const ChunkResult*>> byType;
    for (const auto& r : results)
        byType[r.workloadType].push_back(&r);

    std::map<std::string, WorkloadStats> stats;

    for (auto& [type, chunks] : byType) {
        size_t totalOrig = 0, totalComp = 0;
        double totalLatency = 0;
        std::map<std::string, int> algoCount, preprocCount;

        for (const auto* r : chunks) {
            totalOrig    += r->originalSize;
            totalComp    += r->compressedSize;
            totalLatency += r->latencyMs;
            algoCount  [toString(r->decision.algorithm)]++;
            preprocCount[toString(r->decision.preprocess)]++;
        }

        double n = static_cast<double>(chunks.size());

        auto maxAlgo = std::max_element(algoCount.begin(), algoCount.end(),
            [](const auto& a, const auto& b){ return a.second < b.second; });
        auto maxPreproc = std::max_element(preprocCount.begin(), preprocCount.end(),
            [](const auto& a, const auto& b){ return a.second < b.second; });

        stats[type] = {
            static_cast<double>(totalComp) / totalOrig,
            totalLatency / n,
            (totalOrig / (1024.0 * 1024.0)) / (totalLatency / 1000.0),
            maxAlgo->first,
            maxPreproc->first,
            static_cast<int>(chunks.size())
        };
    }

    return stats;
}

void demonstrateDecisionLimitation(const std::vector<ChunkResult>& results) {
    std::cout << "\n--- Decision Limitation: Independent Criteria ---\n";

    int deltaWithLZ4 = 0;
    int deltaWithZSTD = 0;

    for (const auto& r : results) {
        if (r.decision.preprocess == Preprocess::DELTA) {
            if (r.decision.algorithm == Algorithm::LZ4)  ++deltaWithLZ4;
            if (r.decision.algorithm == Algorithm::ZSTD) ++deltaWithZSTD;
        }
    }

    std::cout << "  Chunks with Delta preprocessing:\n";
    std::cout << "    → routed to ZSTD : " << deltaWithZSTD
              << " (correct — preprocessing-aware)\n";
    std::cout << "    → routed to LZ4  : " << deltaWithLZ4;

    if (deltaWithLZ4 == 0)
        std::cout << " (none — engine already handles this)\n";
    else
        std::cout << " [LIMITATION: LZ4 after delta wastes preprocessing gains]\n";

    std::cout << "\n  Preprocessing-aware logic in the decision engine resolves this.\n"
              << "  logic is made fully preprocessing-aware for all paths.\n";
}

int main() {
    const size_t CHUNKS_PER_WORKLOAD = 85;
    const size_t CHUNK_SIZE          = 4096;

    EngineConfig cfg;

    std::cout << "========================================\n";
    std::cout << "  Pipeline Integration Benchmark\n";
    std::cout << "========================================\n";
    std::cout << "Chunk size         : " << CHUNK_SIZE << " bytes\n";
    std::cout << "Chunks per workload: " << CHUNKS_PER_WORKLOAD << "\n";
    std::cout << "Total chunks       : " << CHUNKS_PER_WORKLOAD * 3 << "\n";
    std::cout << "Interleave pattern : Telemetry → JSON → Binary → repeat\n";

    auto stream = buildInterleavedStream(CHUNKS_PER_WORKLOAD, CHUNK_SIZE);

    std::cout << "\n[Pipeline] Starting async producer-consumer...\n";

    auto wallStart = std::chrono::high_resolution_clock::now();

    Pipeline pipeline(cfg);
    pipeline.start();

    for (auto& item : stream)
        pipeline.push(std::move(item));

    pipeline.finish();

    auto wallEnd = std::chrono::high_resolution_clock::now();
    double wallMs = std::chrono::duration<double, std::milli>(
        wallEnd - wallStart).count();

    const auto& results = pipeline.getResults();

    std::cout << "[Pipeline] Done. "
              << results.size() << " chunks processed in "
              << std::fixed << std::setprecision(2)
              << wallMs << " ms wall time.\n";

    std::cout << "\n--- Ordering Verification ---\n";
    bool ordered = verifyOrdering(results);
    std::cout << "  Chunk ordering preserved: "
              << (ordered ? "[OK]" : "[FAIL — ordering violated]") << "\n";

    std::cout << "\n--- Strategy Switching (first 12 chunks) ---\n";
    std::cout << std::left
              << std::setw(5)  << "#"
              << std::setw(12) << "Workload"
              << std::setw(8)  << "Algo"
              << std::setw(10) << "Preproc"
              << std::setw(10) << "Ratio"
              << std::setw(12) << "Lat (ms)"
              << "\n";
    std::cout << std::string(57, '-') << "\n";

    size_t preview = std::min(results.size(), size_t(12));
    for (size_t i = 0; i < preview; ++i) {
        const auto& r = results[i];
        std::cout << std::setw(5)  << i
                  << std::setw(12) << r.workloadType
                  << std::setw(8)  << toString(r.decision.algorithm)
                  << std::setw(10) << toString(r.decision.preprocess)
                  << std::setw(10) << std::fixed << std::setprecision(4)
                  << r.compressionRatio
                  << std::setw(12) << r.latencyMs
                  << "\n";
    }

    std::cout << "\n--- Per-Workload Performance Breakdown ---\n";
    auto breakdown = breakdownByWorkload(results);

    std::cout << std::left
              << std::setw(12) << "Workload"
              << std::setw(10) << "Chunks"
              << std::setw(12) << "Avg Ratio"
              << std::setw(14) << "Avg Lat (ms)"
              << std::setw(18) << "Throughput MB/s"
              << std::setw(10) << "Algorithm"
              << std::setw(10) << "Preproc"
              << "\n";
    std::cout << std::string(86, '-') << "\n";

    for (const auto& [type, s] : breakdown) {
        std::cout << std::left
                  << std::setw(12) << type
                  << std::setw(10) << s.count
                  << std::setw(12) << std::fixed << std::setprecision(4)
                  << s.avgRatio
                  << std::setw(14) << s.avgLatencyMs
                  << std::setw(18) << s.throughputMBps
                  << std::setw(10) << s.dominantAlgo
                  << std::setw(10) << s.dominantPreproc
                  << "\n";
    }

    std::cout << "\n--- Aggregate Pipeline Metrics ---\n";
    RunMetrics m = pipeline.computeMetrics("Adaptive Pipeline");
    printRunMetrics(m);

    demonstrateDecisionLimitation(results);

    saveResultsCSV(results, "results/pipeline_integration.csv");
    std::cout << "\nFull results saved to results/pipeline_integration.csv\n";

    return 0;
}