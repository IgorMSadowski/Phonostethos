#include "phonostethos/LedDriver.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

using namespace std;

namespace phono_app {
LedDriver::LedDriver(const string& path) : path_(path) {}

LedDriver::~LedDriver() { closeDriver(); }

bool LedDriver::openDriver() {
    fd_ = ::open(path_.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd_ >= 0) {
        available_ = true;
        return true;
    }
    cerr << "LED driver " << path_ << ": " << strerror(errno)
              << '\n';
    return false;
}

bool LedDriver::writeByte(uint8_t command) {
    if (fd_ < 0) return false;
    const auto count = ::write(fd_, &command, sizeof(command));
    if (count != static_cast<ssize_t>(sizeof(command))) {
        cerr << "write " << path_ << ": "
                  << (count < 0 ? strerror(errno) : "short write")
                  << '\n';
        return false;
    }
    if (command != 0xff) cached_mask_ = command;
    return true;
}

bool LedDriver::setModeLed(FilterMode mode) {
    uint8_t mask = cached_mask_ & 0x08;
    switch (mode) {
        case FilterMode::General: mask |= 0x04; break;
        case FilterMode::Heart: mask |= 0x01; break;
        case FilterMode::Lung: mask |= 0x02; break;
    }
    return available_ && writeByte(mask);
}

bool LedDriver::setRecordLed(bool recording) {
    const uint8_t mask = recording ? (cached_mask_ | 0x08) : (cached_mask_ & ~0x08);
    return available_ && writeByte(mask);
}

bool LedDriver::triggerErrorPulse() { return available_ && writeByte(0xff); }

void LedDriver::closeDriver() {
    if (fd_ >= 0) {
        (void)writeByte(0);
        ::close(fd_);
        fd_ = -1;
        available_ = false;
    }
}
}  // namespace phono_app
