#include "mini_redis/command/Command.hpp"
#include "mini_redis/core/Database.hpp"
#include "mini_redis/net/Connection.hpp"
#include "mini_redis/net/Protocol.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

void fail(const std::string& message) {
    std::cerr << "FAILED: " << message << std::endl;
    std::exit(EXIT_FAILURE);
}

void expect(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

void set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        fail(std::string("failed to enable nonblocking mode: ") + std::strerror(errno));
    }
}

void write_all(int fd, const std::string& bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t written = write(fd, bytes.data() + offset, bytes.size() - offset);
        if (written > 0) {
            offset += static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        fail(std::string("failed to write request: ") + std::strerror(errno));
    }
}

std::string read_all_available(int fd) {
    std::string result;
    char buffer[1024];
    while (true) {
        const ssize_t read_count = read(fd, buffer, sizeof(buffer));
        if (read_count > 0) {
            result.append(buffer, static_cast<size_t>(read_count));
            continue;
        }
        if (read_count < 0 && errno == EINTR) {
            continue;
        }
        if (read_count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return result;
        }
        if (read_count == 0) {
            return result;
        }
        fail(std::string("failed to read response: ") + std::strerror(errno));
    }
}

void process_all_complete_requests(Connection& connection, CommandHandler& commands) {
    RespParser parser;
    while (!connection.getReadBuffer().empty()) {
        std::vector<std::string> arguments;
        size_t consumed = 0;
        const auto result = parser.parse(connection.getReadBuffer(), arguments, consumed);
        if (result == RespParser::INCOMPLETE) {
            return;
        }
        if (result == RespParser::ERROR) {
            connection.sendResponse(RespParser::encodeError("protocol error"));
            connection.consumeInput(connection.getReadBuffer().size());
            return;
        }
        connection.sendResponse(commands.execute(arguments));
        connection.consumeInput(consumed);
    }
}

void flush(Connection& connection) {
    for (size_t attempts = 0; connection.hasPendingWrite() && attempts < 1000; ++attempts) {
        expect(connection.writeToSocket(), "flush client response");
    }
    expect(!connection.hasPendingWrite(), "client response fully flushed");
}

} // namespace

int main() {
    int client_a[2];
    int client_b[2];
    expect(socketpair(AF_UNIX, SOCK_STREAM, 0, client_a) == 0, "create client A socketpair");
    expect(socketpair(AF_UNIX, SOCK_STREAM, 0, client_b) == 0, "create client B socketpair");
    set_nonblocking(client_a[0]);
    set_nonblocking(client_a[1]);
    set_nonblocking(client_b[0]);
    set_nonblocking(client_b[1]);

    Connection connection_a(client_a[0]);
    Connection connection_b(client_b[0]);
    Database database(true);
    CommandHandler commands(database);

    const std::string a_pipeline =
        "*3\r\n$3\r\nSET\r\n$8\r\nclient:a\r\n$5\r\nalice\r\n"
        "*2\r\n$3\r\nGET\r\n$8\r\nclient:a\r\n";
    const std::string b_pipeline =
        "*3\r\n$3\r\nSET\r\n$8\r\nclient:b\r\n$3\r\nbob\r\n"
        "*2\r\n$3\r\nGET\r\n$8\r\nclient:a\r\n";

    write_all(client_a[1], a_pipeline);
    write_all(client_b[1], b_pipeline);

    expect(connection_b.readFromSocket(), "read client B before client A");
    process_all_complete_requests(connection_b, commands);
    flush(connection_b);

    expect(connection_a.readFromSocket(), "read client A");
    process_all_complete_requests(connection_a, commands);
    flush(connection_a);

    expect(read_all_available(client_b[1]) == "+OK\r\n$-1\r\n",
           "client B receives only its own ordered responses");
    expect(read_all_available(client_a[1]) == "+OK\r\n$5\r\nalice\r\n",
           "client A receives only its own ordered responses");

    const std::string b_reads_a = "*2\r\n$3\r\nGET\r\n$8\r\nclient:a\r\n";
    write_all(client_b[1], b_reads_a);
    expect(connection_b.readFromSocket(), "read later client B request");
    process_all_complete_requests(connection_b, commands);
    flush(connection_b);
    expect(read_all_available(client_b[1]) == "$5\r\nalice\r\n",
           "shared database is visible across clients");

    close(client_a[1]);
    close(client_b[1]);
    std::cout << "multi-client I/O tests passed" << std::endl;
    return EXIT_SUCCESS;
}
