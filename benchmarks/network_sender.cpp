// network_sender.cpp — run this on your laptop
// Usage: ./network_sender <receiver_ip> [scenario] [output_dir]
// Example: ./network_sender 192.168.1.42 baseline ./results
//
// Platform support:
//   Linux / WSL2  : fully supported
//   macOS         : fully supported
//   Windows native: NOT supported — use WSL2 instead
//
// Latency measurement: RTT/2 approximation.
//   Sender records T_send just before sendFrame() (after compression),
//   waits for 1-byte ACK from receiver, records T_recv after ACK arrives,
//   then e2e_ms = (T_recv - T_send) / 2000.0.
//   This avoids clock-sync issues between two machines entirely.
//   Document in thesis as "RTT/2 one-way latency approximation."

#if defined(_WIN32) && !defined(__CYGWIN__)
    #error "Run this under WSL2 on Windows. Native Winsock is not supported."
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #define CLOSE_SOCKET(fd) ::close(fd)
#endif

#include "generators.h"
#include "heuristics.h"
#include "strategies.h"
#include "engine.h"
#include "framing.h"
#include "stats.h"
#include "types.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>
#include <string>
#include <functional>
#include <filesystem>

static constexpr int    PORT             = 54321;
static constexpr size_t CHUNK_SIZE       = 4096;
static constexpr size_t DATA_SIZE        = 1 << 20;
static constexpr int    N_TRIALS         = 30;
static constexpr int    CHUNKS_PER_TRIAL = 40;

static std::string g_receiverIP;

// ============================================================
//  Workload helpers — delegate to types.h so all five workload
//  types (including Boundary, index 4) are handled consistently.
// ============================================================
static uint8_t workloadIdx(const std::string& name) {
    return workloadIndex(name);   // types.h — handles Boundary
}

static std::string algoName(uint8_t algo) {
    switch (static_cast<Algorithm>(algo)) {
        case Algorithm::LZ4:  return "LZ4";
        case Algorithm::ZSTD: return "ZSTD";
        case Algorithm::GZIP: return "Gzip";
        default: return "Unknown";
    }
}

// ============================================================
//  Compression helpers
// ============================================================
static Chunk compressStatic(const Chunk& chunk, Algorithm algo,
                             Preprocess prep, FrameHeader& hdr)
{
    Chunk processed = chunk;
    if (prep == Preprocess::DELTA) {
        processed = deltaEncode(chunk);
    } else if (prep == Preprocess::BITPACK) {
        bool eligible = true;
        for (Byte b : processed) if (b > 15) { eligible = false; break; }
        if (eligible) processed = bitPackEncode(processed);
        else          prep = Preprocess::NONE;
    }
    Chunk compressed;
    switch (algo) {
        case Algorithm::LZ4:  compressed = compressLZ4(processed);  break;
        case Algorithm::ZSTD: compressed = compressZSTD(processed); break;
        case Algorithm::GZIP: compressed = compressGZIP(processed); break;
    }
    hdr.algorithm        = static_cast<uint8_t>(algo);
    hdr.preprocess       = static_cast<uint8_t>(prep);
    hdr.originalSize     = static_cast<uint32_t>(chunk.size());
    hdr.preprocessedSize = static_cast<uint32_t>(processed.size());
    hdr.compressedSize   = static_cast<uint32_t>(compressed.size());
    return compressed;
}

static EngineConfig g_cfg;
static Chunk adaptiveCompress(const Chunk& chunk, FrameHeader& hdr) {
    Features f = extractFeatures(chunk);
    Decision d = decide(f, g_cfg);
    return compressStatic(chunk, d.algorithm, d.preprocess, hdr);
}

// ============================================================
//  Per-chunk result (sender side)
// ============================================================
struct ChunkRecord {
    int         trial;
    uint32_t    chunkId;
    std::string workload;
    std::string algo;
    double      ratio;
    double      rttMs;      // full RTT
    double      e2eMs;      // RTT / 2
};

// ============================================================
//  Stream builder
// ============================================================
struct TrialChunk { Chunk data; std::string workload; uint8_t workloadIdx; };

static std::vector<TrialChunk> buildMixedStream(
    const std::vector<std::pair<std::string, Chunk>>& datasets,
    int chunksPerWorkload)
{
    std::vector<TrialChunk> stream;
    for (size_t d = 0; d < datasets.size(); ++d) {
        const auto& [name, data] = datasets[d];
        uint8_t idx = workloadIdx(name);
        for (int c = 0; c < chunksPerWorkload; ++c) {
            size_t off = (c * CHUNK_SIZE) % (data.size() - CHUNK_SIZE);
            stream.push_back({
                Chunk(data.begin()+off, data.begin()+off+CHUNK_SIZE),
                name, idx
            });
        }
    }
    return stream;
}

