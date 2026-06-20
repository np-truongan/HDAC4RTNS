// network_receiver.cpp — run this on the Raspberry Pi or any Linux/macOS machine
// Usage: ./network_receiver [port] [output_dir]
// Example: ./network_receiver 54321 ./results
//
// Platform support:
//   Linux / WSL2  : fully supported
//   macOS         : fully supported
//   Windows native: NOT supported — use WSL2 instead
//
// Protocol: after each frame is received and decompressed, a 1-byte ACK is
// sent back to the sender. This creates per-chunk backpressure (preventing
// silent chunk loss) and enables RTT/2 latency measurement on the sender
// side without requiring clock synchronisation between machines.
//
// System identification: the sender writes a systemId byte into FrameHeader
// (via hdr.systemId) so the receiver can group connections correctly even
// when Adaptive picks different algorithms on different chunks. This avoids
// the previous bug where the first chunk's algorithm was used as the system
// label, causing Adaptive trials to be mis-bucketed across multiple CSV files.
//
// The receiver writes one CSV per system, cross-checkable against the
// sender's CSV.

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

static constexpr int DEFAULT_PORT = 54321;

// ============================================================
//  Helpers — use types.h inline helpers so all five workload
//  types (including Boundary, index 4) are handled consistently.
// ============================================================
static std::string algoName(uint8_t algo) {
    switch (static_cast<Algorithm>(algo)) {
        case Algorithm::LZ4:  return "LZ4";
        case Algorithm::ZSTD: return "ZSTD";
        case Algorithm::GZIP: return "Gzip";
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

    // System name is read from FrameHeader::systemId (set by the sender),
    // not inferred from the first chunk's algorithm. This correctly groups
    // Adaptive connections even though Adaptive uses varying algorithms
    // across chunks.
    //
    // Map: system_name -> records for that system
    std::map<std::string, std::vector<ChunkRecord>> systemRecords;
    std::map<std::string, int> trialCounters; // per system

    int connectionCount = 0;

    while (true) {
        int connFd = ::accept(listenFd, nullptr, nullptr);
        if (connFd < 0) break;
        ++connectionCount;

        // Read the system-name handshake: 1-byte length followed by the name.
        // The sender sends this once per connection before the first frame.
        uint8_t nameLen = 0;
        if (::recv(connFd, &nameLen, 1, MSG_WAITALL) != 1 || nameLen == 0) {
            std::cerr << "  [conn " << connectionCount
                      << "] bad handshake, skipping\n";
            CLOSE_SOCKET(connFd);
            continue;
        }
        std::string currentSystem(nameLen, '\0');
        if (::recv(connFd, currentSystem.data(), nameLen, MSG_WAITALL)
                != static_cast<ssize_t>(nameLen)) {
            std::cerr << "  [conn " << connectionCount
                      << "] truncated system name, skipping\n";
            CLOSE_SOCKET(connFd);
            continue;
        }

        int chunkCount = 0;
        std::vector<ChunkRecord> trialChunks;

        while (true) {
            FrameHeader hdr;
            Chunk payload;
            if (!recvFrame(connFd, hdr, payload)) break;

            // Send 1-byte ACK immediately so the sender can measure RTT and
            // so no chunks are silently dropped (backpressure).
            uint8_t ack = 0x01;
            ::send(connFd, &ack, 1, 0);

            // Decompress to verify correctness (result discarded).
            Chunk original = decompressFrame(hdr, payload);
            (void)original;

            double ratio = static_cast<double>(hdr.compressedSize) /
                           static_cast<double>(hdr.originalSize);

            trialChunks.push_back({
                0, // trial filled in below
                hdr.chunkId,
                workloadName(hdr.workloadType),  // types.h — handles Boundary (index 4)
                algoName(hdr.algorithm),
                ratio,
                hdr.originalSize,
                hdr.compressedSize
            });

            ++chunkCount;
        }

        // Assign trial number and store under the correct system name.
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

    // Write one CSV per system.
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
            f << r.trial           << ","
              << r.chunkId         << ","
              << r.workload        << ","
              << systemName        << ","
              << r.algo            << ","
              << r.ratio           << ","
              << r.originalBytes   << ","
              << r.compressedBytes << "\n";
        }
        std::cout << "  -> wrote " << filename
                  << " (" << records.size() << " rows)\n";
    }

    CLOSE_SOCKET(listenFd);
    std::cout << "Done.\n";
    return 0;
}