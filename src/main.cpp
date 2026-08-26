#include "mini_redis/core/Database.hpp"
#include "mini_redis/net/Server.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>

namespace {

volatile std::sig_atomic_t shutdown_flag = 0;

void signal_handler(int) {
    // Assignment to sig_atomic_t is the only operation performed in signal
    // context. Persistence and logging happen after the event loop returns.
    shutdown_flag = 1;
}

} // namespace

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    try {
        Database database;
        Server server(database, 6379, &shutdown_flag);
        server.run();

        if (!database.saveRdb()) {
            std::cerr << "[WARN] Failed to save database during shutdown" << std::endl;
            return EXIT_FAILURE;
        }
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] Exception: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
