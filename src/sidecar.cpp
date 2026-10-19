#include "sidecar.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <fstream>
#include <utils/flog.h>

namespace {
constexpr size_t MAX_LINES = 500;
constexpr std::streamoff MAX_LOG_BYTES = 5 << 20;
const int BACKOFF_S[] = { 1, 2, 5, 10, 30 };
}

void Sidecar::start(const std::string& exe, const std::vector<std::string>& args,
                    const std::vector<std::string>& env, const std::string& logPath) {
    stop();
    this->exe = exe;
    this->args = args;
    this->env = env;
    this->logPath = logPath;
    restartCount = 0;
    wanted = true;
    monitorThread = std::thread(&Sidecar::monitor, this);
}

void Sidecar::stop(int graceMs) {
    wanted = false;
    int p = pid;
    if (p > 0) {
        kill(-p, SIGTERM);
        for (int waited = 0; pid > 0 && waited < graceMs; waited += 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (pid > 0) { kill(-p, SIGKILL); }
    }
    if (monitorThread.joinable()) { monitorThread.join(); }
    std::lock_guard lck(logMtx);
    lastState = "stopped";
}

std::string Sidecar::state() {
    std::lock_guard lck(logMtx);
    return lastState;
}

std::vector<std::string> Sidecar::tail(size_t n) {
    std::lock_guard lck(logMtx);
    size_t from = lines.size() > n ? lines.size() - n : 0;
    return std::vector<std::string>(lines.begin() + from, lines.end());
}

int Sidecar::spawn(int& outFd) {
    int pipeFds[2];
    if (pipe2(pipeFds, O_CLOEXEC) < 0) { return -1; }
    pid_t parent = getpid();
    pid_t child = fork();
    if (child < 0) {
        close(pipeFds[0]);
        close(pipeFds[1]);
        return -1;
    }
    if (child == 0) {
        setpgid(0, 0);
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent) { _exit(1); }
        dup2(pipeFds[1], STDOUT_FILENO);
        dup2(pipeFds[1], STDERR_FILENO);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); }
        for (int fd = 3; fd < 1024; fd++) { close(fd); }
        for (const auto& kv : env) { putenv(strdup(kv.c_str())); }
        std::vector<char*> argv;
        argv.push_back((char*)exe.c_str());
        for (const auto& a : args) { argv.push_back((char*)a.c_str()); }
        argv.push_back(nullptr);
        execv(exe.c_str(), argv.data());
        fprintf(stderr, "exec %s failed: %s\n", exe.c_str(), strerror(errno));
        _exit(127);
    }
    setpgid(child, child);
    close(pipeFds[1]);
    outFd = pipeFds[0];
    return child;
}

void Sidecar::monitor() {
    std::chrono::steady_clock::time_point windowStart = std::chrono::steady_clock::now();
    int crashesInWindow = 0;
    while (wanted) {
        int outFd = -1;
        int child = spawn(outFd);
        if (child < 0) {
            std::lock_guard lck(logMtx);
            lastState = "failed to start";
            break;
        }
        pid = child;
        {
            std::lock_guard lck(logMtx);
            lastState = "running";
        }
        flog::info("P25: started decoder (pid {})", child);

        std::ofstream log(logPath, std::ios::app);
        std::string partial;
        char buf[4096];
        ssize_t n;
        while ((n = read(outFd, buf, sizeof(buf))) > 0) {
            partial.append(buf, n);
            size_t pos;
            while ((pos = partial.find('\n')) != std::string::npos) {
                std::string line = partial.substr(0, pos);
                partial.erase(0, pos + 1);
                if (log) {
                    log << line << '\n';
                    if (log.tellp() > MAX_LOG_BYTES) {
                        log.close();
                        rename(logPath.c_str(), (logPath + ".1").c_str());
                        log.open(logPath, std::ios::trunc);
                    }
                }
                std::lock_guard lck(logMtx);
                lines.push_back(line);
                if (lines.size() > MAX_LINES) { lines.pop_front(); }
            }
        }
        close(outFd);
        int status = 0;
        waitpid(child, &status, 0);
        pid = -1;
        log.flush();

        std::string how = WIFEXITED(status) ? "exited with code " + std::to_string(WEXITSTATUS(status))
                                            : "killed by signal " + std::to_string(WTERMSIG(status));
        flog::info("P25: decoder {}", how);
        if (!wanted) { break; }

        auto now = std::chrono::steady_clock::now();
        if (now - windowStart > std::chrono::minutes(2)) {
            windowStart = now;
            crashesInWindow = 0;
        }
        if (++crashesInWindow > 5) {
            std::lock_guard lck(logMtx);
            lastState = "gave up after repeated exits (" + how + ")";
            wanted = false;
            break;
        }
        int delay = BACKOFF_S[std::min(restartCount.load(), 4)];
        restartCount++;
        {
            std::lock_guard lck(logMtx);
            lastState = how + ", restarting in " + std::to_string(delay) + " s";
        }
        for (int i = 0; i < delay * 10 && wanted; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}
