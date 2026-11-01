// rtltcp input module for FernSDR: a network RTL-SDR source over the rtl_tcp
// protocol, with an optional read-only mode for OpenWebRX rtltcp_compat feeds.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Contract: docs/MODULES.md in FernSDR, API 1 (input). One module process
// delivers samples for one band. Commands arrive on fd 0 as JSON lines,
// events leave on fd 3, samples leave on fd 1 raw in the format `ready`
// announces, and log lines leave on fd 2.

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <time.h>

#include "json.h"

#ifndef FERN_RTLTCP_VERSION
#define FERN_RTLTCP_VERSION "0.2.0"
#endif

namespace {

constexpr int kApi = 1;
const char kId[] = "rtltcp";

const char* tuner_name(uint32_t tuner) {
    switch (tuner) {
        case 1: return "E4000";
        case 2: return "FC0012";
        case 3: return "FC0013";
        case 4: return "FC2580";
        case 5: return "R820T";
        case 6: return "R828D";
        default: return "unknown";
    }
}

// --- small IO helpers -----------------------------------------------------

int64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// Best-effort write of everything; returns 0 on success, -1 on error.
int write_all(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return 0;
}

void emit(const json::Value& v) {
    std::string s = json::dump(v);
    s.push_back('\n');
    write_all(3, s.data(), s.size());
}

void log_line(const std::string& message) {
    std::string s = message;
    s.push_back('\n');
    write_all(2, s.data(), s.size());
}

// --- rtl_tcp client -------------------------------------------------------

int connect_timeout(const std::string& host, int port, int timeout_ms) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_text[16];
    snprintf(port_text, sizeof port_text, "%d", port);
    struct addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), port_text, &hints, &result) != 0) return -1;

    int fd = -1;
    for (struct addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        int s = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (s < 0) continue;
        int flags = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(s, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            fd = s;
            break;
        }
        if (rc < 0 && errno == EINPROGRESS) {
            struct pollfd pfd;
            pfd.fd = s;
            pfd.events = POLLOUT;
            int pr = poll(&pfd, 1, timeout_ms);
            if (pr > 0 && (pfd.revents & POLLOUT)) {
                int error = 0;
                socklen_t error_len = sizeof error;
                if (getsockopt(s, SOL_SOCKET, SO_ERROR, &error, &error_len) == 0 && error == 0) {
                    fd = s;
                    break;
                }
            }
        }
        close(s);
    }
    freeaddrinfo(result);
    if (fd >= 0) {
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
    return fd;
}

