#pragma once
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <dsp/stream.h>
#include <dsp/types.h>

// Receives the decoder's 8 kHz s16le UDP voice (stereo: left = slot 1, right = slot 2) and
// re-emits it as a continuous 8 kHz stereo stream for SDR++'s audio sinks, paced by the
// sink's own clock. Gaps between calls become silence, so the sink never starves; each call
// starts after a short prebuffer to absorb the decoder's bursty delivery.
class AudioRx {
public:
    enum Mode { STEREO = 0, MONO_MIX, SLOT1, SLOT2 };
    static constexpr int RATE = 8000;

    ~AudioRx() { stop(); }

    bool start(int port, int channels);
    void stop();

    dsp::stream<dsp::stereo_t> out;
    std::atomic<float> volume{ 1.0f };
    std::atomic<bool> muted{ false };
    std::atomic<int> mode{ STEREO };
    std::atomic<uint64_t> packets{ 0 };
    std::atomic<bool> playing{ false };
    std::atomic<uint64_t> underruns{ 0 };

private:
    void rxLoop();
    void pacerLoop();

    int fd = -1;
    int channels = 2;
    std::atomic<bool> running{ false };
    std::thread rxThread, pacerThread;
    std::mutex fifoMtx;
    std::deque<int16_t> fifo; // interleaved stereo
    std::atomic<std::chrono::steady_clock::time_point> lastPacket{};
};
