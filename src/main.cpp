#include <imgui.h>
#include <config.h>
#include <core.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <dsp/sink/handler_sink.h>
#include <dsp/multirate/rational_resampler.h>
#include <utils/flog.h>
#include <cmath>
#include <ctime>
#include <deque>
#include <filesystem>
#include <unistd.h>
#include "api_client.h"
#include "audio_rx.h"
#include "iq_ring.h"
#include "profiles.h"
#include "retune_policy.h"
#include "rtltcp_server.h"
#include "sidecar.h"

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "p25_trunking",
    /* Description:     */ "P25 trunking and digital voice via a dsd-neo decoder process",
    /* Author:          */ "entitledosprey",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

using nlohmann::json;
namespace fs = std::filesystem;

ConfigManager config;

namespace {
constexpr double DEFAULT_RATE = 1536000.0; // what the decoder requests with its default 48 kHz DSP bandwidth
constexpr double DC_GUARD_HZ = 60e3;
constexpr double USABLE_FRACTION = 0.42;   // usable half-window as a fraction of the SDR sample rate
constexpr double RING_SECONDS = 0.5;
constexpr double SETTLE_SECONDS = 0.04;    // mid-scale padding after a hardware retune
constexpr size_t MAX_CALLS = 200;

double num(const json& j, const char* key, double def = 0) {
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<double>() : def;
}
std::string str(const json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : "";
}
bool flag(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && ((it->is_boolean() && it->get<bool>()) || (it->is_number() && it->get<double>() != 0));
}
std::string mhz(double hz) {
    if (hz <= 0) { return "--"; }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.5f", hz / 1e6);
    return buf;
}
std::string timeStr(double unixTime) {
    time_t t = unixTime > 0 ? (time_t)unixTime : time(nullptr);
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%S", localtime(&t));
    return buf;
}
std::string findDecoder(const std::string& configured) {
    std::vector<std::string> candidates;
    if (!configured.empty()) { candidates.push_back(configured); }
    const char* home = getenv("HOME");
    if (home) { candidates.push_back(std::string(home) + "/.local/bin/dsd-neo"); }
    candidates.push_back("/usr/local/bin/dsd-neo");
    candidates.push_back("/usr/bin/dsd-neo");
    for (const auto& c : candidates) {
        if (access(c.c_str(), X_OK) == 0) { return c; }
    }
    return "";
}
}

struct SlotView {
    int state = 0; // 1 idle, 2 active, 3 ended
    long long tg = 0;
    std::string name, src;
    bool enc = false, emergency = false;
    double freq = 0;
    long long elapsedMs = 0;
};

struct CallRow {
    std::string time, tgName, srcName, text;
    long long tg = 0, src = 0;
    double freq = 0;
    bool enc = false;
};

