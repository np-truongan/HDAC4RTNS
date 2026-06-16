// network_receiver.cpp — run this on the Raspberry Pi or any Linux/macOS machine
// Usage: ./network_receiver [port]   (default 54321)
//
// Platform support:
//   Linux / WSL2  : fully supported
//   macOS         : fully supported (POSIX sockets identical)
//   Windows native: NOT supported — use WSL2 instead

// ============================================================
//  Platform headers
// ============================================================
#if defined(_WIN32) && !defined(__CYGWIN__)
    // Windows native — not supported, but stub guard to give a clear error
    #error "Run this under WSL2 on Windows. Native Winsock is not supported."
#else
    // Linux, WSL2, macOS
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
#include <string>

static constexpr int DEFAULT_PORT = 54321;

static Chunk decompressFrame(const FrameHeader& hdr, const Chunk& payload) {
    Algorithm  algo = static_cast<Algorithm>(hdr.algorithm);
    Preprocess prep = static_cast<Preprocess>(hdr.preprocess);

    Chunk out;
    switch (algo) {
        case Algorithm::LZ4:  out = decompressLZ4(payload, hdr.originalSize); break;
        case Algorithm::ZSTD: out = decompressZSTD(payload);                  break;
        case Algorithm::GZIP: out = decompressGZIP(payload, hdr.originalSize); break;
    }
    if (prep == Preprocess::DELTA)
        out = deltaDecode(out);
    if (prep == Preprocess::BITPACK)
        out = bitPackDecode(out, hdr.originalSize);
    return out;
}

int main(int argc, char* argv[]) {
    int port = (argc > 1) ? std::stoi(argv[1]) : DEFAULT_PORT;

    int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) {
        std::cerr << "Failed to create socket\n";
        return 1;
    }

    int opt = 1;
    ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // macOS note: SO_REUSEPORT may also be needed on macOS if binding fails;
    // uncomment the line below if you see "address already in use" on macOS:
    // ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // accept on all interfaces
    addr.sin_port        = htons(static_cast<uint16_t>(port));

    if (::bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "Bind failed on port " << port << "\n";
        CLOSE_SOCKET(listenFd);
        return 1;
    }
    ::listen(listenFd, 4);

    std::cout << "Receiver listening on port " << port << " (Ctrl+C to stop)...\n";

    // Each trial from the sender is one TCP connection
    while (true) {
        int connFd = ::accept(listenFd, nullptr, nullptr);
        if (connFd < 0) break;

        int chunks = 0;
        while (true) {
            FrameHeader hdr;
            Chunk payload;
            if (!recvFrame(connFd, hdr, payload)) break;

            uint64_t recvUs = nowMicros();
            double e2eMs = static_cast<double>(recvUs - hdr.sendTimestampUs)
                           / 1000.0;

            // NOTE: e2eMs is only meaningful if both machines clocks are synced.
            // Run: sudo ntpdate pool.ntp.org  (or chronyc) on BOTH machines first.
            // On macOS: sntp -sS pool.ntp.org

            Chunk original = decompressFrame(hdr, payload);
            (void)original; // decompressed correctly; add a checksum here if needed

            std::cout << "chunk="    << hdr.chunkId
                      << " workload=" << static_cast<int>(hdr.workloadType)
                      << " algo="    << hdr.algorithm
                      << " ratio="   << static_cast<double>(hdr.compressedSize)
                                        / hdr.originalSize
                      << " e2e_ms=" << e2eMs << "\n";
            ++chunks;
        }

        std::cout << "[trial done — " << chunks << " chunks]\n";
        CLOSE_SOCKET(connFd);
    }

    CLOSE_SOCKET(listenFd);
    return 0;
}