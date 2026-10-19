#pragma once
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>
#include <dsp/types.h>

// Bounded byte ring between SDR++'s DSP thread (producer) and the rtl_tcp sender (consumer).
// Samples are stored as rtl_tcp cu8 (I, Q interleaved, 127.5 = zero). When the consumer
// falls behind, the oldest bytes are dropped and counted as overruns.
class IqRing {
public:
    void reset(size_t capacityBytes) {
        std::lock_guard lck(mtx);
        buf.assign(capacityBytes, 127);
        head = count = 0;
        overruns = 0;
    }

    void write(const dsp::complex_t* in, int n, float gain) {
        tmp.resize(2 * n);
        float sumsq = 0;
        int clipped = 0;
        for (int i = 0; i < n; i++) {
            float re = in[i].re * gain, im = in[i].im * gain;
            sumsq += re * re + im * im;
            tmp[2 * i] = toU8(re, clipped);
            tmp[2 * i + 1] = toU8(im, clipped);
        }
        if (n > 0) {
            levelDb = 10.0f * std::log10(sumsq / n + 1e-12f);
            clipFraction = (float)clipped / (2.0f * n);
        }
        {
            std::lock_guard lck(mtx);
            push(tmp.data(), tmp.size());
        }
        cv.notify_one();
    }

    // Copy up to max bytes; waits up to timeoutMs for data. Returns bytes copied.
    size_t read(uint8_t* out, size_t max, int timeoutMs) {
        std::unique_lock lck(mtx);
        cv.wait_for(lck, std::chrono::milliseconds(timeoutMs), [this] { return count > 0 || stopped; });
        size_t n = std::min(max, count);
        size_t cap = buf.size();
        size_t tail = (head + cap - count) % cap;
        size_t first = std::min(n, cap - tail);
        memcpy(out, &buf[tail], first);
        memcpy(out + first, &buf[0], n - first);
        count -= n;
        return n;
    }

    // Drop queued samples (they belong to the old tuning) and optionally queue midBytes of
    // mid-scale padding so the consumer's sample clock keeps running while the tuner settles.
    void flush(size_t midBytes) {
        std::lock_guard lck(mtx);
        head = count = 0;
        std::vector<uint8_t> pad(midBytes, 127);
        for (size_t i = 1; i < midBytes; i += 2) { pad[i] = 128; }
        push(pad.data(), pad.size());
    }

    void wake() {
        { std::lock_guard lck(mtx); stopped = true; }
        cv.notify_all();
    }
    void unwake() { std::lock_guard lck(mtx); stopped = false; }

    float fillFraction() {
        std::lock_guard lck(mtx);
        return buf.empty() ? 0.0f : (float)count / buf.size();
    }

    std::atomic<uint64_t> overruns{ 0 };
    std::atomic<float> levelDb{ -100.0f };
    std::atomic<float> clipFraction{ 0.0f };

private:
    static uint8_t toU8(float v, int& clipped) {
        float s = v * 127.5f + 127.5f;
        if (s < 0.0f) { clipped++; return 0; }
        if (s > 255.0f) { clipped++; return 255; }
        return (uint8_t)lrintf(s);
    }

    void push(const uint8_t* p, size_t n) {
        size_t cap = buf.size();
        if (cap == 0) { return; }
        if (n > cap) { p += n - cap; n = cap; }
        if (count + n > cap) {
            overruns += count + n - cap;
            count = cap - n;
        }
        size_t first = std::min(n, cap - head);
        memcpy(&buf[head], p, first);
        memcpy(&buf[0], p + first, n - first);
        head = (head + n) % cap;
        count += n;
    }

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<uint8_t> buf;
    std::vector<uint8_t> tmp;
    size_t head = 0, count = 0;
    bool stopped = false;
};
