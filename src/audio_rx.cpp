#include "audio_rx.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <utils/flog.h>

namespace {
constexpr int FRAME = 160;                     // 20 ms at 8 kHz
constexpr size_t MAX_FIFO = 2 * AudioRx::RATE;   // 1 s of stereo samples
// The decoder delivers Phase 2 voice in ~120 ms bursts with gaps up to ~125 ms between
// them, so start each call with more than one burst queued.
constexpr size_t PREBUFFER = 2 * 2000;           // 250 ms
constexpr auto CALL_END = std::chrono::milliseconds(400); // no packets this long: call over
}

bool AudioRx::start(int port, int channels) {
    if (running) { return true; }
    this->channels = channels == 1 ? 1 : 2;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        flog::error("P25: could not bind audio port 127.0.0.1:{}: {}", port, strerror(errno));
        close(fd);
        fd = -1;
        return false;
    }
    running = true;
    out.clearWriteStop();
    rxThread = std::thread(&AudioRx::rxLoop, this);
    pacerThread = std::thread(&AudioRx::pacerLoop, this);
    return true;
}

void AudioRx::stop() {
    if (!running) { return; }
    running = false;
    out.stopWriter();
    if (rxThread.joinable()) { rxThread.join(); }
    if (pacerThread.joinable()) { pacerThread.join(); }
    close(fd);
    fd = -1;
    std::lock_guard lck(fifoMtx);
    fifo.clear();
}

void AudioRx::rxLoop() {
    int16_t buf[4096];
    while (running) {
        pollfd pfd{ fd, POLLIN, 0 };
        if (poll(&pfd, 1, 200) <= 0) { continue; }
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) { continue; }
        packets++;
        lastPacket = std::chrono::steady_clock::now();
        size_t samples = n / sizeof(int16_t);
        std::lock_guard lck(fifoMtx);
        for (size_t i = 0; i < samples; i++) {
            fifo.push_back(buf[i]);
            if (channels == 1) { fifo.push_back(buf[i]); }
        }
        while (fifo.size() > MAX_FIFO) { fifo.pop_front(); }
    }
}

// No timer here: out.swap() blocks until the audio sink has taken the previous block, so the
// sound card's clock paces this loop. Keeping the next block ready at all times (silence
// when there's no voice) means the sink's callback never waits, which would cause dropouts.
void AudioRx::pacerLoop() {
    while (running) {
        float vol = muted ? 0.0f : volume.load();
        int m = mode;
        {
            std::lock_guard lck(fifoMtx);
            // A short shortfall mid-call is padded with silence; only a real pause ends the
            // call and makes the next one prebuffer again.
            auto quiet = std::chrono::steady_clock::now() - lastPacket.load();
            if (!playing && (fifo.size() >= PREBUFFER || (!fifo.empty() && quiet > std::chrono::milliseconds(150)))) {
                playing = true; // full prebuffer, or a short transmission that has already ended
            }
            if (playing && fifo.empty() && quiet > CALL_END) {
                playing = false;
            }
            if (playing && fifo.size() < 2 * FRAME) { underruns++; }
            for (int i = 0; i < FRAME; i++) {
                float l = 0, r = 0;
                if (playing && fifo.size() >= 2) {
                    l = fifo.front() / 32768.0f;
                    fifo.pop_front();
                    r = fifo.front() / 32768.0f;
                    fifo.pop_front();
                }
                switch (m) {
                case MONO_MIX: l = r = (l + r) * 0.5f; break;
                case SLOT1: r = l; break;
                case SLOT2: l = r; break;
                default: break;
                }
                out.writeBuf[i] = { l * vol, r * vol };
            }
        }
        if (!out.swap(FRAME)) { break; }
    }
}
