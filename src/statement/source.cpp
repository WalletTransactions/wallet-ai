#include "statement/source.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>

#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "statement/textutil.hpp"

namespace stmt {

namespace {

int b64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

// Windows-1251, 0x80–0xFF → кодовые точки Unicode.
const unsigned short kCp1251[128] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409, 0x2039, 0x040A,
    0x040C, 0x040B, 0x040F, 0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0xFFFD, 0x2122,
    0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F, 0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6,
    0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407, 0x00B0, 0x00B1, 0x0406, 0x0456,
    0x0491, 0x00B5, 0x00B6, 0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
};

void append_utf8(std::string& out, unsigned code) {
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

std::string extension_of(const std::string& name) {
    auto dot = name.rfind('.');
    return dot == std::string::npos ? "" : lower(name.substr(dot + 1));
}

} // namespace

std::optional<std::vector<unsigned char>> decode_base64(const std::string& s) {
    std::vector<unsigned char> out;
    out.reserve(s.size() * 3 / 4);
    unsigned buffer = 0;
    int bits = 0;
    for (unsigned char c : s) {
        if (c == '=' || c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        int v = b64_value(c);
        if (v < 0) return std::nullopt;
        buffer = (buffer << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

std::string cp1251_to_utf8(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 2);
    for (char ch : s) {
        auto c = static_cast<unsigned char>(ch);
        if (c < 0x80) out.push_back(ch);
        else if (c >= 0xC0) append_utf8(out, 0x0410 + (c - 0xC0)); // А–я подряд
        else append_utf8(out, kCp1251[c - 0x80]);
    }
    return out;
}

std::string decode_text(const std::string& bytes) {
    std::string s = bytes;
    if (s.rfind("\xEF\xBB\xBF", 0) == 0) s.erase(0, 3);
    return valid_utf8(s) ? s : cp1251_to_utf8(s);
}

std::optional<std::string> pdf_to_text(const std::string& pdf, int timeout_secs, std::size_t memory_bytes) {
    int in_pipe[2];
    int out_pipe[2];
    if (pipe(in_pipe) != 0) return std::nullopt;
    if (pipe(out_pipe) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        return std::nullopt;
    }
    pid_t pid = fork();
    if (pid < 0) {
        for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1]}) close(fd);
        return std::nullopt;
    }
    if (pid == 0) {
        // Дочерний процесс: только async-signal-safe вызовы до exec.
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1]}) close(fd);
        struct rlimit lim;
        lim.rlim_cur = lim.rlim_max = memory_bytes;
        setrlimit(RLIMIT_AS, &lim);
        alarm(static_cast<unsigned>(timeout_secs)); // SIGALRM переживает exec и убивает pdftotext
        execlp("pdftotext", "pdftotext", "-layout", "-enc", "UTF-8", "-", "-", static_cast<char*>(nullptr));
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    fcntl(in_pipe[1], F_SETFL, O_NONBLOCK);
    signal(SIGPIPE, SIG_IGN); // pdftotext может закрыть stdin раньше, чем мы допишем

    std::string out;
    std::size_t written = 0;
    bool writing = true;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_secs + 2);
    char buf[65536];
    for (;;) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            kill(pid, SIGKILL);
            break;
        }
        struct pollfd fds[2];
        int nfds = 0;
        fds[nfds++] = {out_pipe[0], POLLIN, 0};
        if (writing) fds[nfds++] = {in_pipe[1], POLLOUT, 0};
        if (poll(fds, static_cast<nfds_t>(nfds), static_cast<int>(left.count())) < 0) {
            if (errno == EINTR) continue;
            kill(pid, SIGKILL);
            break;
        }
        if (writing && (fds[1].revents & (POLLOUT | POLLERR | POLLHUP)) != 0) {
            ssize_t n = write(in_pipe[1], pdf.data() + written, std::min<std::size_t>(pdf.size() - written, 65536));
            if (n > 0) written += static_cast<std::size_t>(n);
            if (n < 0 && errno != EAGAIN) written = pdf.size();
            if (written >= pdf.size()) {
                close(in_pipe[1]);
                writing = false;
            }
        }
        if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            ssize_t n = read(out_pipe[0], buf, sizeof(buf));
            if (n > 0) out.append(buf, static_cast<std::size_t>(n));
            if (n == 0) break; // EOF — процесс закрыл stdout
        }
    }
    if (writing) close(in_pipe[1]);
    close(out_pipe[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return std::nullopt;
    return out;
}

SourceResult extract_file(const std::string& name, const std::string& content_base64, std::size_t max_bytes) {
    SourceResult r;
    std::string ext = extension_of(name);
    if (ext != "pdf" && ext != "csv" && ext != "txt") {
        r.error = "unsupported_file";
        return r;
    }
    // Оценка до декодирования: base64 длиннее исходника на треть.
    if (content_base64.size() / 4 * 3 > max_bytes + 3) {
        r.error = "too_large";
        return r;
    }
    auto bytes = decode_base64(content_base64);
    if (!bytes) {
        r.error = "bad_base64";
        return r;
    }
    if (bytes->size() > max_bytes) {
        r.error = "too_large";
        return r;
    }
    std::string raw(bytes->begin(), bytes->end());
    if (ext == "pdf") {
        auto text = pdf_to_text(raw, 20, 256u * 1024 * 1024);
        if (!text) {
            r.error = "pdf_failed";
            return r;
        }
        r.text = *text;
    } else {
        r.text = decode_text(raw);
        r.csv = ext == "csv";
    }
    // pdftotext разделяет страницы \f.
    std::replace(r.text.begin(), r.text.end(), '\f', '\n');
    if (trim(normalize_spaces(r.text)).empty()) r.error = "no_text";
    return r;
}

} // namespace stmt
