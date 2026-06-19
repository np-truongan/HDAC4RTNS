#include "generators.h"
#include "heuristics.h"
#include "strategies.h"
#include "engine.h"
#include "framing.h"
#include "stats.h"
#include "types.h"
#include "resource_stats.h"

#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>
#include <string>
#include <numeric>
#include <cmath>
#include <cstring>
#include <cassert>
#include <atomic>
#include <functional>
#include <map>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

static constexpr int    PORT             = 54321;
static constexpr size_t CHUNK_SIZE       = 4096;
static constexpr size_t DATA_SIZE        = 1 << 20;
static constexpr int    N_TRIALS         = 30;
static constexpr int    CHUNKS_PER_TRIAL = 40;

// ============================================================
//  Trial mode
// ============================================================
enum class TrialMode { MIXED, SINGLE };

static std::string modeName(TrialMode m) {
    return (m == TrialMode::MIXED) ? "mixed" : "single";
}

// ============================================================
//  Static compress helper
// ============================================================
static Chunk compressStatic(
    const Chunk&  chunk,
    Algorithm     algo,
    Preprocess    prep,
    FrameHeader&  hdr)
{
    Chunk processed = chunk;

    if (prep == Preprocess::DELTA) {
        processed = deltaEncode(chunk);
    } else if (prep == Preprocess::BITPACK) {
        bool eligible = true;
        for (Byte b : processed)
            if (b > 15) { eligible = false; break; }
        if (eligible)
            processed = bitPackEncode(processed);
        else
            prep = Preprocess::NONE;
    }

    Chunk compressed;
    switch (algo) {
        case Algorithm::LZ4:  compressed = compressLZ4(processed);  break;
        case Algorithm::ZSTD: compressed = compressZSTD(processed); break;
        case Algorithm::GZIP: compressed = compressGZIP(processed); break;
    }

    hdr.algorithm      = static_cast<uint8_t>(algo);
    hdr.preprocess     = static_cast<uint8_t>(prep);
    hdr.originalSize   = static_cast<uint32_t>(chunk.size());
    hdr.compressedSize = static_cast<uint32_t>(compressed.size());

    return compressed;
}

// ============================================================
//  Decompress one received frame
// ============================================================
static Chunk decompressFrame(const FrameHeader& hdr, const Chunk& payload) {
    Algorithm  algo = static_cast<Algorithm>(hdr.algorithm);
    Preprocess prep = static_cast<Preprocess>(hdr.preprocess);

    Chunk decompressed;
    switch (algo) {
        case Algorithm::LZ4:
            decompressed = decompressLZ4(payload, hdr.originalSize); break;
        case Algorithm::ZSTD:
            decompressed = decompressZSTD(payload);                  break;
        case Algorithm::GZIP:
            decompressed = decompressGZIP(payload, hdr.originalSize); break;
    }

    if (prep == Preprocess::DELTA)
        decompressed = deltaDecode(decompressed);
    else if (prep == Preprocess::BITPACK)
        decompressed = bitPackDecode(decompressed, hdr.originalSize);

    return decompressed;
}

// ============================================================
//  TrialChunk — one chunk with its metadata
// ============================================================
struct TrialChunk {
    Chunk       data;
    std::string workload;
    uint8_t     workloadIdx;
};

// Build a mixed stream: chunksPerWorkload chunks per workload, interleaved.
static std::vector<TrialChunk> buildMixedStream(
    const std::vector<std::pair<std::string, Chunk>>& datasets,
    int chunksPerWorkload)
{
    std::vector<TrialChunk> stream;
    for (size_t d = 0; d < datasets.size(); ++d) {
        const auto& [name, data] = datasets[d];
        uint8_t idx = workloadIndex(name);
        for (int c = 0; c < chunksPerWorkload; ++c) {
            size_t off = (c * CHUNK_SIZE) % (data.size() - CHUNK_SIZE);
            stream.push_back({
                Chunk(data.begin() + off, data.begin() + off + CHUNK_SIZE),
                name, idx
            });
        }
    }
    return stream;
}

// Build a single-workload stream: all chunks from one workload.
static std::vector<TrialChunk> buildSingleStream(
    const std::string& workloadName_,
    const Chunk&       data,
    int                totalChunks)
{
    std::vector<TrialChunk> stream;
    uint8_t idx = workloadIndex(workloadName_);
    for (int c = 0; c < totalChunks; ++c) {
        size_t off = (c * CHUNK_SIZE) % (data.size() - CHUNK_SIZE);
        stream.push_back({
            Chunk(data.begin() + off, data.begin() + off + CHUNK_SIZE),
            workloadName_, idx
        });
    }
    return stream;
}

