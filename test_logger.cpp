#include "zaslon_logger.h"
#include <iostream>
#include <thread>
#include <vector>

void worker_thread(int id) {
    ZLOG_INFO("Thread {} is starting work.", id);
    ZLOG_WARN("Thread {} encountered a minor issue: {}", id, "Timeout");
    ZLOG_AUDIT("Thread {} completed security scan. Items scanned: {}", id, id * 100);
}

int main() {
    auto& logger = Zaslon::Foundation::Logger::GetInstance();
    if (!logger.InitializeFileSink(L"zaslon_test.log")) {
        std::cerr << "Failed to initialize file sink!" << std::endl;
        return 1;
    }

    ZLOG_INFO("Logger initialized successfully. Starting test...");

    std::vector<std::thread> threads;
    for (int i = 0; i < 5; ++i) {
        threads.emplace_back(worker_thread, i);
    }

    for (auto& t : threads) {
        t.join();
    }

    ZLOG_CRIT("All threads finished. Shutting down system.");

    std::cout << "Test complete. Check zaslon_test.log" << std::endl;
    return 0;
}
