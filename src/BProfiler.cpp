#include "BProfiler.hpp"
#include "common.hpp"
#include <fmt/format.h>

static bool shouldUseAverages = false;
static usize divideNumber = 0;

static std::map<std::string, u64> currentTimes;
static std::map<std::string, u64> savedTimes;

static inline void addToMap(std::map<std::string, u64>& map, std::string name, u64 num) {
    auto it = map.find(name);
    if (it != map.end())
        it->second += num;
    else
        map[name] = num;
}

void BProfiler::begin(std::string name) {
    currentTimes[name] = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

void BProfiler::end(std::string name) {
    u64 end = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    u64 start = currentTimes[name];
    addToMap(savedTimes, name, end - start);
}

void BProfiler::reset() {
    savedTimes.clear();
    currentTimes.clear();
}

std::string BProfiler::getResults() {
    std::string result;
    for (auto& [name, time] : savedTimes) {
        if (shouldUseAverages && divideNumber > 0)
            result += fmt::format("{}: {:.2f}ms\n", name, (time / (double)divideNumber) / 1000000.0);
        else
            result += fmt::format("{}: {:.2f}ms\n", name, time / 1000000.0);
    }
    return result;
}

void BProfiler::setUseAverages(bool use, usize divide) {
    shouldUseAverages = use;
    divideNumber = divide;
}
