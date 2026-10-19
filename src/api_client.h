#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <json.hpp>

// Client for the decoder's JSON-Lines control/telemetry API on loopback. Reconnects until
// stopped, subscribes to the given topics on every connection, and hands each received
// object to onMessage (on the client thread).
class ApiClient {
public:
    ~ApiClient() { stop(); }

    void start(int port, std::vector<std::string> topics);
    void stop();

    // Send a command; returns false when not connected. An id is added automatically.
    bool command(const std::string& cmd, const nlohmann::json& params = nullptr);

    std::function<void(const nlohmann::json&)> onMessage;
    std::atomic<bool> connected{ false };

private:
    void loop();
    bool sendLine(const nlohmann::json& obj);

    int port = 0;
    std::vector<std::string> topics;
    std::atomic<bool> running{ false };
    std::atomic<int> fd{ -1 };
    std::atomic<int> nextId{ 1 };
    std::mutex sendMtx;
    std::thread thread;
};
