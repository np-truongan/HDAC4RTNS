#include "strategies.h"

#include <lz4.h>
#include <zstd.h>
#include <zlib.h>

#include <stdexcept>
#include <cassert>

Chunk deltaEncode(const Chunk& data) {
    if (data.empty()) return {};
    Chunk out(data.size());
    out[0] = data[0];
    for (size_t i = 1; i < data.size(); ++i)
        out[i] = static_cast<Byte>(
            static_cast<int>(data[i]) - static_cast<int>(data[i - 1]));
    return out;
}

Chunk deltaDecode(const Chunk& data) {
    if (data.empty()) return {};
    Chunk out(data.size());
    out[0] = data[0];
    for (size_t i = 1; i < data.size(); ++i)
        out[i] = static_cast<Byte>(
            static_cast<int>(out[i - 1]) + static_cast<int>(data[i]));
    return out;
}

Chunk bitPackEncode(const Chunk& data) {
    size_t outSize = (data.size() + 1) / 2;
    Chunk out(outSize, 0);
    for (size_t i = 0; i < data.size(); ++i) {
        if (i % 2 == 0)
            out[i / 2] = static_cast<Byte>((data[i] & 0x0F) << 4);
        else
            out[i / 2] |= static_cast<Byte>(data[i] & 0x0F);
    }
    return out;
}

Chunk bitPackDecode(const Chunk& encoded, size_t originalSize) {
    Chunk out(originalSize);
    for (size_t i = 0; i < originalSize; ++i) {
        if (i % 2 == 0)
            out[i] = static_cast<Byte>((encoded[i / 2] >> 4) & 0x0F);
        else
            out[i] = static_cast<Byte>(encoded[i / 2] & 0x0F);
    }
    return out;
}

Chunk compressLZ4(const Chunk& data) {
    int maxSize = LZ4_compressBound(static_cast<int>(data.size()));
    Chunk out(static_cast<size_t>(maxSize));
    int compressedSize = LZ4_compress_default(
        reinterpret_cast<const char*>(data.data()),
        reinterpret_cast<char*>(out.data()),
        static_cast<int>(data.size()),
        maxSize);
    if (compressedSize <= 0) return {};
    out.resize(static_cast<size_t>(compressedSize));
    return out;
}

Chunk decompressLZ4(const Chunk& compressed, size_t originalSize) {
    Chunk out(originalSize);
    int result = LZ4_decompress_safe(
        reinterpret_cast<const char*>(compressed.data()),
        reinterpret_cast<char*>(out.data()),
        static_cast<int>(compressed.size()),
        static_cast<int>(originalSize));
    if (result < 0) return {};
    return out;
}

Chunk compressZSTD(const Chunk& data) {
    size_t maxSize = ZSTD_compressBound(data.size());
    Chunk out(maxSize);
    size_t compressedSize = ZSTD_compress(
        out.data(), maxSize,
        data.data(), data.size(),
        1);
    if (ZSTD_isError(compressedSize)) return {};
    out.resize(compressedSize);
    return out;
}

Chunk decompressZSTD(const Chunk& compressed) {
    unsigned long long originalSize =
        ZSTD_getFrameContentSize(compressed.data(), compressed.size());
    if (originalSize == ZSTD_CONTENTSIZE_ERROR ||
        originalSize == ZSTD_CONTENTSIZE_UNKNOWN)
        return {};
    Chunk out(static_cast<size_t>(originalSize));
    size_t result = ZSTD_decompress(
        out.data(), out.size(),
        compressed.data(), compressed.size());
    if (ZSTD_isError(result)) return {};
    return out;
}

Chunk compressGZIP(const Chunk& data) {
    z_stream zs{};
    if (deflateInit2(&zs, Z_BEST_SPEED, Z_DEFLATED,
                     15 | 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return {};
    uLongf bound = deflateBound(&zs, static_cast<uLong>(data.size()));
    Chunk out(bound);
    zs.next_in   = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data.data()));
    zs.avail_in  = static_cast<uInt>(data.size());
    zs.next_out  = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(bound);
    int ret = deflate(&zs, Z_FINISH);
    deflateEnd(&zs);
    if (ret != Z_STREAM_END) return {};
    out.resize(zs.total_out);
    return out;
}

Chunk decompressGZIP(const Chunk& compressed, size_t originalSize) {
    z_stream zs{};
    if (inflateInit2(&zs, 15 | 16) != Z_OK) return {};
    Chunk out(originalSize);
    zs.next_in   = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(compressed.data()));
    zs.avail_in  = static_cast<uInt>(compressed.size());
    zs.next_out  = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(originalSize);
    int ret = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    if (ret != Z_STREAM_END) return {};
    return out;
}

CompressResult compressWithDecision(const Chunk& input, const Decision& d) {
    Chunk working = input;

    if (d.preprocess == Preprocess::DELTA) {
        working = deltaEncode(working);
    } else if (d.preprocess == Preprocess::BITPACK) {
        working = bitPackEncode(working);
    }

    Chunk compressed;
    switch (d.algorithm) {
        case Algorithm::LZ4:  compressed = compressLZ4(working);  break;
        case Algorithm::ZSTD: compressed = compressZSTD(working); break;
        case Algorithm::GZIP: compressed = compressGZIP(working); break;
    }

    CompressResult r;
    r.original_bytes   = input.size();
    r.compressed_bytes = compressed.size();
    r.ratio            = compressed.empty()
                         ? 0.0
                         : static_cast<double>(compressed.size()) / input.size();
    return r;
}

size_t sizeAfterPreprocess(const Decision& d, size_t originalSize) {
    if (d.preprocess == Preprocess::BITPACK) return (originalSize + 1) / 2;
    return originalSize;
}

Chunk decompressWithDecision(const Chunk& compressed,
                              const Decision& d,
                              size_t originalSize)
{
    size_t preprocessedSize = sizeAfterPreprocess(d, originalSize);

    Chunk processed;
    switch (d.algorithm) {
        case Algorithm::LZ4:  processed = decompressLZ4(compressed, preprocessedSize);  break;
        case Algorithm::ZSTD: processed = decompressZSTD(compressed);                    break;
        case Algorithm::GZIP: processed = decompressGZIP(compressed, preprocessedSize); break;
    }

    switch (d.preprocess) {
        case Preprocess::DELTA:   return deltaDecode(processed);
        case Preprocess::BITPACK: return bitPackDecode(processed, originalSize);
        case Preprocess::NONE:    return processed;
    }
    return processed;
}