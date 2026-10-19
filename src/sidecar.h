#pragma once
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Runs the decoder as a child process in its own process group, captures its output, and
// restarts it with backoff if it exits unexpectedly. The child gets SIGTERM if SDR++ dies.
class Sidecar {
public:
    ~Sidecar() { stop(); }

    void start(const std::string& exe, const std::vector<std::string>& args,
               const std::vector<std::string>& env, const std::string& logPath);
    // SIGTERM to the process group, SIGKILL after graceMs.
    void stop(int graceMs = 3000);

    bool running() const { return pid > 0; }
    std::string state();
    std::vector<std::string> tail(size_t n);
    int restarts() const { return restartCount; }

private:
    void monitor();
    int spawn(int& outFd);

    std::string exe, logPath;
    std::vector<std::string> args, env;
    std::atomic<bool> wanted{ false };
    std::atomic<int> pid{ -1 };
    std::atomic<int> restartCount{ 0 };
    std::thread monitorThread;
    std::mutex logMtx;
    std::deque<std::string> lines;
    std::string lastState = "stopped";
};
