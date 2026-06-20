#include "generators.h"
#include "strategies.h"
#include "types.h"

#include <iostream>
#include <string>

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
    std::cout << "  [FAIL] " << name << " -- " << reason << "\n";
    std::exit(1);
}

void testDeltaRoundTrip() {
    std::cout << "\n--- Delta encoding ---\n";

    {
        Chunk orig = generateTelemetry(4096);
        if (!chunksEqual(orig, deltaDecode(deltaEncode(orig))))
            fail("Telemetry delta round-trip", "decoded != original");
        pass("Telemetry delta round-trip (4096 bytes)");
    }
    {
        Chunk orig = generateBinary(4096);
        if (!chunksEqual(orig, deltaDecode(deltaEncode(orig))))
            fail("Binary delta round-trip", "decoded != original");
        pass("Binary delta round-trip (4096 bytes)");
    }
    {
        Chunk orig = {42};
        if (!chunksEqual(orig, deltaDecode(deltaEncode(orig))))
            fail("Single-byte delta round-trip", "decoded != original");
        pass("Single-byte delta round-trip");
    }
    {
        Chunk orig = {};
        if (!chunksEqual(orig, deltaDecode(deltaEncode(orig))))
            fail("Empty delta round-trip", "decoded != original");
        pass("Empty delta round-trip");
    }
}

void testBitPackRoundTrip() {
    std::cout << "\n--- Bit-packing encoding ---\n";

    {
        Chunk orig = generateNibble(4096);
        Chunk enc  = bitPackEncode(orig);
        Chunk dec  = bitPackDecode(enc, orig.size());
        if (!chunksEqual(orig, dec))
            fail("Nibble bitpack round-trip", "decoded != original");
        pass("Nibble bitpack round-trip (4096 bytes)");

        if (enc.size() != (orig.size() + 1) / 2)
            fail("Nibble bitpack size", "expected half size");
        pass("Nibble bitpack size reduction (4096 -> 2048 bytes)");
    }
    {
        Chunk orig = {7};
        if (!chunksEqual(orig, bitPackDecode(bitPackEncode(orig), orig.size())))
            fail("Single-value bitpack round-trip", "decoded != original");
        pass("Single-value bitpack round-trip");
    }
    {
        Chunk orig = {3, 14};
        if (!chunksEqual(orig, bitPackDecode(bitPackEncode(orig), orig.size())))
            fail("Two-value bitpack round-trip", "decoded != original");
        pass("Two-value bitpack round-trip");
    }
}

void testLZ4RoundTrip() {
    std::cout << "\n--- LZ4 ---\n";
    for (auto& [name, data] : std::vector<std::pair<std::string, Chunk>>{
        { "Telemetry 4KB", generateTelemetry(4096) },
        { "JSON 4KB",      generateJSON(4096)      },
        { "Binary 4KB",    generateBinary(4096)    }
    }) {
        Chunk c = compressLZ4(data);
        if (c.empty()) fail("LZ4 compress " + name, "returned empty");
        if (!chunksEqual(data, decompressLZ4(c, data.size())))
            fail("LZ4 round-trip " + name, "decompressed != original");
        pass("LZ4 round-trip -- " + name);
    }
}

void testZSTDRoundTrip() {
    std::cout << "\n--- ZSTD ---\n";
    for (auto& [name, data] : std::vector<std::pair<std::string, Chunk>>{
        { "Telemetry 4KB", generateTelemetry(4096) },
        { "JSON 4KB",      generateJSON(4096)      },
        { "Binary 4KB",    generateBinary(4096)    }
    }) {
        Chunk c = compressZSTD(data);
        if (c.empty()) fail("ZSTD compress " + name, "returned empty");
        if (!chunksEqual(data, decompressZSTD(c)))
            fail("ZSTD round-trip " + name, "decompressed != original");
        pass("ZSTD round-trip -- " + name);
    }
}

void testGZIPRoundTrip() {
    std::cout << "\n--- GZIP ---\n";
    for (auto& [name, data] : std::vector<std::pair<std::string, Chunk>>{
        { "Telemetry 4KB", generateTelemetry(4096) },
        { "JSON 4KB",      generateJSON(4096)      },
        { "Binary 4KB",    generateBinary(4096)    }
    }) {
        Chunk c = compressGZIP(data);
        if (c.empty()) fail("GZIP compress " + name, "returned empty");
        if (!chunksEqual(data, decompressGZIP(c, data.size())))
            fail("GZIP round-trip " + name, "decompressed != original");
        pass("GZIP round-trip -- " + name);
    }
}

void testDeltaPlusZSTD() {
    std::cout << "\n--- Delta + ZSTD (Telemetry adaptive path) ---\n";

    Chunk orig       = generateTelemetry(4096);
    Chunk encoded    = deltaEncode(orig);
    Chunk compressed = compressZSTD(encoded);
    if (compressed.empty())
        fail("Delta+ZSTD compress", "returned empty");

    Chunk decoded = deltaDecode(decompressZSTD(compressed));
    if (!chunksEqual(orig, decoded))
        fail("Delta+ZSTD round-trip", "final decoded != original");
    pass("Delta+ZSTD full pipeline round-trip (4096 bytes)");
}

void testBitPackPlusZSTD() {
    std::cout << "\n--- BitPack + ZSTD (Nibble adaptive path) ---\n";

    Chunk orig       = generateNibble(4096);
    Chunk packed     = bitPackEncode(orig);
    Chunk compressed = compressZSTD(packed);
    if (compressed.empty())
        fail("BitPack+ZSTD compress", "returned empty");

    Chunk decoded = bitPackDecode(decompressZSTD(compressed), orig.size());
    if (!chunksEqual(orig, decoded))
        fail("BitPack+ZSTD round-trip", "final decoded != original");
    pass("BitPack+ZSTD full pipeline round-trip (4096 bytes)");
}

int main() {
    std::cout << "========================================\n";
    std::cout << "  Round-Trip Correctness Tests\n";
    std::cout << "========================================\n";

    testDeltaRoundTrip();
    testBitPackRoundTrip();
    testLZ4RoundTrip();
    testZSTDRoundTrip();
    testGZIPRoundTrip();
    testDeltaPlusZSTD();
    testBitPackPlusZSTD();

    std::cout << "\n========================================\n";
    std::cout << "  All tests passed.\n";
    std::cout << "========================================\n";

    return 0;
}