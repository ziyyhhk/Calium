#pragma once

#include <string>
#include <map>
#include <chrono>

using usize = size_t;
using u64 = uint64_t;

class BProfiler {
public:
    static void begin(std::string name);
    static void end(std::string name);
    static void reset();
    static std::string getResults();
    static void setUseAverages(bool use, usize divide = 0);
};
