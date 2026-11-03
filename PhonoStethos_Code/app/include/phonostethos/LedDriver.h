#pragma once

#include "phonostethos/RuntimeTypes.h"

namespace phono_app {
class LedDriver {
public:
    explicit LedDriver(const std::string& path);

    bool open();

    bool write(std::uint8_t command);

    bool set_mode(FilterMode mode, bool recording);

    void close();

    ~LedDriver();

private:
    std::string path_;
    int fd_ = -1;
    bool available_ = false;
};
}  // namespace phono_app
