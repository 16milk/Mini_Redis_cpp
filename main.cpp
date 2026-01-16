// main.cpp（更新）
#include "Server.hpp"
#include "Database.hpp"
#include "Command.hpp"
#include <iostream>
#include <csignal>
#include <memory>

// 全局单例（阶段三简单起见，后续可注入）
Database g_db;
std::unique_ptr<CommandHandler> g_cmd_handler;

static volatile sig_atomic_t shutdown_flag = 0;

void signal_handler(int sig) {
    std::cout << "\n[INFO] Received signal " << sig << ", shutting down..." << std::endl;
    shutdown_flag = 1;
    g_db.saveRdb();
    exit(EXIT_SUCCESS);
}

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 初始化命令处理器
    g_cmd_handler = std::make_unique<CommandHandler>(g_db);

    try {
        Server server(6379);
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] Exception: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}