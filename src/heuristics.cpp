#include "heuristics.h"
#include <array>
#include <cmath>
#include <stdexcept>

double computeEntropy(const Chunk& data) {
    if (data.empty()) return 0.0;

    std::array<int, 256> freq{};
    freq.fill(0);

    for (Byte b : data)
        freq[b]++;

    const double n = static_cast<double>(data.size());
    double entropy = 0.0;

    for (int count : freq) {
        if (count == 0) continue;
        double p = count / n;
        entropy -= p * std::log2(p);
    }

    return entropy;
}

double computeSmoothness(const Chunk& data) {
    if (data.size() < 2) return 0.0;

    int smooth = 0;
    for (size_t i = 1; i < data.size(); ++i) {
        if (std::abs(static_cast<int>(data[i]) -
                     static_cast<int>(data[i - 1])) <= 2)
            ++smooth;
    }

    return static_cast<double>(smooth) /
           static_cast<double>(data.size() - 1);
}
Features extractFeatures(const Chunk& data) {
    return {
        computeEntropy(data),
        computeSmoothness(data)
    };
}
