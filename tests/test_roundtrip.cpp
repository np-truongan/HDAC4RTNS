// tests/test_roundtrip.cpp
//
// Week 2 correctness gate.
//
// Before trusting any compression number, verify that every
// encode → compress → decompress → decode pipeline returns
// bit-identical data to its input.  This must pass cleanly
// before any benchmark results are considered valid.
// ============================================================

#include "generators.h"
#include "strategies.h"
#include "types.h"

#include <iostream>
#include <string>
#include <cassert>

// ============================================================
//  Helpers
// ============================================================
static bool chunksEqual(const Chunk& a, const Chunk& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) return false;
    return true;
}

static void pass(const std::string& name) {
    std::cout << "  [PASS] " << name << "\n";
}

static void fail(const std::string& name, const std::string& reason) {
    std::cout << "  [FAIL] " << name << " — " << reason << "\n";
    std::exit(1);
}

// ============================================================
//  Delta round-trip
// ============================================================
void testDeltaRoundTrip() {
    std::cout << "\n--- Delta encoding ---\n";

    // Smooth data: the primary use case
    {
        Chunk original = generateTelemetry(4096);
        Chunk encoded  = deltaEncode(original);
        Chunk decoded  = deltaDecode(encoded);

        if (!chunksEqual(original, decoded))
            fail("Telemetry delta round-trip", "decoded != original");
        pass("Telemetry delta round-trip (4096 bytes)");
    }

    // Arbitrary data: must also be lossless
    {
        Chunk original = generateBinary(4096);
        Chunk encoded  = deltaEncode(original);
        Chunk decoded  = deltaDecode(encoded);

        if (!chunksEqual(original, decoded))
            fail("Binary delta round-trip", "decoded != original");
        pass("Binary delta round-trip (4096 bytes)");
    }

    // Edge case: single byte
    {
        Chunk original = {42};
        Chunk encoded  = deltaEncode(original);
        Chunk decoded  = deltaDecode(encoded);

        if (!chunksEqual(original, decoded))
            fail("Single-byte delta round-trip", "decoded != original");
        pass("Single-byte delta round-trip");
    }

    // Edge case: empty chunk
    {
        Chunk original = {};
        Chunk encoded  = deltaEncode(original);
        Chunk decoded  = deltaDecode(encoded);

        if (!chunksEqual(original, decoded))
            fail("Empty delta round-trip", "decoded != original");
        pass("Empty delta round-trip");
    }
}

// ============================================================
//  Compressor round-trips
// ============================================================
void testLZ4RoundTrip() {
    std::cout << "\n--- LZ4 ---\n";

    for (auto& [name, data] : std::vector<std::pair<std::string, Chunk>>{
        { "Telemetry 4KB", generateTelemetry(4096) },
        { "JSON 4KB",      generateJSON(4096)      },
        { "Binary 4KB",    generateBinary(4096)    }
    }) {
        Chunk compressed   = compressLZ4(data);
        if (compressed.empty())
            fail("LZ4 compress " + name, "returned empty");

        Chunk decompressed = decompressLZ4(compressed, data.size());
        if (!chunksEqual(data, decompressed))
            fail("LZ4 round-trip " + name, "decompressed != original");

        pass("LZ4 round-trip — " + name);
    }
}

void testZSTDRoundTrip() {
    std::cout << "\n--- ZSTD ---\n";

    for (auto& [name, data] : std::vector<std::pair<std::string, Chunk>>{
        { "Telemetry 4KB", generateTelemetry(4096) },
        { "JSON 4KB",      generateJSON(4096)      },
        { "Binary 4KB",    generateBinary(4096)    }
    }) {
        Chunk compressed   = compressZSTD(data);
        if (compressed.empty())
            fail("ZSTD compress " + name, "returned empty");

        Chunk decompressed = decompressZSTD(compressed);
        if (!chunksEqual(data, decompressed))
            fail("ZSTD round-trip " + name, "decompressed != original");

        pass("ZSTD round-trip — " + name);
    }
}

void testGZIPRoundTrip() {
    std::cout << "\n--- GZIP ---\n";

    for (auto& [name, data] : std::vector<std::pair<std::string, Chunk>>{
        { "Telemetry 4KB", generateTelemetry(4096) },
        { "JSON 4KB",      generateJSON(4096)      },
        { "Binary 4KB",    generateBinary(4096)    }
    }) {
        Chunk compressed   = compressGZIP(data);
        if (compressed.empty())
            fail("GZIP compress " + name, "returned empty");

        Chunk decompressed = decompressGZIP(compressed, data.size());
        if (!chunksEqual(data, decompressed))
            fail("GZIP round-trip " + name, "decompressed != original");

        pass("GZIP round-trip — " + name);
    }
}

// ============================================================
//  Delta + compress combined round-trip
//  (the actual path used by the adaptive pipeline)
// ============================================================
void testDeltaPlusCompressRoundTrip() {
    std::cout << "\n--- Delta + ZSTD (adaptive pipeline path) ---\n";

    Chunk original  = generateTelemetry(4096);
    Chunk encoded   = deltaEncode(original);
    Chunk compressed = compressZSTD(encoded);

    if (compressed.empty())
        fail("Delta+ZSTD compress", "returned empty");

    Chunk decompressed = decompressZSTD(compressed);
    Chunk decoded      = deltaDecode(decompressed);

    if (!chunksEqual(original, decoded))
        fail("Delta+ZSTD round-trip", "final decoded != original");

    pass("Delta+ZSTD full pipeline round-trip (4096 bytes)");
}

// ============================================================
//  Main
// ============================================================
int main() {
    std::cout << "========================================\n";
    std::cout << "  Round-Trip Correctness Tests\n";
    std::cout << "========================================\n";

    testDeltaRoundTrip();
    testLZ4RoundTrip();
    testZSTDRoundTrip();
    testGZIPRoundTrip();
    testDeltaPlusCompressRoundTrip();

    std::cout << "\n========================================\n";
    std::cout << "  All tests passed.\n";
    std::cout << "========================================\n";

    return 0;
}
