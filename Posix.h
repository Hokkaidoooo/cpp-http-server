#pragma once
#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>

inline std::system_error systemFailure(const char* operation) {
    return std::system_error(errno, std::generic_category(), operation);
}
class UniqueFd {
public:
    explicit UniqueFd(int fd = -1) : fd_(fd) {}
    ~UniqueFd() { if (fd_ >= 0) ::close(fd_); }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) { if (fd_ >= 0) ::close(fd_); fd_ = other.release(); }
        return *this;
    }
    int get() const { return fd_; }
    int release() { const int result = fd_; fd_ = -1; return result; }
private:
    int fd_;
};