bool recv_all(int fd, void* data, size_t len) {
    char* p = static_cast<char*>(data);
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool send_all(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

void send_command(int sock, uint8_t cmd, uint32_t param) {
    uint8_t buf[5];
    buf[0] = cmd;
    buf[1] = static_cast<uint8_t>((param >> 24) & 0xff);
    buf[2] = static_cast<uint8_t>((param >> 16) & 0xff);
    buf[3] = static_cast<uint8_t>((param >> 8) & 0xff);
    buf[4] = static_cast<uint8_t>(param & 0xff);
    send_all(sock, buf, sizeof buf);
}

// Opens the endpoint and reads the 12-byte header. Returns the socket, or -1.
int open_socket(const std::string& host, int port, uint32_t* tuner_out, uint32_t* gains_out) {
    int sock = connect_timeout(host, port, 10000);
    if (sock < 0) return -1;
    uint8_t header[12];
    if (!recv_all(sock, header, sizeof header)) {
        close(sock);
        return -1;
    }
    if (memcmp(header, "RTL0", 4) != 0) {
        close(sock);
        return -1;
    }
    uint32_t tuner = (static_cast<uint32_t>(header[4]) << 24) | (static_cast<uint32_t>(header[5]) << 16) |
                     (static_cast<uint32_t>(header[6]) << 8) | static_cast<uint32_t>(header[7]);
    uint32_t gains = (static_cast<uint32_t>(header[8]) << 24) | (static_cast<uint32_t>(header[9]) << 16) |
                     (static_cast<uint32_t>(header[10]) << 8) | static_cast<uint32_t>(header[11]);
    if (tuner_out) *tuner_out = tuner;
    if (gains_out) *gains_out = gains;
    return sock;
}

// --- settings access ------------------------------------------------------

std::string setting_string(const json::Value& settings, const char* key, const char* fallback) {
    return settings.has(key) ? settings.get(key).as_string(fallback) : fallback;
}

double setting_number(const json::Value& settings, const char* key, double fallback) {
    return settings.has(key) ? settings.get(key).as_number(fallback) : fallback;
}

bool setting_bool(const json::Value& settings, const char* key, bool fallback) {
    return settings.has(key) ? settings.get(key).as_bool(fallback) : fallback;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

std::string ascii_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

void set_gain(int sock, const std::string& gain_in) {
    std::string gain = ascii_lower(trim(gain_in));
    if (gain.empty() || gain == "auto") {
        send_command(sock, 0x03, 0);  // auto
    } else {
        send_command(sock, 0x03, 1);  // manual
        double db = atof(gain.c_str());
        send_command(sock, 0x04, static_cast<uint32_t>(llround(db * 10.0)));
    }
}

void apply_control(int sock, uint32_t rate, uint32_t center, const json::Value& settings) {
    if (!setting_bool(settings, "control", true)) return;
    send_command(sock, 0x02, rate);
    send_command(sock, 0x01, center);
    set_gain(sock, setting_string(settings, "gain", "auto"));
    send_command(sock, 0x05, static_cast<uint32_t>(setting_number(settings, "ppm", 0.0)));
    send_command(sock, 0x0e, setting_bool(settings, "bias_tee", false) ? 1 : 0);
}

void apply_live(int sock, const json::Value& settings) {
    if (!setting_bool(settings, "control", true)) return;
    if (settings.has("gain")) set_gain(sock, setting_string(settings, "gain", "auto"));
    if (settings.has("ppm")) send_command(sock, 0x05, static_cast<uint32_t>(setting_number(settings, "ppm", 0.0)));
    if (settings.has("bias_tee")) send_command(sock, 0x0e, setting_bool(settings, "bias_tee", false) ? 1 : 0);
}

// --- self-description -----------------------------------------------------

json::Value setting_def(const char* key, const char* type, const char* label) {
    json::Value s = json::Value::make_object();
    s.set("key", json::Value::make_string(key));
    s.set("type", json::Value::make_string(type));
    s.set("label", json::Value::make_string(label));
    return s;
}

json::Value describe() {
    json::Value d = json::Value::make_object();
    d.set("api", json::Value::make_number(kApi));
    d.set("id", json::Value::make_string(kId));
    d.set("name", json::Value::make_string("rtl_tcp"));
    d.set("version", json::Value::make_string(FERN_RTLTCP_VERSION));
    d.set("kind", json::Value::make_string("input"));

    json::Value settings = json::Value::make_array();
    {
        json::Value s = setting_def("host", "string", "Host");
        s.set("default", json::Value::make_string("127.0.0.1"));
        s.set("help", json::Value::make_string("rtl_tcp host, e.g. 127.0.0.1 through a tunnel"));
        settings.push(std::move(s));
    }
    {
        json::Value s = setting_def("device", "string", "Device");
        s.set("default", json::Value::make_string(""));
        s.set("help", json::Value::make_string("A name for this endpoint; tells bands going through the same module apart"));
        settings.push(std::move(s));
    }
    {
        json::Value s = setting_def("port", "number", "Port");
        s.set("default", json::Value::make_number(1234));
        s.set("min", json::Value::make_number(1));
        s.set("max", json::Value::make_number(65535));
        settings.push(std::move(s));
    }
    {
        json::Value s = setting_def("gain", "string", "Gain");
        s.set("default", json::Value::make_string("auto"));
        s.set("live", json::Value::make_bool(true));
        s.set("help", json::Value::make_string("auto, or dB"));
        settings.push(std::move(s));
    }
    {
        json::Value s = setting_def("ppm", "number", "Frequency correction (ppm)");
        s.set("default", json::Value::make_number(0));
        s.set("live", json::Value::make_bool(true));
        settings.push(std::move(s));
    }
    {
        json::Value s = setting_def("bias_tee", "boolean", "Bias tee");
        s.set("default", json::Value::make_bool(false));
        s.set("live", json::Value::make_bool(true));
        settings.push(std::move(s));
    }
    {
        json::Value s = setting_def("control", "boolean", "Send tuning commands");
        s.set("default", json::Value::make_bool(true));
        s.set("help", json::Value::make_string("off for read-only feeds such as OpenWebRX rtltcp_compat"));
        settings.push(std::move(s));
    }
    d.set("settings", std::move(settings));

    json::Value tuning = json::Value::make_object();
    json::Value ranges = json::Value::make_array();
    json::Value r = json::Value::make_array();
    r.push(json::Value::make_number(24000000));
    r.push(json::Value::make_number(1766000000));
    ranges.push(std::move(r));
    tuning.set("ranges", std::move(ranges));
    json::Value rates = json::Value::make_array();
    rates.push(json::Value::make_number(2048000));
    rates.push(json::Value::make_number(2400000));
    rates.push(json::Value::make_number(1024000));
    tuning.set("rates", std::move(rates));
    tuning.set("signal", json::Value::make_string("iq"));
    d.set("tuning", std::move(tuning));

    return d;
}

json::Value list_devices() {
    // A network source has no local hardware to enumerate: each band names
    // its endpoint through the host/port settings. An empty list keeps the
    // admin panel's device-assignment section from showing a stub device.
    json::Value d = json::Value::make_object();
    d.set("devices", json::Value::make_array());
    return d;
}

// --- the module session ---------------------------------------------------

struct State {
    int sock = -1;
    uint32_t tuner = 0;
    uint32_t gains = 0;
    uint32_t rate = 0;
    uint32_t center = 0;
    json::Value settings;
    uint64_t delivered = 0;
    uint64_t dropped = 0;
    bool opened = false;
};

json::Value hello_event() {
    json::Value h = json::Value::make_object();
    h.set("type", json::Value::make_string("hello"));
    h.set("api", json::Value::make_number(kApi));
    h.set("id", json::Value::make_string(kId));
    h.set("version", json::Value::make_string(FERN_RTLTCP_VERSION));
    h.set("kind", json::Value::make_string("input"));
    return h;
}

json::Value ready_event(const State& st) {
    json::Value r = json::Value::make_object();
    r.set("type", json::Value::make_string("ready"));
    r.set("format", json::Value::make_string("u8"));
    r.set("signal", json::Value::make_string("iq"));
    r.set("sample_rate", json::Value::make_number(static_cast<double>(st.rate)));
    r.set("center", json::Value::make_number(static_cast<double>(st.center)));
    json::Value device = json::Value::make_object();
    device.set("name", json::Value::make_string("RTL-SDR via rtl_tcp"));
    device.set("serial", json::Value::make_string(""));
    device.set("tuner", json::Value::make_string(tuner_name(st.tuner)));
    r.set("device", std::move(device));
    r.set("settings", st.settings);
    return r;
}

json::Value stats_event(uint64_t samples, uint64_t dropped) {
    json::Value s = json::Value::make_object();
    s.set("type", json::Value::make_string("stats"));
    s.set("samples", json::Value::make_number(static_cast<double>(samples)));
    s.set("dropped", json::Value::make_number(static_cast<double>(dropped)));
    return s;
}

json::Value error_event(const char* code, const std::string& message, bool fatal) {
    json::Value e = json::Value::make_object();
    e.set("type", json::Value::make_string("error"));
    e.set("code", json::Value::make_string(code));
    e.set("message", json::Value::make_string(message));
    e.set("fatal", json::Value::make_bool(fatal));
    return e;
}

// Handles one command line. Returns -1 to continue, or the exit status to use
// (0 for a stop, 3 for a device that cannot be opened).
int handle_command(const std::string& line, State& st) {
    json::Value message;
    json::Parser parser(line);
    if (!parser.parse(message) || !message.is_object()) return -1;  // ignore malformed

    std::string kind = message.has("type") ? message.get("type").as_string("") : "";

    if (kind == "open") {
        st.rate = static_cast<uint32_t>(message.get("sample_rate").as_number(0.0));
        st.center = static_cast<uint32_t>(message.get("center").as_number(0.0));
        const json::Value& settings = message.get("settings");
        st.settings = settings.is_object() ? settings : json::Value::make_object();

        std::string host = setting_string(st.settings, "host", "127.0.0.1");
        int port = static_cast<int>(setting_number(st.settings, "port", 1234.0));

        int sock = open_socket(host, port, &st.tuner, &st.gains);
        if (sock < 0) {
            log_line("connect failed: " + std::string(strerror(errno)));
            emit(error_event("no-device",
                             "cannot connect to rtl_tcp at " + host + ":" + std::to_string(port) + ": " + strerror(errno),
                             true));
            return 3;
        }
        st.sock = sock;
        apply_control(st.sock, st.rate, st.center, st.settings);
        st.opened = true;
        emit(ready_event(st));
    } else if (kind == "set") {
        const json::Value& settings = message.get("settings");
        if (settings.is_object()) {
            for (const auto& [key, value] : settings.object) st.settings.set(key, value);
        }
        if (st.sock >= 0) apply_live(st.sock, st.settings);
        json::Value applied = json::Value::make_object();
        applied.set("type", json::Value::make_string("applied"));
        if (message.has("id")) applied.set("id", message.get("id"));
        applied.set("settings", st.settings);
        emit(applied);
    } else if (kind == "stop") {
        return 0;
    }
    return -1;
}

// Reconnect for up to 30 seconds; returns true when streaming again.
bool reconnect(State& st) {
    auto deadline = now_ms() + 30000;
    std::string host = setting_string(st.settings, "host", "127.0.0.1");
    int port = static_cast<int>(setting_number(st.settings, "port", 1234.0));
    while (now_ms() < deadline) {
        int sock = open_socket(host, port, &st.tuner, &st.gains);
        if (sock >= 0) {
            st.sock = sock;
            apply_control(st.sock, st.rate, st.center, st.settings);
            return true;
        }
        usleep(1000000);
    }
    return false;
}

int run_module() {
    State st;
    emit(hello_event());
    std::string command_buffer;
    int64_t last_stats = now_ms();

    while (true) {
        struct pollfd fds[2];
        int nfds = 0;
        fds[nfds].fd = 0;
        fds[nfds].events = POLLIN;
        ++nfds;
        if (st.sock >= 0) {
            fds[nfds].fd = st.sock;
            fds[nfds].events = POLLIN;
            ++nfds;
        }

        int pr = poll(fds, static_cast<nfds_t>(nfds), 1000);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            char chunk[65536];
            ssize_t n = read(0, chunk, sizeof chunk);
            if (n < 0) {
                if (errno == EINTR) n = 0;
                else break;
            }
            if (n == 0) break;  // FernSDR is gone
            command_buffer.append(chunk, static_cast<size_t>(n));
            size_t pos;
            while ((pos = command_buffer.find('\n')) != std::string::npos) {
                std::string line = command_buffer.substr(0, pos);
                command_buffer.erase(0, pos + 1);
                int rc = handle_command(line, st);
                if (rc >= 0) return rc;
            }
        }

        if (st.sock >= 0 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            uint8_t data[65536];
            ssize_t n = recv(st.sock, data, sizeof data, 0);
            if (n < 0) {
                if (errno == EINTR) n = 0;
                else n = 0;  // treat read errors as stream loss
            }
            if (n == 0) {
                log_line("rtl_tcp closed the stream; reconnecting");
                ++st.dropped;
                close(st.sock);
                st.sock = -1;
                if (!reconnect(st)) {
                    emit(error_event("lost", "rtl_tcp did not come back", true));
                    return 5;
                }
            } else {
                if (write_all(1, data, static_cast<size_t>(n)) != 0) return 0;  // EPIPE: FernSDR is gone
                st.delivered += static_cast<uint64_t>(n) / 2;
            }
        }

        int64_t now = now_ms();
        if (now - last_stats >= 1000) {
            last_stats = now;
            emit(stats_event(st.delivered, st.dropped));
        }
    }

    if (st.sock >= 0) close(st.sock);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);

    bool module_mode = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--describe") {
            std::string s = json::dump(describe());
            s.push_back('\n');
            write_all(1, s.data(), s.size());
            return 0;
        }
        if (arg == "--list-devices") {
            std::string s = json::dump(list_devices());
            s.push_back('\n');
            write_all(1, s.data(), s.size());
            return 0;
        }
        if (arg == "--fernsdr-module") {
            module_mode = true;
        }
    }
    if (module_mode) return run_module();

    const char* usage = "usage: module --fernsdr-module 1 | --describe | --list-devices\n";
    write_all(2, usage, strlen(usage));
    return 2;
}
