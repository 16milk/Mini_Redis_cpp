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
        fail(std::string("failed to write socket data: ") + std::strerror(errno));
    }
}

void drain_nonblocking(int fd, std::string& output) {
    char buffer[4096];
    while (true) {
        const ssize_t read_count = read(fd, buffer, sizeof(buffer));
        if (read_count > 0) {
            output.append(buffer, static_cast<size_t>(read_count));
            continue;
        }
        if (read_count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        if (read_count < 0 && errno == EINTR) {
            continue;
        }
        if (read_count == 0) {
            return;
        }
        fail(std::string("failed to drain socket data: ") + std::strerror(errno));
    }
}

void test_pipelined_requests_and_fragmented_input() {
    int sockets[2];
    expect(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socketpair creation");
    set_nonblocking(sockets[0]);

    Connection connection(sockets[0]);
    const std::string ping = "*1\r\n$4\r\nPING\r\n";
    const std::string get = "*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n";
    write_all(sockets[1], ping + get);
    expect(connection.readFromSocket(), "read pipelined requests");

    RespParser parser;
    std::vector<std::string> arguments;
    size_t consumed = 0;
    expect(parser.parse(connection.getReadBuffer(), arguments, consumed) == RespParser::COMPLETE,
           "parse first pipelined request");
    expect(arguments == std::vector<std::string>({"PING"}), "first request arguments");
    expect(consumed == ping.size(), "first request consumed bytes");

    connection.consumeInput(consumed);
    expect(parser.parse(connection.getReadBuffer(), arguments, consumed) == RespParser::COMPLETE,
           "parse second pipelined request");
    expect(arguments == std::vector<std::string>({"GET", "key"}), "second request arguments");
    expect(consumed == get.size(), "second request consumed bytes");
    connection.consumeInput(consumed);
    expect(connection.getReadBuffer().empty(), "pipelined buffer fully consumed");

    const std::string set = "*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\nb\r\n";
    const size_t split = set.size() - 3;
    write_all(sockets[1], set.substr(0, split));
    expect(connection.readFromSocket(), "read fragmented request prefix");
    consumed = 123;
    expect(parser.parse(connection.getReadBuffer(), arguments, consumed) == RespParser::INCOMPLETE,
           "fragmented request remains incomplete");
    expect(consumed == 0, "incomplete request reports zero consumed bytes");
    expect(connection.getReadBuffer().size() == split, "incomplete bytes remain buffered");

    write_all(sockets[1], set.substr(split));
    expect(connection.readFromSocket(), "read fragmented request suffix");
    expect(parser.parse(connection.getReadBuffer(), arguments, consumed) == RespParser::COMPLETE,
           "parse completed fragmented request");
    expect(arguments == std::vector<std::string>({"SET", "a", "b"}),
           "fragmented request arguments");
    expect(consumed == set.size(), "fragmented request consumed bytes");
    connection.consumeInput(consumed);

    connection.sendResponse("+OK\r\n");
    expect(shutdown(sockets[1], SHUT_WR) == 0, "client half closes write direction");
    expect(connection.readFromSocket(), "observe client half close");
    expect(connection.peerReadClosed(), "peer half close is retained until response flush");
    expect(connection.writeToSocket(), "write response after peer half close");

    char response[5];
    expect(read(sockets[1], response, sizeof(response)) == static_cast<ssize_t>(sizeof(response)),
           "read response after peer half close");
    expect(std::string(response, sizeof(response)) == "+OK\r\n",
           "response survives peer half close");

    close(sockets[1]);
}

void test_partial_write_is_retained_until_drained() {
    int sockets[2];
    expect(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socketpair creation");
    set_nonblocking(sockets[0]);
    set_nonblocking(sockets[1]);

    int send_buffer_size = 4096;
    expect(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &send_buffer_size,
                      sizeof(send_buffer_size)) == 0,
           "set small send buffer");

    Connection connection(sockets[0]);
    const std::string payload(1024 * 1024, 'x');
    connection.sendResponse(payload);
    expect(connection.writeToSocket(), "initial nonblocking write");
    expect(connection.hasPendingWrite(), "partial write leaves pending output");

    std::string received;
    for (size_t attempts = 0; connection.hasPendingWrite() && attempts < 100000; ++attempts) {
        drain_nonblocking(sockets[1], received);
        expect(connection.writeToSocket(), "resume pending nonblocking write");
    }
    drain_nonblocking(sockets[1], received);

    expect(!connection.hasPendingWrite(), "pending output drains completely");
    expect(received == payload, "partial writes preserve every response byte");

    close(sockets[1]);
}

void test_bounded_read_and_write() {
    int sockets[2];
    expect(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socketpair creation");
    set_nonblocking(sockets[0]);
    set_nonblocking(sockets[1]);

    Connection connection(sockets[0]);
    const std::string input(8 * 1024, 'i');
    write_all(sockets[1], input);
    const auto first_read = connection.readFromSocket(4096, 1);
    expect(first_read.ok && first_read.bytes_read == 4096 &&
               first_read.read_calls == 1 && first_read.budget_exhausted,
           "bounded read stops at its byte and syscall budget");
    expect(connection.pendingInputBytes() == 4096,
           "bounded read retains exactly the admitted input");
    connection.discardInput();

    const std::string output(32 * 1024, 'o');
    const std::string tail = "tail";
    connection.sendResponse(output);
    expect(connection.writeToSocket(4096, 1), "bounded write succeeds");
    expect(connection.hasPendingWrite(), "bounded write leaves output queued");
    connection.sendResponse(tail);

    std::string received;
    drain_nonblocking(sockets[1], received);
    expect(received.size() <= 4096, "bounded write does not exceed byte budget");
    for (size_t attempts = 0; connection.hasPendingWrite() && attempts < 1000; ++attempts) {
        expect(connection.writeToSocket(4096, 1), "resume bounded write");
        drain_nonblocking(sockets[1], received);
    }
    expect(!connection.hasPendingWrite(), "bounded output eventually drains");
    expect(received == output + tail,
           "write offset preserves old and newly appended output bytes");

    close(sockets[1]);
}

} // namespace

int main() {
    test_pipelined_requests_and_fragmented_input();
    test_partial_write_is_retained_until_drained();
    test_bounded_read_and_write();
    std::cout << "network I/O tests passed" << std::endl;
    return EXIT_SUCCESS;
}
