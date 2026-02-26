#include "Panic.hpp"
#include <iostream>
#include <exception>
#include <execinfo.h>
#include <unistd.h>
#include <typeinfo>
#include <cstdlib>
#include <cxxabi.h>
#include <vector>
#include <sstream>

namespace firmius::shared {

namespace {
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
    if (exe.empty()) return;

    std::stringstream cmd;
    cmd << "addr2line -e " << exe << " -f -C " << addr;
    
    FILE* fp = popen(cmd.str().c_str(), "r");
    if (fp) {
        char buf[512];
        if (fgets(buf, sizeof(buf), fp)) {
            std::string func = buf;
            if (!func.empty() && func.back() == '\n') func.pop_back();
            if (fgets(buf, sizeof(buf), fp)) {
                std::string line = buf;
                if (!line.empty() && line.back() == '\n') line.pop_back();
                std::cerr << "  at " << func << " (" << line << ")\n";
            }
        }
        pclose(fp);
    }
}
}

void printBacktrace() {
    void* array[50];
    int size = backtrace(array, 50);
    char** symbols = backtrace_symbols(array, size);
    
    std::cerr << "\n[SYMBOLICATED BACKTRACE]\n";
    for (int i = 0; i < size; ++i) {
        std::cerr << "#" << i << " " << (symbols ? symbols[i] : "???") << "\n";
        resolveAndPrint(array[i]);
    }
    if (symbols) free(symbols);
}

void terminateHandler() {
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
    std::abort();
}

void Panic::init() {
    std::set_terminate(terminateHandler);
}

void Panic::trigger(const std::string& message, const char* file, int line) {
    std::cerr << "\n[FIRMIUS PANIC] at " << file << ":" << line << "\n";
    std::cerr << "Message: " << message << "\n";
    printBacktrace();
    std::abort();
}

}
