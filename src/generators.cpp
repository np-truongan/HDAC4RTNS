#include "generators.h"
#include <algorithm>
#include <string>

Chunk generateTelemetry(size_t size, unsigned seed) {
    Chunk data(size);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> delta(-1, 1);

    data[0] = 100;
    for (size_t i = 1; i < size; ++i) {
        int next = static_cast<int>(data[i - 1]) + delta(rng);
        if (next < 0)   next = 0;
        if (next > 255) next = 255;
        data[i] = static_cast<Byte>(next);
    }
    return data;
}

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

Chunk generateBoundaryTelemetry(size_t size, unsigned seed, float noise_scale) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> small_step(-1, 1);
    std::uniform_int_distribution<int> large_step(-127, 127);
    std::uniform_real_distribution<float> coin(0.0f, 1.0f);

    Chunk out(size);
    out[0] = 100;
    for (size_t i = 1; i < size; ++i) {
        int delta = (coin(rng) < noise_scale)
                    ? large_step(rng)
                    : small_step(rng);
        int next = static_cast<int>(out[i - 1]) + delta;
        out[i] = static_cast<Byte>(std::clamp(next, 0, 255));
    }
    return out;
}

Chunk generateBoundary(size_t size) {
    return generateBoundaryTelemetry(size, 42, 0.5f);
}