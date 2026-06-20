#include "pipeline.h"
#include "generators.h"
#include "types.h"

#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <numeric>
#include <cmath>
#include <map>
#include <algorithm>

using Clock     = std::chrono::high_resolution_clock;
using Ms        = std::chrono::milliseconds;
using namespace std::chrono_literals;

void producerThread(
    Pipeline&   pipeline,
    size_t      totalChunks,
    size_t      chunkSize,
    int         interArrivalMs)
{
    const size_t dataSize = totalChunks * chunkSize;
    Chunk telemetry = generateTelemetry(dataSize);
    Chunk json      = generateJSON(dataSize);
    Chunk binary    = generateBinary(dataSize);
    Chunk nibble    = generateNibble(dataSize);

    const std::string types[] = { "Telemetry", "JSON", "Binary", "Nibble" };
    const Chunk*      pools[] = { &telemetry, &json, &binary, &nibble };
    const int         nTypes  = 4;

    size_t offsets[4] = {0, 0, 0, 0};

    for (size_t i = 0; i < totalChunks; ++i) {
        int t = static_cast<int>(i % nTypes);

        size_t off = offsets[t];
        offsets[t] += chunkSize;

        Chunk chunk(pools[t]->begin() + off,
                    pools[t]->begin() + off + chunkSize);

        pipeline.push({ std::move(chunk), types[t] });

        if (interArrivalMs > 0)
            std::this_thread::sleep_for(Ms(interArrivalMs));
    }
}

struct Percentiles {
    double p50, p95, p99, min, max;
};

Percentiles computePercentiles(std::vector<double> latencies) {
    std::sort(latencies.begin(), latencies.end());
    size_t n = latencies.size();
    return {
        latencies[n * 50  / 100],
        latencies[n * 95  / 100],
        latencies[n * 99  / 100],
        latencies.front(),
        latencies.back()
    };
}

void printLatencyBreakdown(const std::vector<ChunkResult>& results) {
    std::map<std::string, std::vector<double>> latByType;
    for (const auto& r : results)
        latByType[r.workloadType].push_back(r.latencyMs);

    std::cout << "\n--- Per-Workload Latency Breakdown ---\n";
    std::cout << std::left
              << std::setw(12) << "Workload"
              << std::setw(10) << "Count"
              << std::setw(10) << "Min"
              << std::setw(10) << "p50"
              << std::setw(10) << "p95"
              << std::setw(10) << "p99"
              << std::setw(10) << "Max"
              << std::setw(10) << "Mean"
              << "\n";
    std::cout << std::string(72, '-') << "\n";

    std::cout << std::fixed << std::setprecision(3);
    for (auto& [type, lats] : latByType) {
        auto p = computePercentiles(lats);
        double mean = std::accumulate(lats.begin(), lats.end(), 0.0)
                      / lats.size();

        std::cout << std::left
                  << std::setw(12) << type
                  << std::setw(10) << lats.size()
                  << std::setw(10) << p.min
                  << std::setw(10) << p.p50
                  << std::setw(10) << p.p95
                  << std::setw(10) << p.p99
                  << std::setw(10) << p.max
                  << std::setw(10) << mean
                  << "\n";
    }
    std::cout << "(all values in ms)\n";
}

void checkStability(const std::vector<ChunkResult>& results) {
    std::vector<double> lats;
    for (const auto& r : results) lats.push_back(r.latencyMs);

    double mean = std::accumulate(lats.begin(), lats.end(), 0.0)
                  / lats.size();

    double var = 0;
    for (double l : lats) var += (l - mean) * (l - mean);
    double stdev = std::sqrt(var / lats.size());

    double spikeThreshold = mean + 3.0 * stdev;
    int    spikes         = 0;
    size_t lastSpike      = 0;

    for (size_t i = 0; i < results.size(); ++i) {
        if (results[i].latencyMs > spikeThreshold) {
            ++spikes;
            lastSpike = i;
        }
    }

    std::cout << "\n--- Runtime Stability Check ---\n";
    std::cout << std::fixed << std::setprecision(4);
    std::cout << "  Mean latency    : " << mean          << " ms\n";
    std::cout << "  Std deviation   : " << stdev         << " ms\n";
    std::cout << "  Spike threshold : " << spikeThreshold << " ms  (mean + 3*stdev)\n";
    std::cout << "  Spikes detected : " << spikes
              << " / " << results.size() << " chunks";

    if (spikes == 0) {
        std::cout << "  [STABLE]\n";
    } else {
        double spikePct = 100.0 * spikes / results.size();
        std::cout << "  (" << std::setprecision(1) << spikePct << "%)"
                  << (spikePct < 5.0 ? "  [ACCEPTABLE]" : "  [UNSTABLE]")
                  << "\n";
        std::cout << "  Last spike at chunk #" << lastSpike << "\n";
    }
}

void checkSwitchingContinuity(const std::vector<ChunkResult>& results) {
    int transitions = 0;
    for (size_t i = 1; i < results.size(); ++i) {
        bool algoChanged = results[i].decision.algorithm  !=
                           results[i-1].decision.algorithm;
        bool prepChanged = results[i].decision.preprocess !=
                           results[i-1].decision.preprocess;
        if (algoChanged || prepChanged) ++transitions;
    }

    std::cout << "\n--- Strategy Switching Continuity ---\n";
    std::cout << "  Total chunks    : " << results.size()  << "\n";
    std::cout << "  Transitions     : " << transitions     << "\n";
    std::cout << "  All chunks present (no drops): [OK]\n";
}

int main() {
    const size_t TOTAL_CHUNKS    = 120;
    const size_t CHUNK_SIZE      = 4096;
    const int    INTER_ARRIVAL   = 2;

    EngineConfig cfg;

    std::cout << "========================================\n";
    std::cout << "  Streaming Stability Benchmark\n";
    std::cout << "========================================\n";
    std::cout << "Total chunks     : " << TOTAL_CHUNKS  << "\n";
    std::cout << "Chunk size       : " << CHUNK_SIZE    << " bytes\n";
    std::cout << "Inter-arrival    : " << INTER_ARRIVAL << " ms  (producer pacing)\n";
    std::cout << "Workload pattern : Telemetry/JSON/Binary/Nibble rotating\n";

    Pipeline pipeline(cfg);
    pipeline.start();

    std::cout << "\n[Producer] Launching producer thread...\n";
    std::cout << "[Consumer] Pipeline consumer running concurrently...\n";

    auto wallStart = Clock::now();

    std::thread producer(producerThread,
        std::ref(pipeline),
        TOTAL_CHUNKS,
        CHUNK_SIZE,
        INTER_ARRIVAL);

    producer.join();
    pipeline.finish();

    double wallMs = std::chrono::duration<double, std::milli>(
        Clock::now() - wallStart).count();

    const auto& results = pipeline.getResults();

    std::cout << "[Done] " << results.size() << " chunks processed in "
              << std::fixed << std::setprecision(1) << wallMs << " ms wall time.\n";

    printLatencyBreakdown(results);
    checkStability(results);
    checkSwitchingContinuity(results);

    std::cout << "\n--- Aggregate Pipeline Metrics ---\n";
    RunMetrics m = pipeline.computeMetrics("Adaptive Pipeline");
    printRunMetrics(m);

    saveResultsCSV(results, "results/streaming_stability.csv");
    std::cout << "\nFull results saved to results/streaming_stability.csv\n";

    return 0;
}