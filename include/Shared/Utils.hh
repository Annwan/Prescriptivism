#ifndef PRESCRIPTIVISM_SHARED_UTILS_HH
#define PRESCRIPTIVISM_SHARED_UTILS_HH

#include <base/Base.hh>

#include <chrono>
#include <cstdarg>
#include <format>
#include <functional>
#include <print>
#include <string>
#include <thread>

#define FWD(x) std::forward<decltype(x)>(x)

namespace pr {
using namespace base;

struct Profile;
struct ZTermString;

template <typename T>
struct Debug;

template <typename T, auto deleter>
class Handle;

template <typename T>
class LateInit;

template <typename ResultType>
class Thread;

void CloseLoggingThread();

/// Check if a range is empty. This circumvents the stupid
/// amortised time constraint that the standard library’s
/// rgs::empty() has.
template <typename Range>
bool Empty(Range&& range) {
    if constexpr (requires { rgs::empty(std::forward<Range>(range)); }) {
        return rgs::empty(std::forward<Range>(range));
    } else {
        return range.begin() == range.end();
    }
}

struct SilenceLog {
    LIBBASE_IMMOVABLE(SilenceLog);
    SilenceLog();
    ~SilenceLog();
};

template <typename... Args>
void Log(std::format_string<Args...> fmt, Args&&... args);
} // namespace pr

namespace pr {
void LogImpl(std::string);
}

template <typename T>
struct pr::Debug {
    T* value;
    Debug(T* t) : value(t) {}
    Debug(T& t) : value(&t) {}
    Debug(T&& t) : value(&t) {}
};

template <typename T, auto deleter>
class pr::Handle {
    T value{};

public:
    Handle() = default;
    Handle(T value) : value(std::move(value)) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { Delete(); }

    Handle(Handle&& other) noexcept : value(std::exchange(other.value, T{})) {}
    auto operator=(Handle&& other) noexcept -> Handle& {
        if (this != &other) {
            Delete();
            value = std::exchange(other.value, T{});
        }
        return *this;
    }

    [[nodiscard]] auto operator->() -> T* { return &value; }
    [[nodiscard]] auto operator*() -> T& { return value; }
    [[nodiscard]] auto get() const -> const T& { return value; }

private:
    void Delete() {
        if (value) deleter(value);
    }
};

template <typename T>
class pr::LateInit {
    LIBBASE_IMMOVABLE(LateInit);
    alignas(T) mutable std::byte storage[sizeof(T)];
    mutable bool initialised = false;

public:
    LateInit() = default;
    ~LateInit() {
        if (initialised) Ptr()->~T();
    }

    void operator=(T&& value)
    requires std::is_move_assignable_v<T>
    {
        if (not initialised) init(std::move(value));
        else *Ptr() = std::move(value);
    }

    template <typename... Args>
    auto init(Args&&... args) const -> T* {
        if (initialised) Ptr()->~T();
        initialised = true;
        new (storage) T(std::forward<Args>(args)...);
        return Ptr();
    }

    void reset() {
        if (initialised) Ptr()->~T();
        initialised = false;
    }

    [[nodiscard]] auto operator->() const -> T* {
        Assert(initialised, "LateInit not initialised!");
        return Ptr();
    }

    [[nodiscard]] auto operator*() const -> T& {
        Assert(initialised, "LateInit not initialised!");
        return *Ptr();
    }

private:
    auto Ptr() const -> T* { return reinterpret_cast<T*>(std::launder(storage)); }
};

struct pr::Profile {
    std::string name;
    chr::system_clock::time_point start = chr::system_clock::now();
    Profile(std::string name) : name(name) {}
    ~Profile() {
        auto end = chr::system_clock::now();
        auto duration = chr::duration_cast<chr::milliseconds>(end - start);
        std::println("Profile ({}): {}ms", name, duration.count());
    }
};

/// Thread that can be restarted, aborted, and which may return a value.
template <typename ResultType>
class pr::Thread {
    LIBBASE_IMMOVABLE(Thread);
    std::atomic_bool run_flag = false;
    Result<ResultType> result = Error("Thread was aborted");

    // The thread MUST be the last member of this class, otherwise, we
    // might try to write to the result or run flag after they have been
    // destroyed if the thread is joined in the destructor.
    std::jthread thread;

public:
    explicit Thread() = default;

    template <typename Callable, typename... Args>
    requires (not std::is_same_v<Callable, Thread>)
    explicit Thread(Callable&& c, Args&&... args) {
        start(std::forward<Callable>(c), std::forward<Args>(args)...);
    }

    /// Check if the thread is running.
    [[nodiscard]] bool running() const { return run_flag.load(std::memory_order::acquire); }

    /// Start the thread.
    template <typename Callable, typename... Args>
    void start(Callable&& c, Args&&... args) { // clang-format off
        Assert(not running(), "Thread already started!");
        result = Error("Thread was aborted");
        thread = std::jthread{[
            this,
            ...args = std::forward<Args>(args),
            callable = std::forward<Callable>(c)
        ](
            std::stop_token stop
        ) {
            // Stop token goes at the end because the first argument
            // might be 'this' if the callable is a member function.
            result = std::invoke(callable, args..., stop);
            run_flag.store(false, std::memory_order::release);
        }};
        run_flag.store(true, std::memory_order::release);
    } // clang-format on

    /// Stop the thread and unbind it from the current instance.
    void stop_and_release() {
        if (thread.request_stop()) thread.detach();
    }

    /// Get the result value of the thread.
    [[nodiscard]] auto value() -> Result<ResultType>
    requires (not std::is_void_v<ResultType>)
    {
        Assert(not running(), "Thread is still running!");
        return std::move(result);
    }
};

/// Wrapper around a null-terminated string; this is non-owning
/// and should only be used in function parameters.
struct pr::ZTermString {
private:
    std::string_view data;

public:
    constexpr ZTermString() : data("") {}
    constexpr ZTermString(std::string& str) : data(str) {}

    template <usz n>
    constexpr ZTermString(const char (&str)[n]) : data(str, n - 1) {}

    auto c_str() const -> const char* { return data.data(); }
};

template <typename... Args>
void pr::Log(std::format_string<Args...> fmt, Args&&... args) {
    pr::LogImpl(std::format(fmt, std::forward<Args>(args)...));
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
template <typename T>
struct std::formatter<pr::Debug<T>> : std::formatter<std::string> {
    template <typename FormatContext>
    auto format(pr::Debug<T> t, FormatContext& ctx) const {
        std::string s;

        auto Formatter = [&](const char* fmt, ...) {
            va_list ap;
            va_start(ap, fmt);
            auto sz = std::vsnprintf(nullptr, 0, fmt, ap);
            va_end(ap);

            s.resize(s.size() + sz + 1);
            va_start(ap, fmt);
            std::vsnprintf(s.data() + s.size() - sz - 1, sz + 1, fmt, ap);
            va_end(ap);
        };

        __builtin_dump_struct(t.value, Formatter);
        return std::formatter<std::string>::format(s, ctx);
    }
};
#pragma clang diagnostic pop

#endif // PRESCRIPTIVISM_SHARED_UTILS_HH
