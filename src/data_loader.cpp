#include "data_loader.h"
#include <fstream>
#include <stdexcept>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

Chunk loadFileToChunk(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + filepath);
    }
    std::streamsize size = file.tellg();
    if (size <= 0) {
        throw std::runtime_error("File is empty: " + filepath);
    }
    file.seekg(0, std::ios::beg);
    Chunk data(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
        throw std::runtime_error("Failed to read file: " + filepath);
    }
    std::cout << "[DataLoader] Loaded " << size << " bytes from " << filepath << "\n";
    return data;
}

std::vector<Chunk> loadAndChunkFile(const std::string& filepath, size_t chunkSize) {
    Chunk full = loadFileToChunk(filepath);
    std::vector<Chunk> chunks;
    chunks.reserve((full.size() + chunkSize - 1) / chunkSize);
    for (size_t off = 0; off < full.size(); off += chunkSize) {
        size_t len = std::min(chunkSize, full.size() - off);
        chunks.emplace_back(full.begin() + off, full.begin() + off + len);
    }
    return chunks;
}

std::string getBaseName(const std::string& filepath) {
    return fs::path(filepath).stem().string();
}

std::vector<std::pair<std::string, Chunk>> loadAllFilesFromDirectory(
    const std::string& dirpath,
    size_t targetBytes)
{
    std::vector<std::pair<std::string, Chunk>> result;
    std::error_code ec;

    if (!fs::exists(dirpath, ec)) {
        std::cerr << "[DataLoader] Directory does not exist: " << dirpath << "\n";
        return result;
    }

    for (const auto& entry : fs::directory_iterator(dirpath, ec)) {
        if (ec) {
            std::cerr << "[DataLoader] Error iterating directory: " << ec.message() << "\n";
            continue;
        }
        if (!entry.is_regular_file()) continue;

        std::string path = entry.path().string();

        // --- FILTER 1: Skip Windows Zone.Identifier files ---
        if (path.find(":Zone.Identifier") != std::string::npos) {
            std::cout << "[DataLoader] Skipping Zone.Identifier file: " << path << "\n";
            continue;
        }

        // --- FILTER 2 (Optional): Skip tiny files (< 1024 bytes) ---
        // This avoids loading garbage or log files that are not real PCAPs.
        // Comment out this block if you want to keep all files.
        try {
            if (entry.file_size() < 1024) {
                std::cout << "[DataLoader] Skipping tiny file: " << path
                          << " (" << entry.file_size() << " bytes)\n";
                continue;
            }
        } catch (...) {
            // If file_size() fails, just proceed cautiously.
        }

        std::string name = getBaseName(path);

        try {
            Chunk data = loadFileToChunk(path);

            if (targetBytes > 0) {
                if (data.size() > targetBytes) {
                    data.resize(targetBytes);
                    std::cout << "[DataLoader] Truncated " << path << " to " << targetBytes << " bytes.\n";
                } else if (data.size() < targetBytes) {
                    std::cout << "[DataLoader] Padding " << path << " (" << data.size()
                              << " bytes) with zeros to " << targetBytes << " bytes.\n";
                    data.resize(targetBytes, 0);
                }
            }

            result.emplace_back(name, std::move(data));
        } catch (const std::exception& e) {
            std::cerr << "[DataLoader] Skipping " << path << ": " << e.what() << "\n";
        }
    }

    std::cout << "[DataLoader] Loaded " << result.size() << " valid files from " << dirpath << "\n";
    return result;
}