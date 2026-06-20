#include "framing.h"

#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <stdexcept>

static bool writeAll(int fd, const void* buf, size_t n) {
    const char* p = static_cast<const char*>(buf);
    size_t remaining = n;

    while (remaining > 0) {
        ssize_t sent = ::write(fd, p, remaining);
        if (sent <= 0) return false;
        p         += sent;
        remaining -= static_cast<size_t>(sent);
    }
    return true;
}

static bool readAll(int fd, void* buf, size_t n) {
    char* p = static_cast<char*>(buf);
    size_t remaining = n;

    while (remaining > 0) {
        ssize_t got = ::read(fd, p, remaining);
        if (got <= 0) return false;   // EOF or error
        p         += got;
        remaining -= static_cast<size_t>(got);
    }
    return true;
}

bool sendFrame(int fd, const FrameHeader& header, const Chunk& payload) {
    // Validate sizes match
    if (payload.size() != header.compressedSize)
        return false;

    // Send header first, then payload
    if (!writeAll(fd, &header, sizeof(FrameHeader)))
        return false;

    if (!payload.empty())
        if (!writeAll(fd, payload.data(), payload.size()))
            return false;

    return true;
}

bool recvFrame(int fd, FrameHeader& header, Chunk& payload) {
    if (!readAll(fd, &header, sizeof(FrameHeader)))
        return false;

    if (header.magic != 0xADC0DE42)
        return false;

    payload.resize(header.compressedSize);
    if (header.compressedSize > 0)
        if (!readAll(fd, payload.data(), header.compressedSize))
            return false;

    return true;
}

uint64_t nowMicros() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(
            high_resolution_clock::now().time_since_epoch()
        ).count()
    );
}