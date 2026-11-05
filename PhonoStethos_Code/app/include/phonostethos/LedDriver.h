#pragma once

#include "phonostethos/RuntimeTypes.h"

using namespace std;

namespace phono_app {
class LedDriver {
public:
    explicit LedDriver(const string& path);

    bool openDriver();

    bool writeByte(uint8_t command);

    bool setModeLed(FilterMode mode);
    bool setRecordLed(bool recording);
    bool triggerErrorPulse();

    void closeDriver();

    ~LedDriver();

private:
    string path_;
    int fd_ = -1;
    bool available_ = false;
    uint8_t cached_mask_ = 0;
};
}  // namespace phono_app
