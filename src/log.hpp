#pragma once
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <format>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// Namespace is "applog" instead of "log" to avoid clashing with the C math
// function ::log that MSVC injects into the global namespace via <cmath>.
namespace applog {

enum class Level { Debug, Info, Warn, Error };

inline const char* level_str(Level l) noexcept {
    switch (l) {
    case Level::Debug: return "DEBUG";
    case Level::Info:  return "INFO ";
    case Level::Warn:  return "WARN ";
    case Level::Error: return "ERROR";
    }
    return "?????";
}

struct Entry {
    Level       lvl{Level::Debug};
    std::string file;
    int         line{};
    std::string msg;
    std::string timestamp; // "HH:MM:SS.mmm"
};

inline constexpr std::size_t RING_CAP = 2048;

namespace detail {

// constexpr basename — evaluated at compile-time when called with __FILE__.
constexpr std::string_view file_base(const char* path) noexcept {
    std::string_view sv(path);
    auto p = sv.rfind('/');
    if (p == sv.npos) p = sv.rfind('\\');
    return p == sv.npos ? sv : sv.substr(p + 1);
}

struct State {
    std::mutex mu;
    std::array<Entry, RING_CAP> ring{};
    std::size_t head{0};   // total writes (unbounded); slot = head % RING_CAP
    FILE*       fp{nullptr};
    Level       min_level{Level::Debug};
    std::function<void(const Entry&)> on_entry;
};

inline State& state() noexcept {
    static State s;
    return s;
}

inline void write_raw(Level lvl, std::string_view file, int line, std::string msg) {
    auto& s = state();
    std::lock_guard lk(s.mu);
    if (static_cast<int>(lvl) < static_cast<int>(s.min_level)) return;

    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    int  ms  = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()).count() % 1000);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &tt);
#else
    localtime_r(&tt, &tm_buf);
#endif
    char ts[16];
    std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm_buf);
    auto ts_str = std::format("{}.{:03d}", ts, ms);

    auto line_txt = std::format("[{}] [{}] [{}:{}] {}\n",
                                ts_str, level_str(lvl), file, line, msg);
    std::fputs(line_txt.c_str(), stderr);
    if (s.fp) { std::fputs(line_txt.c_str(), s.fp); std::fflush(s.fp); }

    Entry e;
    e.lvl       = lvl;
    e.file      = std::string(file);
    e.line      = line;
    e.msg       = std::move(msg);
    e.timestamp = ts_str;
    s.ring[s.head % RING_CAP] = std::move(e);
    if (s.on_entry) s.on_entry(s.ring[s.head % RING_CAP]);
    ++s.head;
}

template<typename... Args>
void write(Level lvl, std::string_view file, int line,
           std::format_string<Args...> fmt, Args&&... args) {
    write_raw(lvl, file, line, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace detail

// ── Public API ────────────────────────────────────────────────────────────────

// Open a file sink at `path`.  Call once before any LOG_* macros.
inline void init(const std::string& path, Level min_level = Level::Debug) {
    auto& s = detail::state();
    std::lock_guard lk(s.mu);
    s.min_level = min_level;
    if (!path.empty()) {
#ifdef _WIN32
        fopen_s(&s.fp, path.c_str(), "w");
#else
        s.fp = std::fopen(path.c_str(), "w");
#endif
    }
}

inline void shutdown() {
    auto& s = detail::state();
    std::lock_guard lk(s.mu);
    if (s.fp) { std::fclose(s.fp); s.fp = nullptr; }
}

inline void set_level(Level l) {
    auto& s = detail::state();
    std::lock_guard lk(s.mu);
    s.min_level = l;
}

inline void set_entry_callback(std::function<void(const Entry&)> fn) {
    auto& s = detail::state();
    std::lock_guard lk(s.mu);
    s.on_entry = std::move(fn);
}

// Chronological snapshot of the ring buffer (oldest first, up to max_count).
inline std::vector<Entry> recent_entries(std::size_t max_count = RING_CAP) {
    auto& s = detail::state();
    std::lock_guard lk(s.mu);
    std::size_t n = std::min({s.head, RING_CAP, max_count});
    std::size_t oldest_wi = s.head - n;
    std::vector<Entry> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        out.push_back(s.ring[(oldest_wi + i) % RING_CAP]);
    return out;
}

} // namespace applog

// ── Macros ────────────────────────────────────────────────────────────────────
#define LOG_DEBUG(...) ::applog::detail::write(::applog::Level::Debug, \
    ::applog::detail::file_base(__FILE__), __LINE__, __VA_ARGS__)
#define LOG_INFO(...)  ::applog::detail::write(::applog::Level::Info,  \
    ::applog::detail::file_base(__FILE__), __LINE__, __VA_ARGS__)
#define LOG_WARN(...)  ::applog::detail::write(::applog::Level::Warn,  \
    ::applog::detail::file_base(__FILE__), __LINE__, __VA_ARGS__)
#define LOG_ERR(...)   ::applog::detail::write(::applog::Level::Error, \
    ::applog::detail::file_base(__FILE__), __LINE__, __VA_ARGS__)
