#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace persistence {

// Same-directory durable replace: write temp, fsync file, atomic rename, fsync
// parent directory. A failed write must not clobber the previous file.
bool writeFileAtomically(const std::string& path, const std::uint8_t* data,
                         std::size_t size, std::string& error);
bool writeFileAtomically(const std::string& path, const std::string& data,
                         std::string& error);
bool writeFileAtomically(const std::string& path,
                         const std::vector<std::uint8_t>& data, std::string& error);

bool readFile(const std::string& path, std::string& out, std::size_t max_bytes,
              std::string& error);

bool fsyncFd(int fd, std::string& error);
bool fsyncDirectory(const std::string& directory, std::string& error);
bool createDirectories(const std::string& path, std::string& error);
bool removeFile(const std::string& path, std::string& error);
bool directoryExists(const std::string& path);
bool fileExists(const std::string& path);
bool renameReplace(const std::string& from, const std::string& to, std::string& error);

std::string parentDirectory(const std::string& path);
std::vector<std::string> listDirectory(const std::string& path, std::string& error);

std::string errnoMessage(const std::string& operation, const std::string& path);

}  // namespace persistence
