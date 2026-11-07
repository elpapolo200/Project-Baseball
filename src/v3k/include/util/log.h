// Adaptador del port de MLB15: sustituye util/log.h de Vita3K (spdlog + boost).
// TRACE/DEBUG no generan nada (como en las builds de Vita3K); el resto va a
// v3k_log, que imprime con límite (MLB15_V3K_LOG=0..4 sube el detalle).
#pragma once

#include <fmt/format.h>
#include <fmt/ostream.h>

#include <atomic>
#include <string>
#include <type_traits>

void v3k_log(int level, const std::string &msg);
int v3k_log_level();

template <typename... Args>
inline void v3k_logf(int level, fmt::format_string<Args...> f, Args &&...args) {
    if (level <= v3k_log_level())
        v3k_log(level, fmt::format(f, std::forward<Args>(args)...));
}
inline void v3k_logf(int level, const std::string &s) {
    if (level <= v3k_log_level())
        v3k_log(level, s);
}

#define LOG_TRACE(...) static_cast<void>(0)
#define LOG_DEBUG(...) static_cast<void>(0)
#define LOG_INFO(...) v3k_logf(3, __VA_ARGS__)
#define LOG_WARN(...) v3k_logf(2, __VA_ARGS__)
#define LOG_ERROR(...) v3k_logf(1, __VA_ARGS__)
#define LOG_CRITICAL(...) v3k_logf(0, __VA_ARGS__)

#define LOG_ONCE(log_function, ...)         \
    do {                                    \
        static std::atomic_flag has_logged; \
        if (!has_logged.test_and_set())     \
            log_function(__VA_ARGS__);      \
    } while (0)

#define LOG_TRACE_ONCE(...) LOG_ONCE(LOG_TRACE, __VA_ARGS__)
#define LOG_DEBUG_ONCE(...) LOG_ONCE(LOG_DEBUG, __VA_ARGS__)
#define LOG_INFO_ONCE(...) LOG_ONCE(LOG_INFO, __VA_ARGS__)
#define LOG_WARN_ONCE(...) LOG_ONCE(LOG_WARN, __VA_ARGS__)
#define LOG_ERROR_ONCE(...) LOG_ONCE(LOG_ERROR, __VA_ARGS__)
#define LOG_CRITICAL_ONCE(...) LOG_ONCE(LOG_CRITICAL, __VA_ARGS__)

template <typename T>
std::string log_hex(T val) {
    return fmt::format("0x{:X}", static_cast<std::make_unsigned_t<T>>(val));
}

template <typename T>
std::string log_hex_full(T val) {
    return fmt::format("0x{:0{}X}", static_cast<std::make_unsigned_t<T>>(val), sizeof(T) * 2);
}

// Vita3K formatea enums con boost::describe; aquí salen como número.
FMT_BEGIN_NAMESPACE
template <typename T, typename Char>
    requires(std::is_enum_v<T>)
struct formatter<T, Char> : formatter<std::underlying_type_t<T>, Char> {
    template <typename FormatContext>
    auto format(const T e, FormatContext &ctx) const {
        return formatter<std::underlying_type_t<T>, Char>::format(static_cast<std::underlying_type_t<T>>(e), ctx);
    }
};
FMT_END_NAMESPACE
