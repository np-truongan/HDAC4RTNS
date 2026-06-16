// network_sender.cpp — run this on your laptop (Linux, WSL2, or macOS)
// Usage: ./network_sender <receiver_ip> [scenario]
// Example: ./network_sender 192.168.1.42 moderate
//
// Platform support:
//   Linux / WSL2  : fully supported
//   macOS         : fully supported
//   Windows native: NOT supported — use WSL2 instead

// ============================================================
//  Platform headers
// ============================================================
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
#include <thread>
#include <vector>
#include <string>
#include <functional>
#include <map>

static constexpr int    PORT             = 54321;
static constexpr size_t CHUNK_SIZE       = 4096;
static constexpr size_t DATA_SIZE        = 1 << 20;
static constexpr int    N_TRIALS         = 30;
static constexpr int    CHUNKS_PER_TRIAL = 40;

static std::string g_receiverIP;

// ============================================================
//  Compress helpers — identical to original network_benchmark.cpp
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
    hdr.algorithm      = static_cast<uint8_t>(algo);
    hdr.preprocess     = static_cast<uint8_t>(prep);
    hdr.originalSize   = static_cast<uint32_t>(chunk.size());
    hdr.compressedSize = static_cast<uint32_t>(compressed.size());
    return compressed;
}

static EngineConfig g_cfg;
static Chunk adaptiveCompress(const Chunk& chunk, FrameHeader& hdr) {
    Features f = extractFeatures(chunk);
    Decision d = decide(f, g_cfg);
    return compressStatic(chunk, d.algorithm, d.preprocess, hdr);
}

// ============================================================
//  Stream builders — identical to original
// ============================================================
struct TrialChunk { Chunk data; std::string workload; uint8_t workloadIdx; };

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
                Chunk(data.begin()+off, data.begin()+off+CHUNK_SIZE),
                name, idx
            });
        }
    }
    return stream;
}

// ============================================================
//  Sender trial — connects to remote receiver
// ============================================================
static bool runSenderTrial(
    const std::vector<TrialChunk>& stream,
    std::function<Chunk(const Chunk&, FrameHeader&)> compressFn)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(PORT);

    if (::inet_pton(AF_INET, g_receiverIP.c_str(), &addr.sin_addr) <= 0) {
        std::cerr << "Invalid IP address: " << g_receiverIP << "\n";
        CLOSE_SOCKET(fd);
        return false;
    }

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "Connection failed to " << g_receiverIP
                  << ":" << PORT << "\n";
        CLOSE_SOCKET(fd);
        return false;
    }

    uint32_t chunkId = 0;
    for (const auto& tc : stream) {
        FrameHeader hdr;
        hdr.magic        = 0xADC0DE42;
        hdr.chunkId      = chunkId++;
        hdr.workloadType = tc.workloadIdx;

        Chunk compressed     = compressFn(tc.data, hdr);
        hdr.sendTimestampUs  = nowMicros();
        // NOTE: sendTimestampUs uses the SENDER's clock.
        // The receiver computes e2e latency using its own clock.
        // For accurate results, sync both clocks before running:
        //   Linux/WSL : sudo ntpdate pool.ntp.org
        //   macOS     : sudo sntp -sS pool.ntp.org
        //   Pi        : sudo ntpdate pool.ntp.org
        sendFrame(fd, hdr, compressed);
    }

    CLOSE_SOCKET(fd);
    return true;
}

// ============================================================
//  Main
// ============================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: ./network_sender <receiver_ip> [scenario]\n";
        std::cerr << "Example: ./network_sender 192.168.1.42 moderate\n";
        return 1;
    }

    g_receiverIP         = argv[1];
    std::string scenario = (argc > 2) ? argv[2] : "baseline";

    std::cout << "==============================\n";
    std::cout << "  Network Sender\n";
    std::cout << "  Receiver : " << g_receiverIP << ":" << PORT << "\n";
    std::cout << "  Scenario : " << scenario      << "\n";
    std::cout << "  Trials   : " << N_TRIALS      << "\n";
    std::cout << "  Chunks   : " << CHUNKS_PER_TRIAL << " per trial\n";
    std::cout << "==============================\n\n";

    // macOS note: if you want to apply netem-style delay on macOS,
    // use 'man dnctl' and 'man pfctl' (macOS traffic shaping) instead of tc netem.
    // Example (macOS, optional):
    //   sudo dnctl pipe 1 config delay 25 bw 100Mbit/s
    //   echo "dummynet out proto tcp from any to <receiver_ip> pipe 1" | sudo pfctl -f -
    //   sudo pfctl -e
    // Clean up after:
    //   sudo pfctl -d && sudo dnctl -q flush

    std::vector<std::pair<std::string, Chunk>> datasets = {
        { "Telemetry", generateTelemetry(DATA_SIZE) },
        { "JSON",      generateJSON(DATA_SIZE)       },
        { "Binary",    generateBinary(DATA_SIZE)     },
        { "Nibble",    generateNibble(DATA_SIZE)     }
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
        std::cout << "Running " << name
                  << " (" << N_TRIALS << " trials)...\n";
        int failed = 0;
        for (int t = 0; t < N_TRIALS; ++t) {
            // Small gap between trials so receiver has time to re-enter accept()
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!runSenderTrial(stream, fn)) ++failed;
        }
        if (failed > 0)
            std::cerr << "  WARNING: " << failed << " trial(s) failed to connect.\n";
        std::cout << "  done.\n";
    }

    std::cout << "\nAll done. Check the receiver terminal for latency output.\n";
    return 0;
}