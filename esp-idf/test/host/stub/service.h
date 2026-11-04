/* Host stand-in for spangap-core's service.h, so host tests can include lxst.h. */
#pragma once

class Service {
public:
    virtual ~Service() {}
    virtual void onStart() {}
    virtual void onInit() {}
};
