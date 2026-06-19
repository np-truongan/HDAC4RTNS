#pragma once

#include <sys/resource.h>
#include <sys/time.h>
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <iostream>

struct ResourceSnapshot {
    double   userTimeMs = 0.0;
    double   sysTimeMs  = 0.0;
    long     maxRssKb   = 0;   // peak RSS at time of snapshot (cumulative since process start)
};

struct ResourceDelta {
    std::string label;
    double      userTimeMs = 0.0;  // CPU time spent in user mode during the region
    double      sysTimeMs  = 0.0;  // CPU time spent in kernel mode during the region
    double      cpuTimeMs  = 0.0;  // userTimeMs + sysTimeMs
    long        peakRssKb  = 0;    // peak RSS observed BY THE END of the region
                                    // (NOTE: ru_maxrss is a high-water mark for the
                                    //  whole process, not scoped to this region alone.
                                    //  If you need a true delta, you'd need platform-specific
                                    //  current-RSS sampling instead — see note in .cpp)
};

inline ResourceSnapshot captureResourceSnapshot() {
    ResourceSnapshot snap;
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        snap.userTimeMs = usage.ru_utime.tv_sec * 1000.0 + usage.ru_utime.tv_usec / 1000.0;
        snap.sysTimeMs  = usage.ru_stime.tv_sec * 1000.0 + usage.ru_stime.tv_usec / 1000.0;
        // ru_maxrss is in KB on Linux, but in BYTES on macOS (BSD heritage quirk).
        // Normalize to KB here so downstream code is platform-agnostic.
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
    d.peakRssKb  = after.maxRssKb;  // high-water mark, not a true delta — see struct comment
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