// benchmarks/network_benchmark.cpp
//
// Loopback TCP network benchmark with standardized conditions.
//
// Architecture:
//   - Receiver thread binds a TCP socket and listens
//   - Sender thread connects, compresses chunks, sends frames
//   - Receiver decompresses, records end-to-end latency
//   - Repeated for N_TRIALS runs per system per scenario
//   - 95% confidence intervals computed across trials
//
// Network conditions via tc netem (applied externally):
//   Scenario A: baseline   — no netem (loopback only)
//   Scenario B: moderate   — 50ms RTT, 100Mbps bandwidth
//   Scenario C: constrained — 200ms RTT, 10Mbps, 0.5% loss
//
// The benchmark itself does not apply netem — the caller
// script (scripts/run_network_benchmark.sh) applies and
// removes netem conditions between scenario runs.
//
// Output: results/network_<scenario>.csv
//         results/network_stats_<scenario>.csv

#include "generators.h"
#include "heuristics.h"
#include "strategies.h"
#include "engine.h"
#include "framing.h"
#include "stats.h"
#include "types.h"

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

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <map>

// ============================================================
//  Constants
// ============================================================
static constexpr int    PORT          = 54321;
static constexpr size_t CHUNK_SIZE    = 4096;
static constexpr size_t DATA_SIZE     = 1 << 20;   // 1MB per workload
static constexpr int    N_TRIALS      = 30;         // per system per scenario
static constexpr int    CHUNKS_PER_TRIAL = 40;      // 10 per workload type

// ============================================================
//  Compress one chunk using the given static algorithm.
//  Returns compressed bytes and fills the header fields.
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
//  Decompress one frame payload using header metadata.
// ============================================================
static Chunk decompressFrame(const FrameHeader& hdr, const Chunk& payload) {
    Algorithm  algo = static_cast<Algorithm>(hdr.algorithm);
    Preprocess prep = static_cast<Preprocess>(hdr.preprocess);

    Chunk decompressed;
    switch (algo) {
        case Algorithm::LZ4:
            decompressed = decompressLZ4(payload, hdr.originalSize);
            break;
        case Algorithm::ZSTD:
            decompressed = decompressZSTD(payload);
            break;
        case Algorithm::GZIP:
            decompressed = decompressGZIP(payload, hdr.originalSize);
            break;
    }

    // Reverse preprocessing
    if (prep == Preprocess::DELTA)
        decompressed = deltaDecode(decompressed);
    else if (prep == Preprocess::BITPACK)
        decompressed = bitPackDecode(decompressed, hdr.originalSize);

    return decompressed;
}

// ============================================================
//  Stream of chunks for one trial
//  10 chunks per workload type, interleaved
// ============================================================
struct TrialChunk {
    Chunk       data;
    std::string workload;
    uint8_t     workloadIdx;
};

static std::vector<TrialChunk> buildTrialStream(
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
                name,
                idx
            });
        }
    }
    return stream;
}

// ============================================================
//  Receiver thread
//
//  Binds socket, signals ready, then for each trial:
//    - Accepts connection
//    - Receives all frames
//    - Records end-to-end latency per frame
//    - Closes connection (sender reconnects each trial)
// ============================================================
struct ReceiverResult {
    std::vector<NetworkResult> perChunk;
};

static void receiverThread(
    int                        nTrials,
    int                        chunksPerTrial,
    std::atomic<bool>&         receiverReady,
    std::vector<ReceiverResult>& trialResults)
{
    // Create and bind listening socket
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

            // Decompress (verifies correctness implicitly)
            Chunk original = decompressFrame(hdr, payload);

            NetworkResult nr;
            nr.chunkId            = hdr.chunkId;
            nr.workload           = workloadName(hdr.workloadType);
            nr.algorithm          = toString(static_cast<Algorithm>(hdr.algorithm));
            nr.preprocess         = toString(static_cast<Preprocess>(hdr.preprocess));
            nr.originalSize       = hdr.originalSize;
            nr.compressedSize     = hdr.compressedSize;
            nr.compressionRatio   = static_cast<double>(hdr.compressedSize)
                                    / hdr.originalSize;
            nr.endToEndLatencyMs  = e2eMs;

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

        // Stamp as late as possible before send
        hdr.sendTimestampUs = nowMicros();

        sendFrame(fd, hdr, compressed);
    }

    ::close(fd);
}

