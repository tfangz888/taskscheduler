#pragma once
#include <iostream>
#include <fstream>
#include <string>
#include <mutex>
#include <cstdarg>

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3
};

class Logger {
private:
    static std::mutex logMutex;
    static std::ofstream logFile;
    static LogLevel currentLevel;

public:
    static void init(const std::string& filename = "scheduler.log", LogLevel level = LogLevel::INFO) {
        std::lock_guard<std::mutex> lock(logMutex);
        currentLevel = level;
        if (!logFile.is_open()) {
            logFile.open(filename, std::ios::app);
        }
    }

    static void log(LogLevel level, const std::string& format, ...) {
        if (level < currentLevel) return;

        std::lock_guard<std::mutex> lock(logMutex);

        const char* levelStr[] = {"DEBUG", "INFO", "WARN", "ERROR"};
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);

        std::cout << "[" << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S")
                  << "] [" << levelStr[static_cast<int>(level)] << "] ";

        if (logFile.is_open()) {
            logFile << "[" << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S")
                    << "] [" << levelStr[static_cast<int>(level)] << "] ";
        }

        va_list args;
        va_start(args, format);

        char buffer[1024];
        vsnprintf(buffer, sizeof(buffer), format.c_str(), args);

        std::cout << buffer << std::endl;

        if (logFile.is_open()) {
            logFile << buffer << std::endl;
            logFile.flush();
        }

        va_end(args);
    }

    static void set_log_level(LogLevel level) {
        std::lock_guard<std::mutex> lock(logMutex);
        currentLevel = level;
    }

    static void close() {
        std::lock_guard<std::mutex> lock(logMutex);
        if (logFile.is_open()) {
            logFile.close();
        }
    }
};

std::mutex Logger::logMutex;
std::ofstream Logger::logFile;
LogLevel Logger::currentLevel = LogLevel::INFO;
