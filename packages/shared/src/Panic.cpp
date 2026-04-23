#include "Panic.hpp"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <new>
#include <sstream>
#include <typeinfo>

#if defined(__linux__) || defined(__APPLE__)
#include <execinfo.h>
#define FIRMIUS_PANIC_HAS_BACKTRACE 1
#else
#define FIRMIUS_PANIC_HAS_BACKTRACE 0
#endif

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#define FIRMIUS_PANIC_HAS_UNISTD 1
#else
#define FIRMIUS_PANIC_HAS_UNISTD 0
#endif

namespace firmius::shared {

namespace {
std::function<void()> g_prePanicCallback;

#if defined(__linux__)
std::string getExecutablePath() {
    char buf[1024];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len != -1) {
        buf[len] = '\0';
        return std::string(buf);
    }
    return "";
}

void resolveAndPrint(void* addr) {
    std::string exe = getExecutablePath();
    if (exe.empty()) {
        return;
    }

    std::stringstream cmd;
    cmd << "addr2line -e " << exe << " -f -C " << addr;

    FILE* fp = popen(cmd.str().c_str(), "r");
    if (!fp) {
        return;
    }

    char buf[512];
    if (fgets(buf, sizeof(buf), fp)) {
        std::string func = buf;
        if (!func.empty() && func.back() == '\n') {
            func.pop_back();
        }
        if (fgets(buf, sizeof(buf), fp)) {
            std::string line = buf;
            if (!line.empty() && line.back() == '\n') {
                line.pop_back();
            }
            std::cerr << "  at " << func << " (" << line << ")\n";
        }
    }
    pclose(fp);
}
#endif

void printBacktrace() {
#if FIRMIUS_PANIC_HAS_BACKTRACE
    void* array[50];
    int size = backtrace(array, 50);
    char** symbols = backtrace_symbols(array, size);

    std::cerr << "\n[SYMBOLICATED BACKTRACE]\n";
    for (int i = 0; i < size; ++i) {
        std::cerr << "#" << i << " " << (symbols ? symbols[i] : "???") << "\n";
#if defined(__linux__)
        resolveAndPrint(array[i]);
#endif
    }

    if (symbols) {
        free(symbols);
    }
#else
    std::cerr << "\n[BACKTRACE UNAVAILABLE ON THIS PLATFORM]\n";
#endif
}

} // namespace

std::mutex Panic::registryMutex;
std::unordered_map<std::string, Panic::DebugInfoEntry> Panic::extraInfoRegistry;

void Panic::addExtraInfo(const std::string& name, const std::string& content) {
    std::lock_guard<std::mutex> lock(registryMutex);
    extraInfoRegistry.insert_or_assign(name, DebugInfoEntry(content));
}

void Panic::addExtraInfo(const std::string& name, DebugInfoCallback callback) {
    std::lock_guard<std::mutex> lock(registryMutex);
    extraInfoRegistry.insert_or_assign(name, DebugInfoEntry(std::move(callback)));
}

void Panic::removeExtraInfo(const std::string& name) {
    std::lock_guard<std::mutex> lock(registryMutex);
    extraInfoRegistry.erase(name);
}

void Panic::printExtraInfo() {
    std::lock_guard<std::mutex> lock(registryMutex);

    if (extraInfoRegistry.empty()) {
        return;
    }

    std::cerr << "\n[EXTRA DEBUG INFO]\n";

    for (const auto& [name, entry] : extraInfoRegistry) {
        std::cerr << "[" << name << "]\n";

        try {
            if (entry.isDynamic && entry.dynamicCallback) {
                std::string content = entry.dynamicCallback();
                std::cerr << content << "\n";
            } else {
                std::cerr << entry.staticContent << "\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "[Error collecting info: " << e.what() << "]\n";
        } catch (...) {
            std::cerr << "[Error collecting info: unknown exception]\n";
        }
    }
}

void terminateHandler() {
    if (g_prePanicCallback) {
        g_prePanicCallback();
    }

    std::cerr << "\n[FIRMIUS TERMINATE CALLED]\n";
    auto ex = std::current_exception();
    if (ex) {
        try {
            std::rethrow_exception(ex);
        } catch (const std::exception& e) {
            std::cerr << "Exception Type: " << typeid(e).name() << "\n";
            std::cerr << "Exception Message: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "Unknown Exception caught in terminate handler.\n";
        }
    } else {
        std::cerr << "No active exception found in terminate handler.\n";
    }

    printBacktrace();
    Panic::printExtraInfo();
    std::abort();
}

void Panic::signalHandler(int sig) {
    const char* signalName = "UNKNOWN";
    switch (sig) {
        case SIGSEGV:
            signalName = "SIGSEGV";
            break;
#ifdef SIGBUS
        case SIGBUS:
            signalName = "SIGBUS";
            break;
#endif
        case SIGFPE:
            signalName = "SIGFPE";
            break;
        case SIGABRT:
            signalName = "SIGABRT";
            break;
    }

    const char* msg1 = "\n[FIRMIUS CRASH] Caught signal: ";
#if FIRMIUS_PANIC_HAS_UNISTD && FIRMIUS_PANIC_HAS_BACKTRACE
    write(STDERR_FILENO, msg1, strlen(msg1));
    write(STDERR_FILENO, signalName, strlen(signalName));
    write(STDERR_FILENO, "\n", 1);

    void* array[50];
    int size = backtrace(array, 50);
    backtrace_symbols_fd(array, size, STDERR_FILENO);
#else
    std::cerr << msg1 << signalName << "\n";
#endif

    // Note: printExtraInfo and prePanicCallback are NOT async-signal-safe.
    // We skip them in the signal handler to avoid deadlocks or further crashes.
#if FIRMIUS_PANIC_HAS_UNISTD
    _exit(1);
#else
    std::_Exit(1);
#endif
}

void memoryPressureHandler() {
    std::cerr << "\n[FIRMIUS] Memory allocation failed (std::bad_alloc). "
              << "System is under memory pressure.\n";
    printBacktrace();
    Panic::printExtraInfo();
    throw std::bad_alloc();
}

void Panic::init() {
    std::set_terminate(terminateHandler);
    std::set_new_handler(memoryPressureHandler);
    std::signal(SIGSEGV, Panic::signalHandler);
#ifdef SIGBUS
    std::signal(SIGBUS, Panic::signalHandler);
#endif
    std::signal(SIGFPE, Panic::signalHandler);
    std::signal(SIGABRT, Panic::signalHandler);
}

void Panic::trigger(const std::string& message, const char* file, int line) {
    if (g_prePanicCallback) {
        g_prePanicCallback();
    }
    std::cerr << "\n[FIRMIUS PANIC] at " << file << ":" << line << "\n";
    std::cerr << "Message: " << message << "\n";
    printBacktrace();
    printExtraInfo();
    std::abort();
}

void Panic::setPrePanicCallback(std::function<void()> callback) {
    g_prePanicCallback = std::move(callback);
}

} // namespace firmius::shared