// ============================================================
//  Receiver thread
// ============================================================
struct ReceiverResult {
    std::vector<NetworkResult> perChunk;
};

static void receiverThread(
    int                          nTrials,
    int                          chunksPerTrial,
    std::atomic<bool>&           receiverReady,
    std::vector<ReceiverResult>& trialResults)
{
    int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(PORT);

    ::bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::listen(listenFd, 1);
    receiverReady.store(true);

    for (int trial = 0; trial < nTrials; ++trial) {
        int connFd = ::accept(listenFd, nullptr, nullptr);
        ReceiverResult result;

        for (int i = 0; i < chunksPerTrial; ++i) {
            FrameHeader hdr;
            Chunk payload;
            if (!recvFrame(connFd, hdr, payload)) break;

            uint64_t recvUs = nowMicros();
            double e2eMs = static_cast<double>(recvUs - hdr.sendTimestampUs)
                           / 1000.0;

            Chunk original = decompressFrame(hdr, payload);

            NetworkResult nr;
            nr.chunkId           = hdr.chunkId;
            nr.workload          = workloadName(hdr.workloadType);
            nr.algorithm         = toString(static_cast<Algorithm>(hdr.algorithm));
            nr.preprocess        = toString(static_cast<Preprocess>(hdr.preprocess));
            nr.originalSize      = hdr.originalSize;
            nr.compressedSize    = hdr.compressedSize;
            nr.compressionRatio  = static_cast<double>(hdr.compressedSize)
                                   / hdr.originalSize;
            nr.endToEndLatencyMs = e2eMs;

            result.perChunk.push_back(nr);
        }

        ::close(connFd);
        trialResults.push_back(result);
    }

    ::close(listenFd);
}

// ============================================================
//  Sender: one trial
// ============================================================
static void runSenderTrial(
    const std::vector<TrialChunk>& stream,
    std::function<Chunk(const Chunk&, FrameHeader&)> compressFn)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(PORT);

    ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    uint32_t chunkId = 0;
    for (const auto& tc : stream) {
        FrameHeader hdr;
        hdr.magic        = 0xADC0DE42;
        hdr.chunkId      = chunkId++;
        hdr.workloadType = tc.workloadIdx;

        Chunk compressed = compressFn(tc.data, hdr);
        hdr.sendTimestampUs = nowMicros();
        sendFrame(fd, hdr, compressed);
    }

    ::close(fd);
}

// ============================================================
//  Run N_TRIALS for one system + stream, return per-trial results
// ============================================================
static std::vector<ReceiverResult> runSystem(
    const std::string&             systemName,
    const std::vector<TrialChunk>& stream,
    std::function<Chunk(const Chunk&, FrameHeader&)> compressFn)
{
    std::cout << "  Running " << systemName
              << " (" << N_TRIALS << " trials)...\n";

    std::vector<ReceiverResult> trialResults;
    trialResults.reserve(N_TRIALS);

    std::atomic<bool> receiverReady{false};

    std::thread recv(receiverThread,
        N_TRIALS,
        static_cast<int>(stream.size()),
        std::ref(receiverReady),
        std::ref(trialResults));

    while (!receiverReady.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    for (int t = 0; t < N_TRIALS; ++t) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        runSenderTrial(stream, compressFn);
    }

    recv.join();
    return trialResults;
}

// ============================================================
//  Aggregate per-trial results into TrialStats per workload
// ============================================================
static std::vector<TrialStats> aggregateStats(
    const std::string&                 systemName,
    const std::vector<ReceiverResult>& trials)
{
    std::map<std::string, std::vector<double>> latByWorkload;
    std::map<std::string, std::vector<double>> ratioByWorkload;

    for (const auto& trial : trials)
        for (const auto& r : trial.perChunk) {
            latByWorkload  [r.workload].push_back(r.endToEndLatencyMs);
            ratioByWorkload[r.workload].push_back(r.compressionRatio);
        }

    std::vector<TrialStats> stats;
    for (auto& [workload, lats] : latByWorkload) {
        stats.push_back(computeStats(
            systemName + " | " + workload + " | latency_ms", lats));
        stats.push_back(computeStats(
            systemName + " | " + workload + " | ratio",
            ratioByWorkload[workload]));
    }
    return stats;
}

// ============================================================
//  Save raw per-chunk results to CSV
// ============================================================
static void saveRawCSV(
    const std::string&                 systemName,
    const std::vector<ReceiverResult>& trials,
    const std::string&                 scenario,
    std::ofstream&                     csv)
{
    for (int t = 0; t < static_cast<int>(trials.size()); ++t)
        for (const auto& r : trials[t].perChunk)
            csv << systemName          << ","
                << scenario            << ","
                << t                   << ","
                << r.chunkId           << ","
                << r.workload          << ","
                << r.algorithm         << ","
                << r.preprocess        << ","
                << r.originalSize      << ","
                << r.compressedSize    << ","
                << r.compressionRatio  << ","
                << r.endToEndLatencyMs << "\n";
}

