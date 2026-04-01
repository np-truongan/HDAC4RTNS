#include "heuristics.h"
#include <array>
#include <cmath>
#include <stdexcept>

// ============================================================
//  Shannon entropy over byte-frequency distribution.
//
//  We use a fixed 256-bucket frequency table (one pass) then
//  compute the standard entropy formula.  This is O(n) in the
//  chunk size with very low constant — suitable for inline use
//  in a streaming pipeline.
// ============================================================
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

// ============================================================
//  Smoothness: proportion of adjacent pairs with |diff| <= 2.
//
//  Threshold of 2 is intentional — it captures byte-level
//  sequential correlation without being too sensitive to
//  noise in near-smooth data (e.g. slowly drifting telemetry).
// ============================================================
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

// ============================================================
//  Convenience wrapper
// ============================================================
Features extractFeatures(const Chunk& data) {
    return {
        computeEntropy(data),
        computeSmoothness(data)
    };
}
