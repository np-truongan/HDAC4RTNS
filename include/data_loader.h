#pragma once

#include "types.h"
#include <string>
#include <vector>

Chunk loadFileToChunk(const std::string& filepath);

std::vector<Chunk> loadAndChunkFile(const std::string& filepath, size_t chunkSize);

std::string getBaseName(const std::string& filepath);

std::vector<std::pair<std::string, Chunk>> loadAllFilesFromDirectory(
    const std::string& dirpath,
    size_t targetBytes = 0 
);