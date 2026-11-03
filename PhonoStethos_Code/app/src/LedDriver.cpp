#include "phonostethos/LedDriver.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

namespace phono_app {
LedDriver::LedDriver(const std::string& path) : path_(path) {}

LedDriver::~LedDriver() { close(); }

bool LedDriver::open() {
    fd_ = ::open(path_.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd_ >= 0) {
        available_ = true;
        return true;
    }
    std::cerr << "LED driver " << path_ << ": " << std::strerror(errno)
              << '\n';
    return false;
}

bool LedDriver::write(std::uint8_t command) {
    if (fd_ < 0) return false;
    const auto count = ::write(fd_, &command, sizeof(command));
    if (count != static_cast<ssize_t>(sizeof(command))) {
        std::cerr << "write " << path_ << ": "
                  << (count < 0 ? std::strerror(errno) : "short write")
                  << '\n';
        return false;
    }
    return true;
}

bool LedDriver::set_mode(FilterMode mode, bool recording) {
    std::uint8_t mask = recording ? 0x08 : 0;
    switch (mode) {
        case FilterMode::General: mask |= 0x04; break;
        case FilterMode::Heart: mask |= 0x01; break;
        case FilterMode::Lung: mask |= 0x02; break;
    }
    return available_ && write(mask);
}

void LedDriver::close() {
    if (fd_ >= 0) {
        (void)write(0);
        ::close(fd_);
        fd_ = -1;
    }
}
}  // namespace phono_app
