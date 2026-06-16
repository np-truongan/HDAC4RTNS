#pragma once

#include "types.h"

Chunk deltaEncode(const Chunk& data);
Chunk deltaDecode(const Chunk& data);

Chunk bitPackEncode(const Chunk& data);
Chunk bitPackDecode(const Chunk& encoded, size_t originalSize);

Chunk compressLZ4 (const Chunk& data);
Chunk compressZSTD(const Chunk& data);
Chunk compressGZIP(const Chunk& data);

Chunk decompressLZ4 (const Chunk& compressed, size_t originalSize);
Chunk decompressZSTD(const Chunk& compressed);
Chunk decompressGZIP(const Chunk& compressed, size_t originalSize);

CompressResult compressWithDecision(const Chunk& input, const Decision& d);

size_t sizeAfterPreprocess(const Decision& d, size_t originalSize);

Chunk decompressWithDecision(const Chunk& compressed, const Decision& d, size_t originalSize);