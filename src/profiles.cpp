#include "profiles.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <json.hpp>
#include <utils/flog.h>

namespace fs = std::filesystem;
using nlohmann::json;

std::vector<double> Profile::allFrequencies() const {
    std::vector<double> all = controlChannels;
    all.insert(all.end(), voiceFrequencies.begin(), voiceFrequencies.end());
    return all;
}

std::vector<Profile> loadProfiles(const std::string& dir) {
    std::vector<Profile> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) { return out; }
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() != ".json") { continue; }
        std::ifstream f(entry.path());
        json j = json::parse(f, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            flog::warn("P25: skipping unreadable profile {}", entry.path().string());
            continue;
        }
        Profile p;
        p.id = entry.path().stem().string();
        p.path = entry.path().string();
        p.name = j.value("name", p.id);
        p.system = j.value("system", "");
        p.modulation = j.value("modulation", "C4FM");
        p.groupCsv = j.value("group_csv", "");
        for (const auto& v : j.value("control_channels", json::array())) { p.controlChannels.push_back(v.get<double>()); }
        for (const auto& v : j.value("voice_frequencies", json::array())) { p.voiceFrequencies.push_back(v.get<double>()); }
        if (p.controlChannels.empty()) {
            flog::warn("P25: profile {} has no control channels", p.id);
            continue;
        }
        out.push_back(std::move(p));
    }
    std::sort(out.begin(), out.end(), [](const Profile& a, const Profile& b) { return a.name < b.name; });
    return out;
}

bool saveProfile(const Profile& p) {
    std::ifstream in(p.path);
    json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { j = json::object(); }
    in.close();
    j["name"] = p.name;
    j["system"] = p.system;
    j["modulation"] = p.modulation;
    j["group_csv"] = p.groupCsv;
    j["control_channels"] = p.controlChannels;
    j["voice_frequencies"] = p.voiceFrequencies;
    std::string tmp = p.path + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) { return false; }
        out << j.dump(2) << '\n';
    }
    std::error_code ec;
    fs::rename(tmp, p.path, ec);
    return !ec;
}

std::string writeChannelMap(const Profile& p, const std::string& dir) {
    fs::create_directories(dir);
    std::string path = dir + "/" + p.id + "_chan.csv";
    std::ofstream out(path);
    out << "ChannelNumber(dec),frequency(Hz),note\n";
    int n = 1;
    for (double f : p.controlChannels) { out << n++ << "," << (long long)f << ",cc candidate\n"; }
    return path;
}
