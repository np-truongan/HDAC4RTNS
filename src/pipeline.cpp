#include "pipeline.h"
#include "heuristics.h"
#include "strategies.h"
#include "resource_stats.h"

#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <numeric>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <stdexcept>

struct Pipeline::Impl {
    EngineConfig              cfg;
    std::queue<StreamItem>    queue;
    std::mutex                queueMutex;
    std::condition_variable   cv;
    bool                      done = false;
    std::thread               consumerThread;
    std::vector<ChunkResult>  results;
    std::mutex                resultsMutex;

    explicit Impl(EngineConfig c) : cfg(c) {}

    void consumerLoop() {
        while (true) {
            std::unique_lock<std::mutex> lock(queueMutex);
            cv.wait(lock, [this] {
                return !queue.empty() || done;
            });

            if (queue.empty() && done) break;

            StreamItem item = std::move(queue.front());
            queue.pop();
            lock.unlock();

            ChunkResult cr = processChunk(item);

            {
                std::lock_guard<std::mutex> rlock(resultsMutex);
                results.push_back(std::move(cr));
            }
        }
    }

    ChunkResult processChunk(const StreamItem& item) {
        ChunkResult result;
        result.originalSize  = item.data.size();
        result.workloadType  = item.workloadType;
        result.features      = extractFeatures(item.data);
        result.decision      = decide(result.features, cfg);

        ResourceSnapshot rBefore = captureResourceSnapshot();
        auto t0 = std::chrono::high_resolution_clock::now();

        Chunk processed = item.data;

        if (result.decision.preprocess == Preprocess::DELTA) {
            processed = deltaEncode(processed);
        } else if (result.decision.preprocess == Preprocess::BITPACK) {
            bool eligible = true;
            for (Byte b : processed) {
                if (b > 15) { eligible = false; break; }
            }
            if (eligible)
                processed = bitPackEncode(processed);
            else
                result.decision.preprocess = Preprocess::NONE;
        }

        Chunk compressed;
        switch (result.decision.algorithm) {
            case Algorithm::LZ4:  compressed = compressLZ4(processed);  break;
            case Algorithm::ZSTD: compressed = compressZSTD(processed); break;
            case Algorithm::GZIP: compressed = compressGZIP(processed); break;
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        ResourceSnapshot rAfter = captureResourceSnapshot();
        ResourceDelta    rDelta = diffResourceSnapshot(rBefore, rAfter, item.workloadType);

        if (compressed.empty())
            throw std::runtime_error("Compression failed for chunk");

        result.compressedSize  = compressed.size();
        result.compressionRatio =
            static_cast<double>(compressed.size()) /
            static_cast<double>(item.data.size());
        result.compressedData = compressed;
        result.latencyMs =
            std::chrono::duration<double, std::milli>(t1 - t0).count();
        result.cpuTimeMs = rDelta.cpuTimeMs;
        result.peakRssKb = rDelta.peakRssKb;

        return result;
    }
};

Pipeline::Pipeline(EngineConfig cfg) : impl(new Impl(cfg)) {}

Pipeline::~Pipeline() {
    if (impl) {
        {
            std::lock_guard<std::mutex> lock(impl->queueMutex);
            impl->done = true;
        }
        impl->cv.notify_all();
        if (impl->consumerThread.joinable())
            impl->consumerThread.join();
        delete impl;
    }
}

void Pipeline::start() {
    impl->consumerThread = std::thread(&Impl::consumerLoop, impl);
}

void Pipeline::push(StreamItem item) {
    {
        std::lock_guard<std::mutex> lock(impl->queueMutex);
        impl->queue.push(std::move(item));
    }
    impl->cv.notify_one();
}

void Pipeline::finish() {
    {
        std::lock_guard<std::mutex> lock(impl->queueMutex);
        impl->done = true;
    }
    impl->cv.notify_all();
    if (impl->consumerThread.joinable())
        impl->consumerThread.join();
}

const std::vector<ChunkResult>& Pipeline::getResults() const {
    return impl->results;
}

RunMetrics Pipeline::computeMetrics(const std::string& systemName) const {
    return aggregateResults(impl->results, systemName);
}

bool Pipeline::tryPopResult(ChunkResult& out) {
    std::lock_guard<std::mutex> lock(impl->resultsMutex);
    if (impl->results.empty()) return false;
    out = std::move(impl->results.front());
    impl->results.erase(impl->results.begin());
    return true;
}

RunMetrics aggregateResults(
    const std::vector<ChunkResult>& results,
    const std::string& systemName)
{
    if (results.empty()) return {};

    RunMetrics m;
    m.systemName = systemName;

    double sumRatio = 0, sumLatency = 0, sumThroughput = 0, sumCpuTime = 0;
    double totalOriginal = 0, totalCompressed = 0;
    long   peakRss = 0;

    for (const auto& r : results) {
        sumRatio     += r.compressionRatio;
        sumLatency   += r.latencyMs;
        sumCpuTime   += r.cpuTimeMs;
        peakRss       = std::max(peakRss, r.peakRssKb);
        totalOriginal   += r.originalSize;
        totalCompressed += r.compressedSize;

        double throughput =
            (r.originalSize / (1024.0 * 1024.0)) /
            (r.latencyMs / 1000.0);
        sumThroughput += throughput;
    }

    size_t n = results.size();
    m.avgCompressionRatio = sumRatio / n;
    m.avgLatencyMs        = sumLatency / n;
    m.avgThroughputMBps   = sumThroughput / n;
    m.avgCpuTimeMs        = sumCpuTime / n;
    m.peakRssKb           = peakRss;
    m.totalOriginalMB     = totalOriginal / (1024.0 * 1024.0);
    m.totalCompressedMB   = totalCompressed / (1024.0 * 1024.0);

    double variance = 0;
    for (const auto& r : results)
        variance += std::pow(r.latencyMs - m.avgLatencyMs, 2);
    m.jitterMs = std::sqrt(variance / n);

    return m;
}

void printRunMetrics(const RunMetrics& m) {
    std::cout << std::fixed << std::setprecision(4);
    std::cout << "System            : " << m.systemName        << "\n";
    std::cout << "Avg Ratio         : " << m.avgCompressionRatio << "\n";
    std::cout << "Avg Latency (ms)  : " << m.avgLatencyMs       << "\n";
    std::cout << "Jitter (ms)       : " << m.jitterMs           << "\n";
    std::cout << "Avg Throughput    : " << m.avgThroughputMBps  << " MB/s\n";
    std::cout << "Avg CPU (ms)      : " << m.avgCpuTimeMs       << "\n";
    std::cout << "Peak RSS (KB)     : " << m.peakRssKb          << "\n";
    std::cout << "Total Original    : " << m.totalOriginalMB    << " MB\n";
    std::cout << "Total Compressed  : " << m.totalCompressedMB  << " MB\n";
}

void saveResultsCSV(
    const std::vector<ChunkResult>& results,
    const std::string& filepath)
{
    std::ofstream f(filepath);
    if (!f.is_open())
        throw std::runtime_error("Cannot open CSV file: " + filepath);

    f << "workload,original_bytes,preprocessed_bytes,compressed_bytes,"
         "compression_ratio,latency_ms,"
         "entropy,smoothness,algorithm,preprocess,"
         "cpu_time_ms,peak_rss_kb\n";

    for (const auto& r : results) {
        size_t preprocessedSize = 0;
        switch (r.decision.preprocess) {
            case Preprocess::NONE:    preprocessedSize = r.originalSize;    break;
            case Preprocess::DELTA:   preprocessedSize = r.originalSize;    break;
            case Preprocess::BITPACK: preprocessedSize = r.originalSize / 2; break;
        }

        f << r.workloadType          << ","
          << r.originalSize          << ","
          << preprocessedSize        << ","
          << r.compressedSize        << ","
          << r.compressionRatio      << ","
          << r.latencyMs             << ","
          << r.features.entropy      << ","
          << r.features.smoothness   << ","
          << toString(r.decision.algorithm)  << ","
          << toString(r.decision.preprocess) << ","
          << r.cpuTimeMs              << ","
          << r.peakRssKb              << "\n";
    }
}