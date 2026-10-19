#include "rtltcp_server.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <vector>
#include <utils/flog.h>

namespace {
constexpr uint32_t TUNER_E4000 = 1;
constexpr size_t SEND_CHUNK = 64 * 1024;

bool sendAll(int fd, const uint8_t* p, size_t n) {
    while (n > 0) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) { return false; }
        p += w;
        n -= w;
    }
    return true;
}

bool recvAll(int fd, uint8_t* p, size_t n) {
    while (n > 0) {
        ssize_t r = recv(fd, p, n, 0);
        if (r <= 0) { return false; }
        p += r;
        n -= r;
    }
    return true;
}
}

bool RtlTcpServer::start(int port, IqRing* ring) {
    if (running) { return true; }
    this->ring = ring;
    listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) { return false; }
    int one = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listenFd, (sockaddr*)&addr, sizeof(addr)) < 0 || listen(listenFd, 1) < 0) {
        flog::error("P25: rtl_tcp server could not listen on 127.0.0.1:{}: {}", port, strerror(errno));
        close(listenFd);
        listenFd = -1;
        return false;
    }
    running = true;
    ring->unwake();
    acceptThread = std::thread(&RtlTcpServer::acceptLoop, this);
    flog::info("P25: rtl_tcp server listening on 127.0.0.1:{}", port);
    return true;
}

void RtlTcpServer::stop() {
    if (!running) { return; }
    running = false;
    ring->wake();
    int fd = clientFd.exchange(-1);
    if (fd >= 0) { shutdown(fd, SHUT_RDWR); }
    if (acceptThread.joinable()) { acceptThread.join(); }
    close(listenFd);
    listenFd = -1;
}

void RtlTcpServer::acceptLoop() {
    std::vector<uint8_t> chunk(SEND_CHUNK);
    while (running) {
        pollfd pfd{ listenFd, POLLIN, 0 };
        if (poll(&pfd, 1, 200) <= 0) { continue; }
        int fd = accept(listenFd, nullptr, nullptr);
        if (fd < 0) { continue; }
        int one = 1, sndbuf = 1 << 20;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

        uint8_t header[12] = { 'R', 'T', 'L', '0' };
        uint32_t tuner = htonl(TUNER_E4000), gains = htonl(0);
        memcpy(header + 4, &tuner, 4);
        memcpy(header + 8, &gains, 4);
        if (!sendAll(fd, header, sizeof(header))) {
            close(fd);
            continue;
        }
        clientFd = fd;
        clientConnected = true;
        flog::info("P25: decoder connected to rtl_tcp server");
        ring->flush(0);
        std::thread cmds(&RtlTcpServer::commandLoop, this, fd);

        while (running && clientFd == fd) {
            size_t n = ring->read(chunk.data(), chunk.size(), 200);
            if (n == 0) { continue; }
            if (!sendAll(fd, chunk.data(), n)) { break; }
            bytesSent += n;
        }

        int expected = fd;
        clientFd.compare_exchange_strong(expected, -1);
        shutdown(fd, SHUT_RDWR);
        cmds.join();
        close(fd);
        clientConnected = false;
        flog::info("P25: decoder disconnected from rtl_tcp server");
    }
}

void RtlTcpServer::commandLoop(int fd) {
    uint8_t cmd[5];
    while (recvAll(fd, cmd, sizeof(cmd))) {
        uint32_t value;
        memcpy(&value, cmd + 1, 4);
        value = ntohl(value);
        switch (cmd[0]) {
        case 0x01:
            lastFrequency = value;
            if (onFrequency) { onFrequency(value); }
            break;
        case 0x02:
            lastSampleRate = value;
            if (onSampleRate) { onSampleRate(value); }
            break;
        default:
            ignoredCommands++;
            break;
        }
    }
    // Client went away: the sender loop notices within one read timeout.
    int expected = fd;
    clientFd.compare_exchange_strong(expected, -1);
}
