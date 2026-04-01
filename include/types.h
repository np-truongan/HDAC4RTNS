#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <cstddef>

// ============================================================
//  Primitive types
// ============================================================
using Byte  = uint8_t;
using Chunk = std::vector<Byte>;

// ============================================================
//  Compression strategies
// ============================================================
enum class Algorithm {
    LZ4,
    ZSTD,
    GZIP
};

enum class Preprocess {
    NONE,
    DELTA,
    BITPACK
};

// ============================================================
//  Heuristic features extracted from a chunk
// ============================================================
struct Features {
    double entropy    = 0.0;   // Shannon entropy  [0, 8]
    double smoothness = 0.0;   // Sequential similarity [0, 1]
};

// ============================================================
//  Decision produced by the engine
// ============================================================
struct Decision {
    Algorithm  algorithm   = Algorithm::ZSTD;
    Preprocess preprocess  = Preprocess::NONE;
};

// ============================================================
//  Per-chunk result recorded during a benchmark run
// ============================================================
struct ChunkResult {
    size_t      originalSize    = 0;
    size_t      compressedSize  = 0;
    double      latencyMs       = 0.0;
    double      compressionRatio = 0.0;   // compressed / original
    Features    features;
    Decision    decision;
    std::string workloadType;             // "Telemetry" | "JSON" | "Binary"
};

// ============================================================
//  Aggregate metrics across a full benchmark run
// ============================================================
struct RunMetrics {
    double avgCompressionRatio = 0.0;
    double avgLatencyMs        = 0.0;
    double jitterMs            = 0.0;     // std-dev of latency
    double avgThroughputMBps   = 0.0;
    double totalOriginalMB     = 0.0;
    double totalCompressedMB   = 0.0;
    std::string systemName;
};

// ============================================================
//  Helper: algorithm / preprocess name strings (for CSV/logs)
// ============================================================
inline std::string toString(Algorithm a) {
    switch (a) {
        case Algorithm::LZ4:  return "LZ4";
        case Algorithm::ZSTD: return "ZSTD";
        case Algorithm::GZIP: return "GZIP";
    }
    return "UNKNOWN";
}

inline std::string toString(Preprocess p) {
    switch (p) {
        case Preprocess::NONE:    return "None";
        case Preprocess::DELTA:   return "Delta";
        case Preprocess::BITPACK: return "BitPack";
    }
    return "UNKNOWN";
}
