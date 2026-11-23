#include "phonostethos/LedDriver.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

using namespace std;

namespace phono_app {
LedDriver::LedDriver(const std::string& path) : path_(path) {}

LedDriver::~LedDriver() { closeDriver(); }

bool LedDriver::openDriver() {
    /* API: ::open
     * Open the LED character device for writing. Returns a descriptor or -1.
     * O_CLOEXEC prevents a child program from inheriting the descriptor.
     */
    fd_ = ::open(path_.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd_ >= 0) {
        return true;
    }
    cerr << "LED driver " << path_ << ": " << strerror(errno)
              << '\n';
    return false;
}

bool LedDriver::writeByte(std::uint8_t mask) {
    if (fd_ < 0) {
        return false;
    }
    /* API: ::write
     * Copy the requested bytes to the open descriptor. Check the returned count. For the LED device, write exactly one mask byte.
     */
    const auto count = ::write(fd_, &mask, sizeof(mask));
    if (count != static_cast<ssize_t>(sizeof(mask))) {
        cerr << "write " << path_ << ": ";
        if (count < 0) {
            cerr << strerror(errno);
        } else {
            cerr << "short write";
        }
        cerr << '\n';
        return false;
    }
    return true;
}

void LedDriver::closeDriver() {
    if (fd_ >= 0) {
        (void)writeByte(0);
        /* API: ::close
         * Release a descriptor after its owner has finished using it. Set the stored descriptor to -1 so it cannot be reused accidentally.
         */
        ::close(fd_);
        fd_ = -1;
    }
}
}  // namespace phono_app
