#pragma once

#include "types.h"
#include "engine.h"
#include <vector>
#include <functional>

struct StreamItem {
    Chunk       data;
    std::string workloadType;   
};

class Pipeline {
public:
    explicit Pipeline(EngineConfig cfg = EngineConfig{});
    ~Pipeline();

    void start();
    void push(StreamItem item);
    void finish();

    const std::vector<ChunkResult>& getResults() const;
    RunMetrics computeMetrics(const std::string& systemName) const;
    bool tryPopResult(ChunkResult& out);

private:
    struct Impl;
    Impl* impl;
};

RunMetrics aggregateResults(
    const std::vector<ChunkResult>& results,
    const std::string& systemName);

void printRunMetrics(const RunMetrics& m);

void saveResultsCSV(
    const std::vector<ChunkResult>& results,
    const std::string& filepath);