class P25TrunkingModule : public ModuleManager::Instance {
public:
    P25TrunkingModule(std::string name) : name(name) {
        config.acquire();
        json& c = config.conf[name];
        if (!c.is_object()) { c = json::object(); }
        auto def = [&](const char* k, json v) { if (!c.contains(k)) { c[k] = v; } };
        def("decoder", "");
        def("profile", "");
        def("rtlTcpPort", 14423);
        def("apiPort", 9911);
        def("audioPort", 23456);
        def("audioChannels", 2);
        def("iqGain", 1.0);
        def("volume", 1.0);
        def("audioMode", 0);
        def("dspBandwidthKHz", 48);
        def("extraArgs", "");
        def("launchDecoder", true);
        decoderPath = c["decoder"];
        launchDecoder = c["launchDecoder"];
        profileId = c["profile"];
        iqGain = c["iqGain"].get<float>();
        audio.volume = (float)c["volume"].get<double>();
        audio.mode = c["audioMode"].get<int>();
        config.release(true);
        snprintf(decoderBuf, sizeof(decoderBuf), "%s", decoderPath.c_str());

        p25Dir = core::args["root"].s() + "/p25_trunking";
        fs::create_directories(p25Dir + "/profiles");
        reloadProfiles();

        resamp.init(&audio.out, AudioRx::RATE, audioSampRate);
        srChangeHandler.ctx = this;
        srChangeHandler.handler = sampleRateChangeHandler;
        stream.init(&resamp.out, &srChangeHandler, audioSampRate);
        sigpath::sinkManager.registerStream(name, &stream);
        stream.start();

        retuneHandler.ctx = this;
        retuneHandler.handler = onSourceRetune;
        sigpath::sourceManager.onRetune.bindHandler(&retuneHandler);

        server.onFrequency = [this](uint32_t f) { tuneTo((double)f); };
        server.onSampleRate = [this](uint32_t r) { setRate((double)r); };
        api.onMessage = [this](const json& m) { handleTelemetry(m); };

        maintRunning = true;
        maintThread = std::thread(&P25TrunkingModule::maintenance, this);

        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~P25TrunkingModule() {
        gui::menu.removeEntry(name);
        stopDecoding(false);
        maintRunning = false;
        if (maintThread.joinable()) { maintThread.join(); }
        sigpath::sourceManager.onRetune.unbindHandler(&retuneHandler);
        stream.stop();
        sigpath::sinkManager.unregisterStream(name);
    }

    void postInit() {
        // New streams start with no sink; route the decoded voice to the speakers once, then
        // leave it to the user's choice in the Sinks menu.
        config.acquire();
        bool done = config.conf[name].value("sinkDefaulted", false);
        config.conf[name]["sinkDefaulted"] = true;
        config.release(!done);
        if (!done) { sigpath::sinkManager.setStreamSink(name, "Audio"); }

        // Resume if the decoder was running when SDR++ last closed.
        config.acquire();
        bool resume = config.conf[name].value("wasRunning", false);
        config.release();
        if (resume && enabled) { startDecoding(); }
    }
    void enable() { enabled = true; }
    void disable() {
        stopDecoding(false);
        enabled = false;
    }
    bool isEnabled() { return enabled; }

private:
    // ------------------------------------------------------------------ lifecycle

    const Profile* currentProfile() {
        for (const auto& p : profiles) {
            if (p.id == profileId) { return &p; }
        }
        return profiles.empty() ? nullptr : &profiles[0];
    }

    void reloadProfiles() {
        std::lock_guard lck(profileMtx);
        profiles = loadProfiles(p25Dir + "/profiles");
    }

    std::vector<std::string> decoderArgs(const Profile& p, int rtlPort, int apiPort, int audioPort, int bwKHz) {
        std::vector<std::string> a = { "-ft" };
        if (p.simulcast()) { a.push_back("-mq"); }
        a.insert(a.end(), { "-^", "-T", "--enc-lockout", "--tg-lockout-session", "--frontend", "none" });
        a.push_back("-i");
        a.push_back("rtltcp:127.0.0.1:" + std::to_string(rtlPort) + ":" + mhz(p.controlChannels[0]) + "M:0:0:" +
                    std::to_string(bwKHz) + ":0:2");
        a.push_back("-C");
        a.push_back(writeChannelMap(p, p25Dir + "/run"));
        if (!p.groupCsv.empty() && fs::exists(p25Dir + "/" + p.groupCsv)) {
            a.push_back("-G");
            a.push_back(p25Dir + "/" + p.groupCsv);
        }
        a.push_back("-o");
        a.push_back("udp:127.0.0.1:" + std::to_string(audioPort));
        a.push_back("--api");
        a.push_back(std::to_string(apiPort));
        a.push_back("-J");
        a.push_back(p25Dir + "/run/" + p.id + "_events.log");
        return a;
    }

    void startDecoding() {
        {
            std::lock_guard lck(profileMtx);
            const Profile* p = currentProfile();
            if (!p) {
                statusText = "No profiles: import an SDRTrunk playlist or add one (see README).";
                return;
            }
            profileId = p->id;
            active = *p;
        }
        std::string exe = findDecoder(decoderPath);
        if (exe.empty() && launchDecoder) {
            statusText = "dsd-neo not found: set its path below.";
            return;
        }

        config.acquire();
        json c = config.conf[name];
        config.release();
        int rtlPort = c["rtlTcpPort"], apiPort = c["apiPort"], audioPort = c["audioPort"];
        std::vector<std::string> args = decoderArgs(active, rtlPort, apiPort, audioPort, c["dspBandwidthKHz"]);
        std::string extra = c["extraArgs"];
        for (size_t pos = 0; pos < extra.size();) {
            size_t end = extra.find(' ', pos);
            if (end == std::string::npos) { end = extra.size(); }
            if (end > pos) { args.push_back(extra.substr(pos, end - pos)); }
            pos = end + 1;
        }

        {
            std::lock_guard tl(telemMtx);
            telemetry = Telemetry{};
        }
        targetFreq = 0;
        retunes = 0;
        vfoRate = DEFAULT_RATE;
        ring.reset((size_t)(2 * vfoRate * RING_SECONDS));
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, DC_GUARD_HZ * 2, vfoRate, vfoRate,
                                            vfoRate, vfoRate, true);
        iqSink.init(vfo->output, iqHandler, this);
        iqSink.start();

        running = true;
        if (!server.start(rtlPort, &ring)) {
            stopDecoding();
            statusText = "Could not open the rtl_tcp port " + std::to_string(rtlPort);
            return;
        }
        audio.start(audioPort, c["audioChannels"]);
        resamp.start();
        api.start(apiPort, { "call", "event", "system", "status", "metrics" });

        std::string cmdline = "DSD_NEO_NO_BOOTSTRAP=1 DSD_NEO_DISABLE_FS4_SHIFT=1 DSD_NEO_TCP_PREBUF_MS=100 " + (exe.empty() ? "dsd-neo" : exe);
        for (const auto& s : args) { cmdline += " " + s; }
        flog::info("P25: {}", cmdline);
        manualCommand = launchDecoder ? "" : cmdline;
        saveSetting("wasRunning", true);
        if (launchDecoder) {
            // The rtl_tcp link is local, so the decoder's 1 s network prebuffer only delays calls.
            std::vector<std::string> env = { "DSD_NEO_NO_BOOTSTRAP=1", "DSD_NEO_DISABLE_FS4_SHIFT=1", "DSD_NEO_TCP_PREBUF_MS=100" };
            sidecar.start(exe, args, env, p25Dir + "/run/decoder.log");
        }
        statusText = "";
    }