// ============================================================
//  Run one trial — returns per-chunk records
// ============================================================
static bool runTrial(
    int trial,
    const std::string& systemName,
    const std::vector<TrialChunk>& stream,
    std::function<Chunk(const Chunk&, FrameHeader&)> compressFn,
    std::vector<ChunkRecord>& out)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(PORT);
    if (::inet_pton(AF_INET, g_receiverIP.c_str(), &addr.sin_addr) <= 0) {
        CLOSE_SOCKET(fd);
        return false;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "  Connection failed (trial " << trial << ")\n";
        CLOSE_SOCKET(fd);
        return false;
    }

    // Send system-name handshake: 1-byte length + name bytes.
    // The receiver uses this to group connections by system rather than
    // inferring from the first chunk's algorithm (which breaks for Adaptive).
    {
        uint8_t nameLen = static_cast<uint8_t>(systemName.size());
        ::send(fd, &nameLen, 1, 0);
        ::send(fd, systemName.data(), nameLen, 0);
    }

    uint32_t chunkId = 0;
    for (const auto& tc : stream) {
        FrameHeader hdr;
        hdr.magic        = 0xADC0DE42;
        hdr.chunkId      = chunkId;
        hdr.workloadType = tc.workloadIdx;

        // Compress before stamping so compression time is excluded from
        // the RTT measurement (we're measuring network transfer only).
        Chunk compressed = compressFn(tc.data, hdr);
        double ratio     = static_cast<double>(hdr.compressedSize) /
                           static_cast<double>(hdr.originalSize);

        // Stamp just before the wire send.
        hdr.sendTimestampUs = nowMicros();
        sendFrame(fd, hdr, compressed);

        // Wait for 1-byte ACK from receiver (backpressure + RTT close).
        uint8_t ack = 0;
        if (::recv(fd, &ack, 1, MSG_WAITALL) != 1) {
            std::cerr << "  ACK missing on chunk " << chunkId << "\n";
            break;
        }
        uint64_t recvUs = nowMicros();

        double rttMs = static_cast<double>(recvUs - hdr.sendTimestampUs) / 1000.0;
        double e2eMs = rttMs / 2.0;

        out.push_back({
            trial,
            chunkId,
            tc.workload,
            algoName(hdr.algorithm),
            ratio,
            rttMs,
            e2eMs
        });

        ++chunkId;
    }

    CLOSE_SOCKET(fd);
    return true;
}

// ============================================================
//  Write CSV for one system
// ============================================================
static void writeCSV(const std::vector<ChunkRecord>& records,
                     const std::string& systemName,
                     const std::string& scenario,
                     const std::string& outDir)
{
    std::string filename = outDir + "/sender_" + systemName + "_" + scenario + ".csv";
    std::ofstream f(filename);
    if (!f.is_open()) {
        std::cerr << "Cannot write: " << filename << "\n";
        return;
    }
    f << "trial,chunk_id,workload,system,algo,ratio,rtt_ms,e2e_ms\n";
    for (const auto& r : records) {
        f << r.trial     << ","
          << r.chunkId   << ","
          << r.workload  << ","
          << systemName  << ","
          << r.algo      << ","
          << r.ratio     << ","
          << r.rttMs     << ","
          << r.e2eMs     << "\n";
    }
    std::cout << "  -> wrote " << filename
              << " (" << records.size() << " rows)\n";
}

// ============================================================
//  Main
// ============================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: ./network_sender <receiver_ip> [scenario] [output_dir]\n";
        std::cerr << "Example: ./network_sender 192.168.1.42 baseline ./results\n";
        return 1;
    }

    g_receiverIP         = argv[1];
    std::string scenario = (argc > 2) ? argv[2] : "baseline";
    std::string outDir   = (argc > 3) ? argv[3] : "./results";

    std::filesystem::create_directories(outDir);

    std::cout << "==============================\n";
    std::cout << "  Network Sender\n";
    std::cout << "  Receiver : " << g_receiverIP << ":" << PORT << "\n";
    std::cout << "  Scenario : " << scenario      << "\n";
    std::cout << "  Trials   : " << N_TRIALS      << "\n";
    std::cout << "  Output   : " << outDir        << "\n";
    std::cout << "  Latency  : RTT/2 (ACK-based, no clock sync needed)\n";
    std::cout << "==============================\n\n";

    // macOS traffic shaping note:
    // For moderate/constrained scenarios on macOS, use dnctl + pfctl instead
    // of tc netem. See comments in README for exact commands.

    // All five workload types — Boundary (index 4) added alongside the
    // existing four. workloadIndex() in types.h handles the mapping.
    std::vector<std::pair<std::string, Chunk>> datasets = {
        { "Telemetry", generateTelemetry(DATA_SIZE) },
        { "JSON",      generateJSON(DATA_SIZE)       },
        { "Binary",    generateBinary(DATA_SIZE)     },
        { "Nibble",    generateNibble(DATA_SIZE)     },
        { "Boundary",  generateBoundary(DATA_SIZE)   },
    };

    int chunksPerWorkload = CHUNKS_PER_TRIAL / static_cast<int>(datasets.size());
    auto stream = buildMixedStream(datasets, chunksPerWorkload);

    using CompressFn = std::function<Chunk(const Chunk&, FrameHeader&)>;
    std::vector<std::pair<std::string, CompressFn>> systems = {
        { "LZ4",  [](const Chunk& c, FrameHeader& h) {
            return compressStatic(c, Algorithm::LZ4,  Preprocess::NONE, h); }},
        { "ZSTD", [](const Chunk& c, FrameHeader& h) {
            return compressStatic(c, Algorithm::ZSTD, Preprocess::NONE, h); }},
        { "Gzip", [](const Chunk& c, FrameHeader& h) {
            return compressStatic(c, Algorithm::GZIP, Preprocess::NONE, h); }},
        { "Adaptive", adaptiveCompress }
    };

    for (const auto& [name, fn] : systems) {
        std::cout << "Running " << name << " (" << N_TRIALS << " trials)...\n";
        std::vector<ChunkRecord> records;
        records.reserve(N_TRIALS * CHUNKS_PER_TRIAL);

        int failed = 0;
        for (int t = 0; t < N_TRIALS; ++t) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!runTrial(t + 1, name, stream, fn, records)) ++failed;
        }

        if (failed > 0)
            std::cerr << "  WARNING: " << failed << " trial(s) failed.\n";

        writeCSV(records, name, scenario, outDir);
        std::cout << "  done.\n\n";
    }

    std::cout << "All done. Check " << outDir << "/ for CSV files.\n";
    return 0;
}