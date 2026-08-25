// utils.hpp
#pragma once
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>

inline void handle_error(const char* msg) {
    std::cerr << "[ERROR] " << msg << ": " << std::strerror(errno) << std::endl;
    exit(EXIT_FAILURE);
}

inline void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) handle_error("fcntl F_GETFL");
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
        handle_error("fcntl F_SETFL O_NONBLOCK");
}
