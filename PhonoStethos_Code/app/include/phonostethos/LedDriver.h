#pragma once

#include <cstdint>
#include <string>

namespace phono_app {
class LedDriver {
public:
    explicit LedDriver(const std::string& path);

    bool openDriver();
    bool writeByte(std::uint8_t mask);

    void closeDriver();

    ~LedDriver();

private:
    std::string path_;
    int fd_ = -1;
};
}  // namespace phono_app
