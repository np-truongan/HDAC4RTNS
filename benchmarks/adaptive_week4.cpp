// benchmarks/adaptive_week4.cpp
//
// Week 4 deliverable: streaming pipeline integration.
//
// Scope (per Week 4 plan):
//   - Feed all three workload types through the async Pipeline
//     class for the first time end-to-end
//   - Interleave workloads to verify strategy switching is
//     seamless (no stalls, no ordering violations)
//   - Report per-workload latency and compression ratio from
//     a single continuous pipeline run
//   - Identify the Week 4 limitation: decision criteria are
//     independent — entropy threshold ignores whether delta
//     was applied (motivates Week 5 refinement)
//   - Output: results/pipeline_week4.csv
//
// This is the first benchmark to use Pipeline directly.
// All previous benchmarks called compressors in a simple loop.
// ============================================================

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

// ============================================================
//  Build an interleaved stream of chunks from all three
//  workload types, simulating heterogeneous real-world traffic.
//
//  Interleave pattern: Telemetry, JSON, Binary, repeat.
//  This ensures the pipeline encounters strategy switches on
//  every third chunk — the worst-case switching frequency.
// ============================================================
std::vector<StreamItem> buildInterleavedStream(
    size_t chunksPerWorkload,
    size_t chunkSize)
{
    // Generate full datasets then slice into chunks
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

// ============================================================
//  Verify ordering: results must arrive in the same order
//  items were pushed. Consumer is single-threaded so this
//  must always hold — but we check explicitly.
// ============================================================
bool verifyOrdering(const std::vector<ChunkResult>& results) {
    // Expected pattern: Telemetry, JSON, Binary, repeat
    const std::string expected[] = { "Telemetry", "JSON", "Binary" };
    for (size_t i = 0; i < results.size(); ++i) {
        if (results[i].workloadType != expected[i % 3])
            return false;
    }
    return true;
}

// ============================================================
//  Per-workload breakdown from a mixed pipeline result set
// ============================================================
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

        // Find dominant strategy
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

// ============================================================
//  Demonstrate the Week 4 limitation explicitly.
//
//  The current engine decides algorithm on raw entropy before
//  considering that delta encoding will transform the data.
//  For a smooth chunk that happens to have entropy just above
//  the LZ4 threshold, the engine picks LZ4 even though delta
//  + ZSTD would give a much better ratio.
//
//  This is logged here as a documented known limitation,
//  which Week 5 addresses via preprocessing-aware decisions.
// ============================================================
void demonstrateWeek4Limitation(const std::vector<ChunkResult>& results) {
    std::cout << "\n--- Week 4 Limitation: Independent Decision Criteria ---\n";

    // Look for telemetry chunks where delta was applied but LZ4 was chosen
    // (In our calibrated engine this shouldn't happen, but we check and report)
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

    std::cout << "\n  Note: In Week 5, bit-packing is added and the decision\n"
              << "  logic is made fully preprocessing-aware for all paths.\n";
}

// ============================================================
//  Main
// ============================================================
int main() {
    const size_t CHUNKS_PER_WORKLOAD = 85;    // ~85 chunks × 3 types = 255 total
    const size_t CHUNK_SIZE          = 4096;  // 4 KB

    EngineConfig cfg;   // default calibrated thresholds

    std::cout << "========================================\n";
    std::cout << "  Week 4: Streaming Pipeline Integration\n";
    std::cout << "========================================\n";
    std::cout << "Chunk size         : " << CHUNK_SIZE << " bytes\n";
    std::cout << "Chunks per workload: " << CHUNKS_PER_WORKLOAD << "\n";
    std::cout << "Total chunks       : " << CHUNKS_PER_WORKLOAD * 3 << "\n";
    std::cout << "Interleave pattern : Telemetry → JSON → Binary → repeat\n";

    // --------------------------------------------------------
    //  Build interleaved stream
    // --------------------------------------------------------
    auto stream = buildInterleavedStream(CHUNKS_PER_WORKLOAD, CHUNK_SIZE);

    // --------------------------------------------------------
    //  Run through the async Pipeline
    // --------------------------------------------------------
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

    // --------------------------------------------------------
    //  Ordering verification
    // --------------------------------------------------------
    std::cout << "\n--- Ordering Verification ---\n";
    bool ordered = verifyOrdering(results);
    std::cout << "  Chunk ordering preserved: "
              << (ordered ? "[OK]" : "[FAIL — ordering violated]") << "\n";

    // --------------------------------------------------------
    //  Strategy switching — show first 12 chunks to demonstrate
    //  seamless switching across workload boundaries
    // --------------------------------------------------------
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

    // --------------------------------------------------------
    //  Per-workload breakdown
    // --------------------------------------------------------
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

    // --------------------------------------------------------
    //  Aggregate pipeline metrics
    // --------------------------------------------------------
    std::cout << "\n--- Aggregate Pipeline Metrics ---\n";
    RunMetrics m = pipeline.computeMetrics("Adaptive Pipeline (Week 4)");
    printRunMetrics(m);

    // --------------------------------------------------------
    //  Document the Week 4 limitation
    // --------------------------------------------------------
    demonstrateWeek4Limitation(results);

    // --------------------------------------------------------
    //  Save CSV
    // --------------------------------------------------------
    saveResultsCSV(results, "results/pipeline_week4.csv");
    std::cout << "\nFull results saved to results/pipeline_week4.csv\n";

    return 0;
}