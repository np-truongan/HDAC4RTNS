#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <cerrno>
#include <cstring>
#define CLOSE_SOCKET(fd) ::close(fd)

#include "framing.h"
#include "strategies.h"
#include "resource_stats.h"
#include "types.h"

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <filesystem>

static constexpr int DEFAULT_PORT = 54321;
static int g_listenFd = -1;

static void signalHandler(int sig) {
    if (sig == SIGINT && g_listenFd >= 0) {
        std::cout << "\nShutting down receiver...\n";
        CLOSE_SOCKET(g_listenFd);
        g_listenFd = -1;
    }
}

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

struct ChunkRecord {
    int         trial;
    uint32_t    chunkId;
    std::string workload;
    std::string algo;
    double      ratio;
    uint32_t    originalBytes;
    uint32_t    compressedBytes;
    double      decompressCpuMs;
    long        peakRssKb;
};

int main(int argc, char* argv[]) {
    int         port   = (argc > 1) ? std::stoi(argv[1]) : DEFAULT_PORT;
    std::string outDir = (argc > 2) ? argv[2] : "./results";

    std::filesystem::create_directories(outDir);

    g_listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (g_listenFd < 0) {
        std::cerr << "Failed to create socket: " << std::strerror(errno) << "\n";
        return 1;
    }

    int opt = 1;
    ::setsockopt(g_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(static_cast<uint16_t>(port));

    if (::bind(g_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "Bind failed on port " << port << ": " << std::strerror(errno) << "\n";
        if (port < 1024)
            std::cerr << "Ports below 1024 require root privileges. Try a port >= 1024.\n";
        CLOSE_SOCKET(g_listenFd);
        return 1;
    }
    ::listen(g_listenFd, 8);

    signal(SIGINT, signalHandler);

    std::cout << "Receiver listening on port " << port << "\n";
    std::cout << "Output directory: " << outDir << "\n";
    std::cout << "Press Ctrl+C to stop and write CSVs.\n\n";

    std::map<std::string, std::vector<ChunkRecord>> systemRecords;
    std::map<std::string, int> trialCounters;
    int connectionCount = 0;

    while (true) {
        int connFd = ::accept(g_listenFd, nullptr, nullptr);
        if (connFd < 0) {
            if (g_listenFd == -1) {
                std::cout << "Listening socket closed. Finishing.\n";
            } else {
                std::cerr << "accept failed: " << std::strerror(errno) << "\n";
            }
            break;
        }
        ++connectionCount;

        uint8_t nameLen = 0;
        if (::recv(connFd, &nameLen, 1, MSG_WAITALL) != 1 || nameLen == 0) {
            std::cerr << "  [conn " << connectionCount << "] bad handshake, skipping\n";
            CLOSE_SOCKET(connFd);
            continue;
        }
        std::string currentSystem(nameLen, '\0');
        if (::recv(connFd, currentSystem.data(), nameLen, MSG_WAITALL)
                != static_cast<ssize_t>(nameLen)) {
            std::cerr << "  [conn " << connectionCount << "] truncated system name, skipping\n";
            CLOSE_SOCKET(connFd);
            continue;
        }

        int chunkCount = 0;
        std::vector<ChunkRecord> trialChunks;

        while (true) {
            FrameHeader hdr;
            Chunk payload;
            if (!recvFrame(connFd, hdr, payload)) break;

            uint8_t ack = 0x01;
            ::send(connFd, &ack, 1, 0);

            auto meas = measureCompression([&]() {
                return decompressFrame(hdr, payload);
            });

            if (meas.compressed.empty()) {
                std::cerr << "  Decompression failed on chunk " << hdr.chunkId << "\n";
                break;
            }

            double ratio = static_cast<double>(hdr.compressedSize) /
                           static_cast<double>(hdr.originalSize);

            trialChunks.push_back({
                0,
                hdr.chunkId,
                workloadName(hdr.workloadType),
                algoName(hdr.algorithm),
                ratio,
                hdr.originalSize,
                hdr.compressedSize,
                meas.cpuMs,
                meas.peakRssKb
            });

            ++chunkCount;
        }

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

    std::cout << "\nWriting CSVs...\n";
    if (systemRecords.empty()) {
        std::cout << "No data collected.\n";
        return 0;
    }

    for (const auto& [systemName, records] : systemRecords) {
        std::string filename = outDir + "/receiver_" + systemName + ".csv";
        std::ofstream f(filename);
        if (!f.is_open()) {
            std::cerr << "Cannot write: " << filename << "\n";
            continue;
        }
        f << "trial,chunk_id,workload,system,algo,ratio,original_bytes,compressed_bytes,decompress_cpu_ms,peak_rss_kb\n";
        for (const auto& r : records) {
            f << r.trial           << ","
              << r.chunkId         << ","
              << r.workload        << ","
              << systemName        << ","
              << r.algo            << ","
              << r.ratio           << ","
              << r.originalBytes   << ","
              << r.compressedBytes << ","
              << r.decompressCpuMs << ","
              << r.peakRssKb       << "\n";
        }
        f.close();
        std::cout << "  -> " << filename << " (" << records.size() << " rows)\n";
    }

    CLOSE_SOCKET(g_listenFd);
    std::cout << "Done.\n";
    return 0;
}