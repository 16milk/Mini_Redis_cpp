#include "mini_redis/persistence/DurableFile.hpp"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace persistence {
namespace {

int openCloexec(const char* path, int flags, mode_t mode = 0) {
    int fd = -1;
    do {
#ifdef O_CLOEXEC
        fd = mode != 0 ? open(path, flags | O_CLOEXEC, mode)
                       : open(path, flags | O_CLOEXEC);
#else
        fd = mode != 0 ? open(path, flags, mode) : open(path, flags);
#endif
    } while (fd == -1 && errno == EINTR);
    return fd;
}

bool writeAll(int fd, const std::uint8_t* data, std::size_t size, std::string& error,
              const std::string& path) {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t written = write(fd, data + offset, size - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        error = errnoMessage("cannot write temporary file", path);
        return false;
    }
    return true;
}

}  // namespace

std::string errnoMessage(const std::string& operation, const std::string& path) {
    return operation + " '" + path + "': " + std::strerror(errno);
}

std::string parentDirectory(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return ".";
    }
    if (slash == 0) {
        return "/";
    }
    return path.substr(0, slash);
}

bool fsyncFd(int fd, std::string& error) {
    while (fsync(fd) == -1) {
        if (errno == EINTR) {
            continue;
        }
        error = std::string("cannot fsync: ") + std::strerror(errno);
        return false;
    }
    return true;
}

bool fsyncDirectory(const std::string& directory, std::string& error) {
    const int fd = openCloexec(directory.c_str(), O_RDONLY);
    if (fd == -1) {
        error = errnoMessage("cannot open directory for fsync", directory);
        return false;
    }
    const bool ok = fsyncFd(fd, error);
    if (!ok) {
        error = errnoMessage("cannot fsync directory", directory);
    }
    close(fd);
    return ok;
}

bool createDirectories(const std::string& path, std::string& error) {
    if (path.empty() || path == ".") {
        return true;
    }
    std::string current;
    std::size_t index = 0;
    if (path[0] == '/') {
        current = "/";
        index = 1;
    }
    while (index < path.size()) {
        const std::size_t slash = path.find('/', index);
        const std::string part =
            path.substr(index, slash == std::string::npos ? std::string::npos : slash - index);
        if (!part.empty() && part != ".") {
            if (!current.empty() && current.back() != '/') {
                current.push_back('/');
            }
            current += part;
            if (mkdir(current.c_str(), 0755) == -1 && errno != EEXIST) {
                error = errnoMessage("cannot create directory", current);
                return false;
            }
        }
        if (slash == std::string::npos) {
            break;
        }
        index = slash + 1;
    }
    return true;
}

bool fileExists(const std::string& path) {
    return access(path.c_str(), F_OK) == 0;
}

bool directoryExists(const std::string& path) {
    struct stat status {};
    if (stat(path.c_str(), &status) == -1) {
        return false;
    }
    return S_ISDIR(status.st_mode);
}

bool removeFile(const std::string& path, std::string& error) {
    if (unlink(path.c_str()) == -1 && errno != ENOENT) {
        error = errnoMessage("cannot remove file", path);
        return false;
    }
    return true;
}

bool renameReplace(const std::string& from, const std::string& to, std::string& error) {
    if (rename(from.c_str(), to.c_str()) == -1) {
        error = errnoMessage("cannot atomically replace file", to);
        return false;
    }
    if (!fsyncDirectory(parentDirectory(to), error)) {
        return false;
    }
    return true;
}

bool writeFileAtomically(const std::string& path, const std::uint8_t* data, std::size_t size,
                         std::string& error) {
    const std::string directory = parentDirectory(path);
    if (!createDirectories(directory, error)) {
        return false;
    }

    std::string temporary_template = path + ".tmp.XXXXXX";
    std::vector<char> temporary_path(temporary_template.begin(), temporary_template.end());
    temporary_path.push_back('\0');
    const int fd = mkstemp(temporary_path.data());
    const std::string temporary(temporary_path.data());
    if (fd == -1) {
        error = errnoMessage("cannot create temporary file", path);
        return false;
    }
    if (fchmod(fd, 0644) == -1) {
        error = errnoMessage("cannot set permissions on temporary file", temporary);
        close(fd);
        unlink(temporary.c_str());
        return false;
    }

    bool success = writeAll(fd, data, size, error, temporary);
    if (success && !fsyncFd(fd, error)) {
        error = errnoMessage("cannot fsync temporary file", temporary);
        success = false;
    }
    if (close(fd) == -1 && success) {
        error = errnoMessage("cannot close temporary file", temporary);
        success = false;
    }
    if (!success) {
        unlink(temporary.c_str());
        return false;
    }

    if (!renameReplace(temporary, path, error)) {
        unlink(temporary.c_str());
        return false;
    }
    return true;
}

bool writeFileAtomically(const std::string& path, const std::string& data, std::string& error) {
    return writeFileAtomically(path, reinterpret_cast<const std::uint8_t*>(data.data()),
                               data.size(), error);
}

bool writeFileAtomically(const std::string& path, const std::vector<std::uint8_t>& data,
                         std::string& error) {
    return writeFileAtomically(path, data.data(), data.size(), error);
}

bool readFile(const std::string& path, std::string& out, std::size_t max_bytes,
              std::string& error) {
    const int fd = openCloexec(path.c_str(), O_RDONLY);
    if (fd == -1) {
        error = errnoMessage("cannot open file", path);
        return false;
    }
    struct stat status {};
    if (fstat(fd, &status) == -1) {
        error = errnoMessage("cannot stat file", path);
        close(fd);
        return false;
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
        static_cast<std::uint64_t>(status.st_size) > max_bytes) {
        error = "file '" + path + "' is not a regular file within the size limit";
        close(fd);
        return false;
    }
    out.assign(static_cast<std::size_t>(status.st_size), '\0');
    std::size_t offset = 0;
    while (offset < out.size()) {
        const ssize_t n = read(fd, &out[offset], out.size() - offset);
        if (n > 0) {
            offset += static_cast<std::size_t>(n);
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        error = errnoMessage("cannot read file", path);
        close(fd);
        return false;
    }
    close(fd);
    if (offset != out.size()) {
        error = "short read of file '" + path + "'";
        return false;
    }
    return true;
}

std::vector<std::string> listDirectory(const std::string& path, std::string& error) {
    std::vector<std::string> names;
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
        error = errnoMessage("cannot list directory", path);
        return names;
    }
    while (dirent* entry = readdir(dir)) {
        const char* name = entry->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
            continue;
        }
        names.emplace_back(name);
    }
    closedir(dir);
    return names;
}

}  // namespace persistence
