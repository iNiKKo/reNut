#pragma once

#include <string>
#include <unordered_map>

namespace renut::trace_stats {

struct Snapshot {
    bool available = false;
    std::string path;
    std::unordered_map<std::string, double> values;
};

Snapshot GetLatest();

}
