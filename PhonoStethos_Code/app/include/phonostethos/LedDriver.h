#pragma once

#include "phonostethos/RuntimeTypes.h"

using namespace std;

namespace phono_app {
class LedDriver {
public:
    explicit LedDriver(const string& path);

    bool open();

    bool write(uint8_t command);

    bool set_mode(FilterMode mode, bool recording);

    void close();

    ~LedDriver();

private:
    string path_;
    int fd_ = -1;
    bool available_ = false;
};
}  // namespace phono_app
