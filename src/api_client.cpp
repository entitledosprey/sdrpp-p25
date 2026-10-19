#include "api_client.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <utils/flog.h>

using nlohmann::json;

void ApiClient::start(int port, std::vector<std::string> topics) {
    stop();
    this->port = port;
    this->topics = std::move(topics);
    running = true;
    thread = std::thread(&ApiClient::loop, this);
}

void ApiClient::stop() {
    running = false;
    int f = fd.exchange(-1);
    if (f >= 0) { shutdown(f, SHUT_RDWR); }
    if (thread.joinable()) { thread.join(); }
    if (f >= 0) { close(f); }
    connected = false;
}

bool ApiClient::sendLine(const json& obj) {
    std::lock_guard lck(sendMtx);
    int f = fd;
    if (f < 0) { return false; }
    std::string line = obj.dump() + "\n";
    const char* p = line.data();
    size_t n = line.size();
    while (n > 0) {
        ssize_t w = send(f, p, n, MSG_NOSIGNAL);
        if (w <= 0) { return false; }
        p += w;
        n -= w;
    }
    return true;
}

bool ApiClient::command(const std::string& cmd, const json& params) {
    if (!connected) { return false; }
    json req = { { "id", nextId++ }, { "cmd", cmd } };
    if (!params.is_null()) { req["params"] = params; }
    return sendLine(req);
}

void ApiClient::loop() {
    while (running) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(s, (sockaddr*)&addr, sizeof(addr)) < 0) {
            close(s);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        fd = s;
        std::string buf;
        bool subscribed = false;
        char chunk[16384];
        while (running) {
            pollfd pfd{ s, POLLIN, 0 };
            int pr = poll(&pfd, 1, 500);
            if (pr < 0) { break; }
            if (pr == 0) { continue; }
            ssize_t n = recv(s, chunk, sizeof(chunk), 0);
            if (n <= 0) { break; }
            buf.append(chunk, n);
            size_t pos;
            while ((pos = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, pos);
                buf.erase(0, pos + 1);
                if (line.empty() || line == "\r") { continue; }
                json msg = json::parse(line, nullptr, false);
                if (msg.is_discarded()) { continue; }
                if (!subscribed && msg.value("type", "") == "welcome") {
                    connected = true;
                    sendLine({ { "id", nextId++ }, { "cmd", "subscribe" }, { "topics", topics } });
                    sendLine({ { "id", nextId++ }, { "cmd", "get" }, { "what", "status" } });
                    subscribed = true;
                    flog::info("P25: connected to decoder API on port {}", port);
                }
                if (onMessage) { onMessage(msg); }
            }
        }
        connected = false;
        int expected = s;
        if (fd.compare_exchange_strong(expected, -1)) { close(s); }
        if (running) { std::this_thread::sleep_for(std::chrono::milliseconds(500)); }
    }
}