    // userStop: remember that the user stopped it, so it won't resume at the next launch.
    void stopDecoding(bool userStop = true) {
        if (!running) { return; }
        running = false;
        if (userStop) { saveSetting("wasRunning", false); }
        api.command("quit");
        sidecar.stop();
        api.stop();
        server.stop();
        audio.stop();
        resamp.stop();
        iqSink.stop();
        if (vfo) {
            sigpath::vfoManager.deleteVFO(vfo);
            vfo = nullptr;
        }
        std::lock_guard lck(profileMtx);
        for (auto& p : profiles) {
            if (p.id == active.id) { p.voiceFrequencies = active.voiceFrequencies; }
        }
        if (profileDirty) {
            saveProfile(active);
            profileDirty = false;
        }
    }

    // ------------------------------------------------------------------ IQ and tuning

    static void iqHandler(dsp::complex_t* data, int count, void* ctx) {
        P25TrunkingModule* _this = (P25TrunkingModule*)ctx;
        _this->ring.write(data, count, _this->iqGain);
    }

    void setRate(double rate) {
        if (!vfo || rate == vfoRate) { return; }
        double max = sigpath::iqFrontEnd.getEffectiveSamplerate();
        if (rate > max) {
            flog::warn("P25: decoder asked for {} S/s but the SDR only provides {}", rate, max);
            return;
        }
        vfoRate = rate;
        vfo->setBandwidthLimits(rate, rate, true);
        vfo->setSampleRate(rate, rate);
        ring.reset((size_t)(2 * rate * RING_SECONDS));
        flog::info("P25: decoder sample rate {}", rate);
    }

    // Called from the rtl_tcp command thread whenever the decoder retunes, and from the
    // maintenance thread after someone else moved the SDR.
    void tuneTo(double f) {
        std::lock_guard lck(tuneMtx);
        if (!vfo) { return; }
        targetFreq = f;
        double center = gui::waterfall.getCenterFrequency();
        double fs = sigpath::iqFrontEnd.getEffectiveSamplerate();
        double cc;
        std::vector<double> known;
        {
            std::lock_guard tl(telemMtx);
            cc = telemetry.cc;
        }
        {
            std::lock_guard pl(profileMtx);
            if (cc <= 0 && !active.controlChannels.empty()) { cc = active.controlChannels[0]; }
            known = active.allFrequencies();
        }
        p25::RetunePlan plan = p25::planRetune(f, center, USABLE_FRACTION * fs, DC_GUARD_HZ, cc, known);
        if (plan.retune) {
            ownRetune = true;
            gui::waterfall.setCenterFrequency(plan.center);
            sigpath::sourceManager.tune(plan.center);
            ownRetune = false;
            center = plan.center;
            retunes++;
            lastRetune = (double)time(nullptr);
            ring.flush((size_t)(2 * vfoRate * SETTLE_SECONDS));
        }
        else {
            ring.flush(0);
        }
        sigpath::vfoManager.setOffset(name, f - center);
        externalRetune = false;
    }