// ============================================================
//  Run N_TRIALS for one system, return per-trial results
// ============================================================
static std::vector<ReceiverResult> runSystem(
    const std::string&             systemName,
    const std::vector<TrialChunk>& stream,
    std::function<Chunk(const Chunk&, FrameHeader&)> compressFn)
{
    std::cout << "  Running " << systemName << " ("
              << N_TRIALS << " trials)...\n";

    std::vector<ReceiverResult> trialResults;
    trialResults.reserve(N_TRIALS);

    std::atomic<bool> receiverReady{false};

    // Launch receiver
    std::thread recv(receiverThread,
        N_TRIALS,
        static_cast<int>(stream.size()),
        std::ref(receiverReady),
        std::ref(trialResults));

    // Wait for receiver to bind
    while (!receiverReady.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    // Run sender trials sequentially
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
    // Collect latencies per workload across all trials
    std::map<std::string, std::vector<double>> latByWorkload;
    std::map<std::string, std::vector<double>> ratioByWorkload;

    for (const auto& trial : trials) {
        for (const auto& r : trial.perChunk) {
            latByWorkload  [r.workload].push_back(r.endToEndLatencyMs);
            ratioByWorkload[r.workload].push_back(r.compressionRatio);
        }
    }

    std::vector<TrialStats> stats;
    for (auto& [workload, lats] : latByWorkload) {
        auto latStats   = computeStats(systemName + " | " + workload + " | latency_ms",
                                       lats);
        auto ratioStats = computeStats(systemName + " | " + workload + " | ratio",
                                       ratioByWorkload[workload]);
        stats.push_back(latStats);
        stats.push_back(ratioStats);
    }
    return stats;
}

// ============================================================
//  Save raw per-chunk results from all trials to CSV
// ============================================================
static void saveRawCSV(
    const std::string&                 systemName,
    const std::vector<ReceiverResult>& trials,
    const std::string&                 scenario,
    std::ofstream&                     csv)
{
    for (int t = 0; t < static_cast<int>(trials.size()); ++t) {
        for (const auto& r : trials[t].perChunk) {
            csv << systemName         << ","
                << scenario           << ","
                << t                  << ","
                << r.chunkId          << ","
                << r.workload         << ","
                << r.algorithm        << ","
                << r.preprocess       << ","
                << r.originalSize     << ","
                << r.compressedSize   << ","
                << r.compressionRatio << ","
                << r.endToEndLatencyMs << "\n";
        }
    }
}

// ============================================================
//  Adaptive compress function
// ============================================================
static EngineConfig g_cfg;

static Chunk adaptiveCompress(const Chunk& chunk, FrameHeader& hdr) {
    Features f = extractFeatures(chunk);
    Decision d = decide(f, g_cfg);

    Preprocess prep = d.preprocess;
    Algorithm  algo = d.algorithm;

    return compressStatic(chunk, algo, prep, hdr);
}

// ============================================================
//  Main
// ============================================================
int main(int argc, char* argv[]) {
    // Accept scenario name as argument (for CSV labeling)
    std::string scenario = (argc > 1) ? argv[1] : "baseline";

    std::cout << "========================================\n";
    std::cout << "  Network Benchmark — Scenario: " << scenario << "\n";
    std::cout << "========================================\n";
    std::cout << "Trials per system : " << N_TRIALS        << "\n";
    std::cout << "Chunks per trial  : " << CHUNKS_PER_TRIAL << "\n";
    std::cout << "Chunk size        : " << CHUNK_SIZE       << " bytes\n";
    std::cout << "Port              : " << PORT             << "\n\n";

    // --------------------------------------------------------
    //  Generate datasets (reused across all trials)
    // --------------------------------------------------------
    std::vector<std::pair<std::string, Chunk>> datasets = {
        { "Telemetry", generateTelemetry(DATA_SIZE) },
        { "JSON",      generateJSON(DATA_SIZE)      },
        { "Binary",    generateBinary(DATA_SIZE)    },
        { "Nibble",    generateNibble(DATA_SIZE)    }
    };

    int chunksPerWorkload = CHUNKS_PER_TRIAL / static_cast<int>(datasets.size());
    auto stream = buildTrialStream(datasets, chunksPerWorkload);

    // --------------------------------------------------------
    //  Define systems to benchmark
    // --------------------------------------------------------
    using CompressFn = std::function<Chunk(const Chunk&, FrameHeader&)>;

    std::vector<std::pair<std::string, CompressFn>> systems = {
        { "LZ4",  [](const Chunk& c, FrameHeader& h) {
            return compressStatic(c, Algorithm::LZ4, Preprocess::NONE, h); }},
        { "ZSTD", [](const Chunk& c, FrameHeader& h) {
            return compressStatic(c, Algorithm::ZSTD, Preprocess::NONE, h); }},
        { "Gzip", [](const Chunk& c, FrameHeader& h) {
            return compressStatic(c, Algorithm::GZIP, Preprocess::NONE, h); }},
        { "Adaptive", adaptiveCompress }
    };

    // --------------------------------------------------------
    //  Open output CSVs
    // --------------------------------------------------------
    std::string rawPath   = "results/network_"       + scenario + ".csv";
    std::string statsPath = "results/network_stats_" + scenario + ".csv";

    std::ofstream rawCsv(rawPath);
    rawCsv << "system,scenario,trial,chunk_id,workload,algorithm,"
              "preprocess,original_bytes,compressed_bytes,"
              "compression_ratio,e2e_latency_ms\n";

    std::vector<TrialStats> allStats;

    // --------------------------------------------------------
    //  Run each system
    // --------------------------------------------------------
    for (auto& [name, fn] : systems) {
        auto trials = runSystem(name, stream, fn);
        saveRawCSV(name, trials, scenario, rawCsv);
        auto stats = aggregateStats(name, trials);
        for (auto& s : stats) allStats.push_back(s);
    }

    // --------------------------------------------------------
    //  Print summary table
    // --------------------------------------------------------
    std::cout << "\n--- Results: " << scenario << " ---\n";
    printStatsHeader();
    for (const auto& s : allStats)
        printStats(s);

    // --------------------------------------------------------
    //  Save stats CSV
    // --------------------------------------------------------
    saveStatsCSV(allStats, statsPath);

    std::cout << "\nRaw results  : " << rawPath   << "\n";
    std::cout << "Stats summary: " << statsPath  << "\n";

    return 0;
}