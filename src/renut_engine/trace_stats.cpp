#include "renut_engine/trace_stats.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

namespace renut::trace_stats {

namespace {

std::vector<std::string> Split(std::string_view line) {
    std::vector<std::string> values;
    size_t start = 0;
    while (start <= line.size()) {
        const size_t end = line.find(',', start);
        values.emplace_back(line.substr(start, end == std::string_view::npos ? line.size() - start : end - start));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return values;
}

bool IsTrace(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    return name.starts_with("renut_trace_") && path.extension() == ".csv";
}

}

Snapshot GetLatest() {
    static std::chrono::steady_clock::time_point last_check;
    static Snapshot cached;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_check < std::chrono::milliseconds(500)) {
        return cached;
    }
    last_check = now;
    std::filesystem::path latest;
    std::filesystem::file_time_type latest_time{};
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::current_path(), error)) {
        if (error || !entry.is_regular_file(error) || !IsTrace(entry.path())) {
            continue;
        }
        const auto modified = entry.last_write_time(error);
        if (!error && (latest.empty() || modified > latest_time)) {
            latest = entry.path();
            latest_time = modified;
        }
    }
    if (latest.empty()) {
        cached = {};
        return cached;
    }
    std::ifstream input(latest);
    std::string header;
    std::string line;
    std::string latest_row;
    size_t column_count = 0;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (header.empty()) {
            header = line;
            column_count = Split(header).size();
        } else {
            if (Split(line).size() == column_count) {
                latest_row = line;
            }
        }
    }
    const auto keys = Split(header);
    const auto values = Split(latest_row);
    if (keys.empty() || keys.size() != values.size()) {
        return cached;
    }
    Snapshot snapshot;
    snapshot.available = true;
    snapshot.path = latest.filename().string();
    for (size_t index = 0; index < keys.size(); ++index) {
        try {
            snapshot.values.emplace(keys[index], std::stod(values[index]));
        } catch (...) {
        }
    }
    if (!snapshot.values.empty()) {
        cached = std::move(snapshot);
    }
    return cached;
}

}