    static void onSourceRetune(double freq, void* ctx) {
        P25TrunkingModule* _this = (P25TrunkingModule*)ctx;
        if (_this->running && !_this->ownRetune) { _this->externalRetune = true; }
    }

    static void sampleRateChangeHandler(float sampleRate, void* ctx) {
        P25TrunkingModule* _this = (P25TrunkingModule*)ctx;
        _this->audioSampRate = sampleRate;
        _this->resamp.tempStop();
        _this->resamp.setOutSamplerate(sampleRate);
        _this->resamp.tempStart();
    }

    void maintenance() {
        int ticks = 0;
        while (maintRunning) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            // Only defend the tuning while a decoder is actually using it.
            if (running && externalRetune && targetFreq > 0 && server.clientConnected) {
                flog::info("P25: SDR was retuned elsewhere; putting the decoder back on {}", targetFreq.load());
                tuneTo(targetFreq);
            }
            if (++ticks % 40 == 0 && profileDirty) {
                std::lock_guard lck(profileMtx);
                saveProfile(active);
                profileDirty = false;
            }
        }
    }

    // ------------------------------------------------------------------ telemetry

    struct Telemetry {
        std::string protocol, sysid, message;
        double cc = 0, vc = 0;
        bool trunking = false;
        int nac = -1, neighbors = 0;
        SlotView slots[2];
        std::deque<CallRow> calls;
        json tgPolicy;
        double snr = NAN, cfo = NAN;
        bool carrierLock = false;
    };

    void learnVoiceFrequency(double f) {
        std::lock_guard lck(profileMtx);
        for (double k : active.allFrequencies()) {
            if (std::fabs(k - f) < 1000) { return; }
        }
        active.voiceFrequencies.push_back(f);
        profileDirty = true;
        flog::info("P25: learned voice frequency {} MHz", mhz(f));
    }

    void handleTelemetry(const json& m) {
        std::vector<double> heard;
        updateTelemetry(m, heard);
        for (double f : heard) { learnVoiceFrequency(f); }
    }

    void updateTelemetry(const json& m, std::vector<double>& heard) {
        std::string type = str(m, "type");
        std::lock_guard lck(telemMtx);
        Telemetry& t = telemetry;
        if (type == "call") {
            for (auto& s : t.slots) { s = SlotView{}; }
            for (const auto& s : m.value("slots", json::array())) {
                int i = (int)num(s, "slot");
                if (i < 0 || i > 1) { continue; }
                SlotView& v = t.slots[i];
                v.state = (int)num(s, "state");
                v.tg = (long long)num(s, "tg_id");
                v.name = str(s, "name");
                v.src = str(s, "src_text");
                v.enc = flag(s, "enc");
                v.emergency = flag(s, "emergency");
                v.freq = num(s, "frequency_hz");
                v.elapsedMs = (long long)num(s, "elapsed_ms");
                if (v.state == 2 && v.freq > 0 && v.freq != t.cc) { heard.push_back(v.freq); }
            }
        }
        else if (type == "system" || type == "status") {
            if (m.contains("protocol") && m["protocol"].is_string()) { t.protocol = m["protocol"]; }
            t.cc = num(m, "cc_freq_hz", t.cc);
            t.vc = num(m, "vc_freq_hz", t.vc);
            if (m.contains("trunking")) { t.trunking = flag(m, "trunking"); }
            if (m.contains("p25_neighbors") && m["p25_neighbors"].is_array()) { t.neighbors = m["p25_neighbors"].size(); }
            if (m.contains("tg_policy")) { t.tgPolicy = m["tg_policy"]; }
            if (m.contains("message")) {
                t.message = m["message"].is_object() ? str(m["message"], "text") : "";
            }
        }
        else if (type == "metrics") {
            t.snr = num(m, "snr_db", NAN);
            t.cfo = num(m, "cfo_hz", NAN);
            t.carrierLock = flag(m, "carrier_lock");
        }
        else if (type == "event") {
            if (!str(m, "sysid_string").empty()) { t.sysid = str(m, "sysid_string"); }
            if ((int)num(m, "access_code_kind") == 2) { t.nac = (int)num(m, "access_code"); }
            bool voice = (int)num(m, "category") == 2;
            if ((voice || flag(m, "enc")) && !flag(m, "update")) {
                CallRow r;
                r.time = timeStr(num(m, "event_start_time", num(m, "event_time")));
                r.tg = (long long)num(m, "target_id");
                r.tgName = str(m, "t_name");
                r.src = (long long)num(m, "source_id");
                r.srcName = str(m, "s_name");
                r.freq = num(m, "freq_hz");
                r.enc = flag(m, "enc");
                r.text = str(m, "event");
                t.calls.push_front(r);
                if (t.calls.size() > MAX_CALLS) { t.calls.pop_back(); }
            }
        }
    }

    // ------------------------------------------------------------------ UI

    static void menuHandler(void* ctx) {
        P25TrunkingModule* _this = (P25TrunkingModule*)ctx;
        float width = ImGui::GetContentRegionAvail().x;
        if (!_this->enabled) { style::beginDisabled(); }

        // Profile and start/stop
        {
            std::lock_guard lck(_this->profileMtx);
            const Profile* cur = _this->currentProfile();
            if (_this->running) { style::beginDisabled(); }
            ImGui::SetNextItemWidth(width - ImGui::CalcTextSize("Reload").x - 16);
            if (ImGui::BeginCombo(CONCAT("##p25_profile_", _this->name), cur ? cur->name.c_str() : "(no profiles)")) {
                for (const auto& p : _this->profiles) {
                    bool sel = cur && p.id == cur->id;
                    if (ImGui::Selectable(p.name.c_str(), sel)) {
                        _this->profileId = p.id;
                        config.acquire();
                        config.conf[_this->name]["profile"] = p.id;
                        config.release(true);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            bool reload = ImGui::Button(CONCAT("Reload##p25_reload_", _this->name));
            if (_this->running) { style::endDisabled(); }
            if (cur) {
                ImGui::TextDisabled("%s, %zu control channel(s), %zu learned voice", cur->simulcast() ? "Simulcast (CQPSK)" : "C4FM",
                                    cur->controlChannels.size(), cur->voiceFrequencies.size());
            }
            _this->reloadRequested = reload;
        }
        if (_this->reloadRequested) {
            _this->reloadRequested = false;
            _this->reloadProfiles();
        }
        if (!_this->running) {
            if (ImGui::Button(CONCAT("Start##p25_start_", _this->name), ImVec2(width, 0))) { _this->startDecoding(); }
        }
        else if (ImGui::Button(CONCAT("Stop##p25_stop_", _this->name), ImVec2(width, 0))) {
            _this->stopDecoding();
        }
        if (!_this->statusText.empty()) { ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "%s", _this->statusText.c_str()); }

        if (_this->running) { _this->drawRunning(width); }

        if (ImGui::CollapsingHeader(CONCAT("Settings##p25_settings_", _this->name))) { _this->drawSettings(width); }
        if (ImGui::CollapsingHeader(CONCAT("Decoder log##p25_log_", _this->name))) {
            ImGui::BeginChild(CONCAT("##p25_logchild_", _this->name), ImVec2(width, 200), true, ImGuiWindowFlags_HorizontalScrollbar);
            for (const auto& l : _this->sidecar.tail(200)) { ImGui::TextUnformatted(l.c_str()); }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) { ImGui::SetScrollHereY(1.0f); }
            ImGui::EndChild();
        }

        if (!_this->enabled) { style::endDisabled(); }
    }

    void drawRunning(float width) {
        Telemetry t;
        {
            std::lock_guard lck(telemMtx);
            t = telemetry;
        }
        if (launchDecoder) { ImGui::Text("Decoder: %s%s", sidecar.state().c_str(), api.connected ? "" : " (API connecting)"); }
        else {
            ImGui::TextWrapped("Decoder not launched. Run it yourself:");
            ImGui::InputText(CONCAT("##p25_manual_", name), (char*)manualCommand.c_str(), manualCommand.size() + 1, ImGuiInputTextFlags_ReadOnly);
        }
        ImGui::Text("IQ link: %s, %.3f MS/s, buffer %.0f%%, overruns %llu", server.clientConnected ? "connected" : "waiting",
                    vfoRate / 1e6, ring.fillFraction() * 100, (unsigned long long)ring.overruns.load());
        ImGui::Text("IQ level %.1f dBFS%s, SDR retunes %d%s%s", ring.levelDb.load(),
                    ring.clipFraction > 0.001f ? " (clipping)" : "", retunes.load(),
                    lastRetune > 0 ? ", last " : "", lastRetune > 0 ? timeStr(lastRetune).c_str() : "");
        ImGui::Separator();
        if (t.nac >= 0) { ImGui::Text("%s  %s  NAC 0x%03X", t.protocol.empty() ? "--" : t.protocol.c_str(), t.sysid.c_str(), t.nac); }
        else { ImGui::Text("%s  %s  NAC --", t.protocol.empty() ? "--" : t.protocol.c_str(), t.sysid.c_str()); }
        ImGui::Text("CC %s MHz   VC %s MHz   neighbours %d", mhz(t.cc).c_str(), mhz(t.vc).c_str(), t.neighbors);
        if (!std::isnan(t.snr)) { ImGui::Text("SNR %.1f dB, CFO %.0f Hz%s", t.snr, std::isnan(t.cfo) ? 0.0 : t.cfo, t.carrierLock ? ", locked" : ""); }
        if (!t.message.empty()) { ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", t.message.c_str()); }

        if (ImGui::BeginTable(CONCAT("##p25_slots_", name), 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Talkgroup");
            ImGui::TableSetupColumn("Source");
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableHeadersRow();
            for (int i = 0; i < 2; i++) {
                const SlotView& s = t.slots[i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", i + 1);
                ImGui::TableSetColumnIndex(1);
                if (s.state == 2) {
                    ImVec4 col = s.enc ? ImVec4(1, 0.35f, 0.35f, 1) : ImVec4(0.3f, 1, 0.3f, 1);
                    ImGui::TextColored(col, "%s%s (%lld)%s", s.emergency ? "EMERGENCY " : "",
                                       s.name.empty() ? "TG" : s.name.c_str(), s.tg, s.enc ? " ENC" : "");
                }
                else {
                    ImGui::TextDisabled("idle");
                }
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(s.state == 2 ? s.src.c_str() : "");
                ImGui::TableSetColumnIndex(3);
                if (s.state == 2) {
                    if (ImGui::SmallButton(CONCAT("Hold##p25_hold", std::to_string(i)))) { api.command("tg_hold_set", { { "value", s.tg } }); }
                    ImGui::SameLine();
                    if (ImGui::SmallButton(CONCAT("Lock out##p25_lock", std::to_string(i)))) { api.command("lockout_slot", { { "value", i } }); }
                    ImGui::SameLine();
                    if (ImGui::SmallButton(CONCAT("Skip##p25_skip", std::to_string(i)))) { api.command("skip_slot", { { "value", i } }); }
                }
            }
            ImGui::EndTable();
        }
        if (ImGui::Button(CONCAT("Release hold##p25_unhold_", name))) { api.command("tg_hold_set", { { "value", 0 } }); }
        ImGui::SameLine();
        if (ImGui::Button(CONCAT("Back to control channel##p25_cc_", name))) { api.command("return_cc"); }

        // Audio
        float vol = audio.volume;
        ImGui::SetNextItemWidth(width * 0.5f);
        if (ImGui::SliderFloat(CONCAT("Volume##p25_vol_", name), &vol, 0.0f, 2.0f, "%.2f")) {
            audio.volume = vol;
            saveSetting("volume", vol);
        }
        ImGui::SameLine();
        bool muted = audio.muted;
        if (ImGui::Checkbox(CONCAT("Mute##p25_mute_", name), &muted)) { audio.muted = muted; }
        int mode = audio.mode;
        ImGui::SetNextItemWidth(width * 0.5f);
        if (ImGui::Combo(CONCAT("Audio##p25_amode_", name), &mode, "Slots L/R\0Mono mix\0Slot 1 only\0Slot 2 only\0")) {
            audio.mode = mode;
            saveSetting("audioMode", mode);
        }

        // Recent calls
        ImGui::Text("Recent calls (%zu)%s   audio underruns %llu", t.calls.size(), audio.playing ? "   [audio]" : "",
                    (unsigned long long)audio.underruns.load());
        if (ImGui::BeginTable(CONCAT("##p25_calls_", name), 5,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                              ImVec2(width, 220))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Talkgroup");
            ImGui::TableSetupColumn("Source");
            ImGui::TableSetupColumn("MHz", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableHeadersRow();
            int row = 0;
            for (const auto& c : t.calls) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(c.time.c_str());
                ImGui::TableSetColumnIndex(1);
                if (c.enc) { ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "%s (%lld) ENC, skipped", c.tgName.c_str(), c.tg); }
                else { ImGui::Text("%s (%lld)", c.tgName.c_str(), c.tg); }
                ImGui::TableSetColumnIndex(2);
                if (c.srcName.empty()) { ImGui::Text("%lld", c.src); }
                else { ImGui::Text("%s (%lld)", c.srcName.c_str(), c.src); }
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(mhz(c.freq).c_str());
                ImGui::TableSetColumnIndex(4);
                std::string id = std::to_string(row++);
                if (c.tg > 0 && ImGui::SmallButton(CONCAT("Mute TG##p25_tgoff", id))) {
                    api.command("tg_listen_set", { { "id_start", c.tg }, { "id_end", c.tg }, { "listen", false } });
                }
            }
            ImGui::EndTable();
        }
    }

    void drawSettings(float width) {
        if (running) { style::beginDisabled(); }
        ImGui::TextUnformatted("dsd-neo path");
        ImGui::SetNextItemWidth(width);
        if (ImGui::InputText(CONCAT("##p25_decoder_", name), decoderBuf, sizeof(decoderBuf))) {
            decoderPath = decoderBuf;
            saveSetting("decoder", decoderPath);
        }
        std::string found = findDecoder(decoderPath);
        ImGui::TextDisabled("%s", found.empty() ? "not found" : ("using " + found).c_str());
        if (ImGui::Checkbox(CONCAT("Launch dsd-neo##p25_launch_", name), &launchDecoder)) { saveSetting("launchDecoder", launchDecoder); }
        if (running) { style::endDisabled(); }

        float gain = iqGain;
        ImGui::SetNextItemWidth(width * 0.5f);
        if (ImGui::SliderFloat(CONCAT("IQ gain##p25_iqgain_", name), &gain, 0.1f, 8.0f, "%.2f")) {
            iqGain = gain;
            saveSetting("iqGain", gain);
        }
        ImGui::TextWrapped("Profiles live in %s/profiles. Import an SDRTrunk playlist with "
                           "scripts/import_sdrtrunk.py, then press Reload.", p25Dir.c_str());
    }

    template <class T>
    void saveSetting(const char* key, T value) {
        config.acquire();
        config.conf[name][key] = value;
        config.release(true);
    }

    std::string name;
    std::atomic<bool> enabled{ true };
    std::atomic<bool> running{ false };
    std::string p25Dir, decoderPath, profileId, statusText, manualCommand;
    bool launchDecoder = true;
    char decoderBuf[1024] = {};

    std::mutex profileMtx;
    std::vector<Profile> profiles;
    Profile active;
    std::atomic<bool> profileDirty{ false };
    bool reloadRequested = false;

    VFOManager::VFO* vfo = nullptr;
    dsp::sink::Handler<dsp::complex_t> iqSink;
    IqRing ring;
    std::atomic<float> iqGain{ 1.0f };
    double vfoRate = DEFAULT_RATE;

    std::mutex tuneMtx;
    std::atomic<double> targetFreq{ 0 };
    std::atomic<bool> ownRetune{ false }, externalRetune{ false };
    std::atomic<int> retunes{ 0 };
    std::atomic<double> lastRetune{ 0 };
    EventHandler<double> retuneHandler;

    RtlTcpServer server;
    Sidecar sidecar;
    ApiClient api;
    AudioRx audio;
    dsp::multirate::RationalResampler<dsp::stereo_t> resamp;
    double audioSampRate = 48000;
    EventHandler<float> srChangeHandler;
    SinkManager::Stream stream;

    std::mutex telemMtx;
    Telemetry telemetry;

    std::thread maintThread;
    std::atomic<bool> maintRunning{ false };
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/p25_trunking_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new P25TrunkingModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (P25TrunkingModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
