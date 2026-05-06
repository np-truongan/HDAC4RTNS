#include "generators.h"
#include <random>
#include <string>

// ============================================================
//  Telemetry: smoothly varying numerical sequence.
//  Each byte increments by -1, 0, or +1 from the previous.
//  This gives smoothness ~0.99 and entropy ~2–3 bits.
// ============================================================
Chunk generateTelemetry(size_t size, unsigned seed) {
    Chunk data(size);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> delta(-1, 1);

    data[0] = 100;
    for (size_t i = 1; i < size; ++i) {
        int next = static_cast<int>(data[i - 1]) + delta(rng);
        // clamp to [0, 255] to stay within Byte range
        if (next < 0)   next = 0;
        if (next > 255) next = 255;
        data[i] = static_cast<Byte>(next);
    }
    return data;
}

// ============================================================
//  JSON: repeated structured pattern resembling sensor messages.
//  High repetition → moderate entropy (~3–4 bits).
// ============================================================
Chunk generateJSON(size_t size) {
    const std::string pattern =
        R"({"sensor":12,"temp":31,"humidity":55,"status":"OK"})";

    Chunk data;
    data.reserve(size);

    while (data.size() < size) {
        for (char c : pattern) {
            data.push_back(static_cast<Byte>(c));
            if (data.size() == size) break;
        }
    }
    return data;
}

// ============================================================
//  Binary: pseudo-random byte sequence.
//  Near-uniform distribution → entropy ~7.9 bits.
// ============================================================
Chunk generateBinary(size_t size, unsigned seed) {
    Chunk data(size);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> dist(0, 255);

    for (auto& b : data)
        b = static_cast<Byte>(dist(rng));

    return data;
}

Chunk generateNibble(size_t size, unsigned seed) {
    Chunk data(size);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> dist(0, 15);

    for (auto& b : data)
        b = static_cast<Byte>(dist(rng));

    return data;
}