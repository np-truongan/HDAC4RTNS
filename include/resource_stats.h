#pragma once

#include <sys/resource.h>
#include <sys/time.h>
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <chrono>
#include <functional>

struct ResourceSnapshot {
    double   userTimeMs = 0.0;
    double   sysTimeMs  = 0.0;
    long     maxRssKb   = 0;
};

struct ResourceDelta {
    std::string label;
    double      userTimeMs = 0.0;
    double      sysTimeMs  = 0.0;
    double      cpuTimeMs  = 0.0;
    long        peakRssKb  = 0;
};

inline ResourceSnapshot captureResourceSnapshot() {
    ResourceSnapshot snap;
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        snap.userTimeMs = usage.ru_utime.tv_sec * 1000.0 + usage.ru_utime.tv_usec / 1000.0;
        snap.sysTimeMs  = usage.ru_stime.tv_sec * 1000.0 + usage.ru_stime.tv_usec / 1000.0;
#if defined(__APPLE__)
        snap.maxRssKb = usage.ru_maxrss / 1024;
#else
        snap.maxRssKb = usage.ru_maxrss;
#endif
    }
    return snap;
}

inline ResourceDelta diffResourceSnapshot(
    const ResourceSnapshot& before,
    const ResourceSnapshot& after,
    const std::string& label)
{
    ResourceDelta d;
    d.label      = label;
    d.userTimeMs = after.userTimeMs - before.userTimeMs;
    d.sysTimeMs  = after.sysTimeMs  - before.sysTimeMs;
    d.cpuTimeMs  = d.userTimeMs + d.sysTimeMs;
    d.peakRssKb  = after.maxRssKb;
    return d;
}

inline void printResourceDelta(const ResourceDelta& d) {
    std::cout << "Label          : " << d.label << "\n"
              << "User CPU (ms)  : " << d.userTimeMs << "\n"
              << "Sys CPU (ms)   : " << d.sysTimeMs  << "\n"
              << "Total CPU (ms) : " << d.cpuTimeMs  << "\n"
              << "Peak RSS (KB)  : " << d.peakRssKb  << "\n";
}

inline void saveResourceDeltasCSV(
    const std::vector<ResourceDelta>& deltas,
    const std::string& filepath)
{
    std::ofstream f(filepath);
    if (!f.is_open()) return;
    f << "label,user_cpu_ms,sys_cpu_ms,total_cpu_ms,peak_rss_kb\n";
    for (const auto& d : deltas) {
        f << d.label      << ","
          << d.userTimeMs << ","
          << d.sysTimeMs  << ","
          << d.cpuTimeMs  << ","
          << d.peakRssKb  << "\n";
    }
}

template <typename F>
struct CompressionMeasurement {
    Chunk compressed;
    double wallMs;
    double cpuMs;
    long   peakRssKb;
};

template <typename F>
CompressionMeasurement<F> measureCompression(F&& compressFn) {
    using Clock = std::chrono::high_resolution_clock;

    ResourceSnapshot before = captureResourceSnapshot();
    auto t0 = Clock::now();

    Chunk compressed = compressFn();

    auto t1 = Clock::now();
    ResourceSnapshot after = captureResourceSnapshot();

    double wallMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    ResourceDelta delta = diffResourceSnapshot(before, after, "");

    return { std::move(compressed), wallMs, delta.cpuTimeMs, after.maxRssKb };
}