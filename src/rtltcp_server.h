#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include "iq_ring.h"

// Minimal rtl_tcp server on loopback: sends the 12-byte "RTL0" header, streams cu8 IQ from
// an IqRing, and forwards the client's set-frequency (0x01) and set-sample-rate (0x02)
// commands. Gain, ppm, AGC, bias-tee and the other tuner commands are acknowledged by
// ignoring them: the real tuner belongs to SDR++.
class RtlTcpServer {
public:
    ~RtlTcpServer() { stop(); }

    bool start(int port, IqRing* ring);
    void stop();

    std::function<void(uint32_t)> onFrequency;
    std::function<void(uint32_t)> onSampleRate;

    std::atomic<bool> clientConnected{ false };
    std::atomic<uint64_t> bytesSent{ 0 };
    std::atomic<uint32_t> lastFrequency{ 0 };
    std::atomic<uint32_t> lastSampleRate{ 0 };
    std::atomic<uint32_t> ignoredCommands{ 0 };

private:
    void acceptLoop();
    void commandLoop(int fd);

    IqRing* ring = nullptr;
    int listenFd = -1;
    std::atomic<int> clientFd{ -1 };
    std::atomic<bool> running{ false };
    std::thread acceptThread;
};
