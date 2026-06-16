#pragma once

#include "types.h"
#include <vector>
#include <string>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <fstream>

inline TrialStats computeStats(
    const std::string&         label,
    const std::vector<double>& values)
{
    TrialStats s;
    s.label = label;
    s.n     = static_cast<int>(values.size());

    if (values.empty()) return s;

    s.mean = std::accumulate(values.begin(), values.end(), 0.0) / s.n;

    double var = 0.0;
    for (double v : values)
        var += (v - s.mean) * (v - s.mean);
    s.stdev = std::sqrt(var / s.n);

    double margin = 1.96 * s.stdev / std::sqrt(static_cast<double>(s.n));
    s.ci95Low  = s.mean - margin;
    s.ci95High = s.mean + margin;

    s.min = *std::min_element(values.begin(), values.end());
    s.max = *std::max_element(values.begin(), values.end());

    return s;
}

inline void printStats(const TrialStats& s) {
    std::cout << std::fixed << std::setprecision(4);
    std::cout << std::left  << std::setw(30) << s.label
              << std::right
              << std::setw(10) << s.mean
              << std::setw(10) << s.stdev
              << std::setw(14) << ("[" + std::to_string(s.ci95Low).substr(0,6)
                               + ", " + std::to_string(s.ci95High).substr(0,6) + "]")
              << std::setw(10) << s.min
              << std::setw(10) << s.max
              << "\n";
}

inline void printStatsHeader() {
    std::cout << std::left  << std::setw(30) << "Label"
              << std::right
              << std::setw(10) << "Mean"
              << std::setw(10) << "StDev"
              << std::setw(14) << "95% CI"
              << std::setw(10) << "Min"
              << std::setw(10) << "Max"
              << "\n";
    std::cout << std::string(74, '-') << "\n";
}

inline void saveStatsCSV(
    const std::vector<TrialStats>& stats,
    const std::string& filepath)
{
    std::ofstream f(filepath);
    f << "label,n,mean,stdev,ci95_low,ci95_high,min,max\n";
    for (const auto& s : stats) {
        f << s.label    << ","
          << s.n        << ","
          << s.mean     << ","
          << s.stdev    << ","
          << s.ci95Low  << ","
          << s.ci95High << ","
          << s.min      << ","
          << s.max      << "\n";
    }
}