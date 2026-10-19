#pragma once
#include <string>
#include <vector>

// One trunked site: where to find its control channel, how it's modulated, and which
// talkgroup list applies. Profiles are JSON files in <config root>/p25_trunking/profiles/
// (written by scripts/import_sdrtrunk.py or by hand).
struct Profile {
    std::string id;   // file stem
    std::string path; // full path to the JSON file
    std::string name;
    std::string system;
    std::string modulation = "C4FM"; // or "CQPSK" (simulcast)
    std::vector<double> controlChannels;
    std::vector<double> voiceFrequencies; // learned from calls
    std::string groupCsv;                 // relative to the p25_trunking directory

    bool simulcast() const { return modulation == "CQPSK"; }
    std::vector<double> allFrequencies() const;
};

std::vector<Profile> loadProfiles(const std::string& dir);
bool saveProfile(const Profile& p);
// Write dsd-neo's channel map (used as the control-channel hunt list); returns its path.
std::string writeChannelMap(const Profile& p, const std::string& dir);