// ============================================================
//  Adaptive compress function
// ============================================================
static EngineConfig g_cfg;

static Chunk adaptiveCompress(const Chunk& chunk, FrameHeader& hdr) {
    Features f = extractFeatures(chunk);
    Decision d = decide(f, g_cfg);
    return compressStatic(chunk, d.algorithm, d.preprocess, hdr);
}

// ============================================================
//  Core benchmark runner — shared by mixed and single-workload modes
// ============================================================
using CompressFn = std::function<Chunk(const Chunk&, FrameHeader&)>;

static void runBenchmark(
    const std::string&                                  scenario,
    const std::string&                                  modeTag,
    const std::vector<TrialChunk>&                      stream,
    const std::vector<std::pair<std::string, CompressFn>>& systems,
    std::ofstream&                                      rawCsv,
    std::vector<TrialStats>&                            allStats)
{
    for (const auto& [name, fn] : systems) {
        std::string fullName = name + "_" + modeTag;
        auto trials = runSystem(fullName, stream, fn);
        saveRawCSV(fullName, trials, scenario, rawCsv);
        auto stats = aggregateStats(fullName, trials);
        for (auto& s : stats) allStats.push_back(s);
    }
}

// ============================================================
//  Main
// ============================================================
int main(int argc, char* argv[]) {
    std::string scenario = (argc > 1) ? argv[1] : "baseline";

    std::cout << "========================================\n";
    std::cout << "  Network Benchmark — Scenario: " << scenario << "\n";
    std::cout << "========================================\n";
    std::cout << "Trials per system : " << N_TRIALS         << "\n";
    std::cout << "Chunks per trial  : " << CHUNKS_PER_TRIAL << "\n";
    std::cout << "Chunk size        : " << CHUNK_SIZE        << " bytes\n";
    std::cout << "Port              : " << PORT              << "\n\n";

    std::vector<std::pair<std::string, Chunk>> datasets = {
        { "Telemetry", generateTelemetry(DATA_SIZE) },
        { "JSON",      generateJSON(DATA_SIZE)       },
        { "Binary",    generateBinary(DATA_SIZE)     },
        { "Nibble",    generateNibble(DATA_SIZE)     }
    };

    int chunksPerWorkload = CHUNKS_PER_TRIAL / static_cast<int>(datasets.size());

    std::vector<std::pair<std::string, CompressFn>> systems = {
        { "LZ4",  [](const Chunk& c, FrameHeader& h) {
            return compressStatic(c, Algorithm::LZ4, Preprocess::NONE, h); }},
        { "ZSTD", [](const Chunk& c, FrameHeader& h) {
            return compressStatic(c, Algorithm::ZSTD, Preprocess::NONE, h); }},
        { "Gzip", [](const Chunk& c, FrameHeader& h) {
            return compressStatic(c, Algorithm::GZIP, Preprocess::NONE, h); }},
        { "Adaptive", adaptiveCompress }
    };

    std::string rawPath   = "results/network_"       + scenario + ".csv";
    std::string statsPath = "results/network_stats_" + scenario + ".csv";

    std::ofstream rawCsv(rawPath);
    rawCsv << "system,scenario,trial,chunk_id,workload,algorithm,"
              "preprocess,original_bytes,compressed_bytes,"
              "compression_ratio,e2e_latency_ms\n";

    std::vector<TrialStats> allStats;

    // --- Mixed-workload trials (existing behaviour) ---
    std::cout << "\n[Mode: mixed]\n";
    auto mixedStream = buildMixedStream(datasets, chunksPerWorkload);
    runBenchmark(scenario, "mixed", mixedStream, systems, rawCsv, allStats);

    // --- Single-workload trials for Binary (isolates binary-specific latency) ---
    std::cout << "\n[Mode: single / Binary]\n";
    const Chunk& binaryData = datasets[2].second;   // index 2 = Binary
    auto singleStream = buildSingleStream("Binary", binaryData, CHUNKS_PER_TRIAL);
    runBenchmark(scenario, "single_binary", singleStream, systems, rawCsv, allStats);

    std::cout << "\n--- Results: " << scenario << " ---\n";
    printStatsHeader();
    for (const auto& s : allStats)
        printStats(s);

    saveStatsCSV(allStats, statsPath);

    std::cout << "\nRaw results  : " << rawPath   << "\n";
    std::cout << "Stats summary: " << statsPath  << "\n";

    return 0;
}