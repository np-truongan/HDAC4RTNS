#pragma once

#include "types.h"
#include "engine.h"
#include <vector>
#include <functional>

// ============================================================
//  StreamItem: what the producer puts on the queue
// ============================================================
struct StreamItem {
    Chunk       data;
    std::string workloadType;   // "Telemetry" | "JSON" | "Binary"
};

// ============================================================
//  Pipeline
//
//  Owns an internal thread-safe queue.  The producer calls
//  push(); the consumer thread drains it, runs heuristics,
//  makes a decision, applies preprocessing, compresses with
//  real library calls, and records a ChunkResult.
//
//  Usage:
//      Pipeline p;
//      p.start();
//      for (auto& item : myStream) p.push(item);
//      p.finish();
//      auto results = p.getResults();
// ============================================================
class Pipeline {
public:
    explicit Pipeline(EngineConfig cfg = EngineConfig{});
    ~Pipeline();

    void start();                       // launches consumer thread
    void push(StreamItem item);         // thread-safe enqueue
    void finish();                      // signals done, joins thread

    const std::vector<ChunkResult>& getResults() const;
    RunMetrics computeMetrics(const std::string& systemName) const;

private:
    struct Impl;
    Impl* impl;
};

// ============================================================
//  Metrics helpers
// ============================================================
RunMetrics aggregateResults(
    const std::vector<ChunkResult>& results,
    const std::string& systemName);

void printRunMetrics(const RunMetrics& m);

void saveResultsCSV(
    const std::vector<ChunkResult>& results,
    const std::string& filepath);
