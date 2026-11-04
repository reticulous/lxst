/**
 * lxst_endpoint_device_null — the device endpoint on a board without audio
 * hardware, where spangap/audio builds no engine (the CMakeLists picks this file
 * instead of lxst_endpoint_device.cpp). It never opens, so a session the device
 * owns stays silent and a browser that takes the endpoint carries the audio.
 */
#include "lxst_endpoint.h"

namespace {

class NullDeviceEndpoint : public LxstEndpoint {
public:
    bool open(LxstEndpointSink*, int, int) override { return false; }
    void close() override {}
    bool isOpen() const override { return false; }
    void capture(bool) override {}
    bool capturing() const override { return false; }
    bool play(const int16_t*, int) override { return false; }
};

NullDeviceEndpoint s_dev;

} // namespace

LxstEndpoint* lxstDeviceEndpoint() { return &s_dev; }
