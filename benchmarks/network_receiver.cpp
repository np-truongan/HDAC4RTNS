// network_receiver.cpp — run this on the Raspberry Pi or any Linux/macOS machine
// Usage: ./network_receiver [port] [output_dir]
// Example: ./network_receiver 54321 ./results
//
// Platform support:
//   Linux / WSL2  : fully supported
//   macOS         : fully supported
//   Windows native: NOT supported — use WSL2 instead
//
// Protocol: after each frame is received and decompressed, sends a 1-byte ACK
// back to the sender. This enables RTT/2 latency measurement on the sender side
// without requiring clock synchronisation between machines.
//
// The receiver writes its own CSV recording compression ratios and chunk counts,
// which can be cross-checked against the sender's CSV.

#if defined(_WIN32) && !defined(__CYGWIN__)
    #error "Run this under WSL2 on Windows. Native Winsock is not supported."
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #define CLOSE_SOCKET(fd) ::close(fd)
#endif

#include "framing.h"
#include "strategies.h"
#include "types.h"

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <ctime>

static constexpr int DEFAULT_PORT = 54321;

// ============================================================
//  Helpers
// ============================================================
static std::string algoName(uint8_t algo) {
    switch (static_cast<Algorithm>(algo)) {
        case Algorithm::LZ4:  return "LZ4";
        case Algorithm::ZSTD: return "ZSTD";
        case Algorithm::GZIP: return "Gzip";
        default: return "Unknown";
    }
}

static std::string workloadName(uint8_t idx) {
    switch (idx) {
        case 0: return "Telemetry";
        case 1: return "JSON";
        case 2: return "Binary";
        case 3: return "Nibble";
        default: return "Unknown";
    }
}

static Chunk decompressFrame(const FrameHeader& hdr, const Chunk& payload) {
    Algorithm  algo = static_cast<Algorithm>(hdr.algorithm);
    Preprocess prep = static_cast<Preprocess>(hdr.preprocess);

    Chunk out;
    switch (algo) {
        case Algorithm::LZ4:  out = decompressLZ4(payload, hdr.originalSize); break;
        case Algorithm::ZSTD: out = decompressZSTD(payload);                   break;
        case Algorithm::GZIP: out = decompressGZIP(payload, hdr.originalSize); break;
    }
    if (prep == Preprocess::DELTA)
        out = deltaDecode(out);
    if (prep == Preprocess::BITPACK)
        out = bitPackDecode(out, hdr.originalSize);
    return out;
}

// ============================================================
//  Per-chunk record (receiver side)
// ============================================================
struct ChunkRecord {
    int         trial;
    uint32_t    chunkId;
    std::string workload;
    std::string algo;
    double      ratio;
    uint32_t    originalBytes;
    uint32_t    compressedBytes;
};

// ============================================================
//  Derive system name from algorithm seen in first chunk
//  (receiver doesn't know which system the sender is using,
//   so we infer from algo field and group by connection)
// ============================================================

// ============================================================
//  Main
// ============================================================
int main(int argc, char* argv[]) {
    int         port   = (argc > 1) ? std::stoi(argv[1]) : DEFAULT_PORT;
    std::string outDir = (argc > 2) ? argv[2] : "./results";

    std::filesystem::create_directories(outDir);

    int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) {
        std::cerr << "Failed to create socket\n";
        return 1;
    }

    int opt = 1;
    ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    // macOS: uncomment if you see "address already in use":
    // ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(static_cast<uint16_t>(port));

    if (::bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "Bind failed on port " << port << "\n";
        CLOSE_SOCKET(listenFd);
        return 1;
    }
    ::listen(listenFd, 8);

    std::cout << "Receiver listening on port " << port << "\n";
    std::cout << "Output directory: " << outDir << "\n";
    std::cout << "Ctrl+C to stop after all trials complete.\n\n";

    // Each system runs N_TRIALS connections. We accumulate records across
    // all connections and detect system changes by watching algo patterns.
    // Simpler: use a connection counter as trial index, group by algo.

    // Map: algo_name -> records for that system
    std::map<std::string, std::vector<ChunkRecord>> systemRecords;
    std::map<std::string, int> trialCounters; // per system

    int connectionCount = 0;

    while (true) {
        int connFd = ::accept(listenFd, nullptr, nullptr);
        if (connFd < 0) break;
        ++connectionCount;

        std::string currentSystem = "";
        int         chunkCount    = 0;
        std::vector<ChunkRecord> trialChunks;

        while (true) {
            FrameHeader hdr;
            Chunk payload;
            if (!recvFrame(connFd, hdr, payload)) break;

            // Send 1-byte ACK immediately after receiving frame
            // This is what enables RTT/2 measurement on the sender
            uint8_t ack = 0x01;
            ::send(connFd, &ack, 1, 0);

            // Decompress to verify correctness (result discarded)
            Chunk original = decompressFrame(hdr, payload);
            (void)original;

            std::string aName = algoName(hdr.algorithm);
            double ratio = static_cast<double>(hdr.compressedSize) /
                           static_cast<double>(hdr.originalSize);

            trialChunks.push_back({
                0, // trial filled in below
                hdr.chunkId,
                workloadName(hdr.workloadType),
                aName,
                ratio,
                hdr.originalSize,
                hdr.compressedSize
            });

            // Track which system this connection belongs to
            // (first chunk's algo determines the system label)
            if (currentSystem.empty()) {
                currentSystem = aName;
            }

            ++chunkCount;
        }

        // Assign trial number and store
        if (!trialChunks.empty()) {
            int& trialNum = trialCounters[currentSystem];
            ++trialNum;
            for (auto& r : trialChunks)
                r.trial = trialNum;
            auto& records = systemRecords[currentSystem];
            records.insert(records.end(), trialChunks.begin(), trialChunks.end());

            std::cout << "  [conn " << connectionCount
                      << " | system=" << currentSystem
                      << " | trial=" << trialNum
                      << " | chunks=" << chunkCount << "]\n";
        }

        CLOSE_SOCKET(connFd);
    }

    // Write one CSV per system
    std::cout << "\nWriting CSVs...\n";
    for (const auto& [systemName, records] : systemRecords) {
        std::string filename = outDir + "/receiver_" + systemName + ".csv";
        std::ofstream f(filename);
        if (!f.is_open()) {
            std::cerr << "Cannot write: " << filename << "\n";
            continue;
        }
        f << "trial,chunk_id,workload,system,algo,ratio,original_bytes,compressed_bytes\n";
        for (const auto& r : records) {
            f << r.trial          << ","
              << r.chunkId        << ","
              << r.workload       << ","
              << systemName       << ","
              << r.algo           << ","
              << r.ratio          << ","
              << r.originalBytes  << ","
              << r.compressedBytes << "\n";
        }
        std::cout << "  -> wrote " << filename
                  << " (" << records.size() << " rows)\n";
    }

    CLOSE_SOCKET(listenFd);
    std::cout << "Done.\n";
    return 0;
}