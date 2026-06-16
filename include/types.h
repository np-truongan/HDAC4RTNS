#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <cstddef>

using Byte  = uint8_t;
using Chunk = std::vector<Byte>;

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

struct Features {
    double entropy    = 0.0;
    double smoothness = 0.0;
};

struct Decision {
    Algorithm  algorithm   = Algorithm::ZSTD;
    Preprocess preprocess  = Preprocess::NONE;
};

struct CompressResult {
    double ratio            = 0.0;
    size_t compressed_bytes = 0;
    size_t original_bytes   = 0;
};

struct ChunkResult {
    size_t      originalSize     = 0;
    size_t      compressedSize   = 0;
    double      latencyMs        = 0.0;
    double      compressionRatio = 0.0;
    Features    features;
    Decision    decision;
    std::string workloadType;
    Chunk       compressedData;
};

struct RunMetrics {
    double avgCompressionRatio = 0.0;
    double avgLatencyMs        = 0.0;
    double jitterMs            = 0.0;
    double avgThroughputMBps   = 0.0;
    double totalOriginalMB     = 0.0;
    double totalCompressedMB   = 0.0;
    std::string systemName;
};

struct FrameHeader {
    uint32_t magic           = 0xADC0DE42;
    uint32_t chunkId         = 0;
    uint32_t originalSize    = 0;
    uint32_t preprocessedSize = 0;
    uint32_t compressedSize  = 0;
    uint8_t  algorithm       = 0;
    uint8_t  preprocess      = 0;
    uint8_t  workloadType    = 0;
    uint8_t  _pad            = 0;
    uint64_t sendTimestampUs = 0;
};

inline std::string workloadName(uint8_t idx) {
    switch (idx) {
        case 0: return "Telemetry";
        case 1: return "JSON";
        case 2: return "Binary";
        case 3: return "Nibble";
        case 4: return "Boundary";
    }
    return "Unknown";
}

inline uint8_t workloadIndex(const std::string& name) {
    if (name == "Telemetry") return 0;
    if (name == "JSON")      return 1;
    if (name == "Binary")    return 2;
    if (name == "Nibble")    return 3;
    if (name == "Boundary")  return 4;
    return 255;
}

struct NetworkResult {
    uint32_t    chunkId;
    std::string workload;
    std::string algorithm;
    std::string preprocess;
    size_t      originalSize;
    size_t      compressedSize;
    double      compressionRatio;
    double      endToEndLatencyMs;
};

struct TrialStats {
    std::string label;
    int         n        = 0;
    double      mean     = 0.0;
    double      stdev    = 0.0;
    double      ci95Low  = 0.0;
    double      ci95High = 0.0;
    double      min      = 0.0;
    double      max      = 0.0;
};

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