#pragma once

#include <cstdint>

#include <driver/i2c_master.h>

struct Sc7a20Sample {
    int16_t x_mg = 0;
    int16_t y_mg = 0;
    int16_t z_mg = 0;
};

struct Sc7a20Tilt {
    int16_t x_q10 = 0;
    int16_t y_q10 = 0;
    bool valid = false;
};

// Fangtang does not ship the SC7A20 motion path. Keep the Agent UI call sites
// compiling with a no-op service.
class Sc7a20MotionService {
public:
    static Sc7a20MotionService& GetInstance() {
        static Sc7a20MotionService instance;
        return instance;
    }

    bool Start(i2c_master_bus_handle_t) { return false; }
    void SetSuspended(bool) {}
    bool ReadAcceleration(Sc7a20Sample* sample) const {
        if (sample != nullptr) *sample = {};
        return false;
    }
    bool ReadTilt(Sc7a20Tilt* tilt) const {
        if (tilt != nullptr) *tilt = {};
        return false;
    }

private:
    Sc7a20MotionService() = default;
};
