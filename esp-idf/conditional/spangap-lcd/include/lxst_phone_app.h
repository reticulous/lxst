/**
 * lxst_phone_app.h — the on-device Phone app as a boot-registered Service.
 *
 * LxstPhoneApp is an LcdApp: the straddle's `services:` entry points the
 * generated boot code at this header. Methods are defined in lxst_phone_lcd.cpp,
 * where the file-static UI state lives. Compiled only under
 * conditional/spangap-lcd/.
 *
 * The app is a view over the phone task's storage and a writer of its command
 * keys: our number, a dial field, the call log, the peers heard and the LXMF
 * contacts, and one call screen that follows lxst.phone.state (incoming,
 * outgoing, live, ended). An incoming call raises the app over whatever is on
 * the screen. While a half-duplex call is live on the call screen and no text
 * field has the focus, the app claims the held Space key (lcd_keys.h), so
 * holding Space talks.
 */
#pragma once

#include "lcd_app.h"
#include "lvgl.h"

class LxstPhoneApp : public LcdApp {
public:
    LxstPhoneApp();
    void onCreate(lv_obj_t* root) override;
    /** The call screen goes back to the home screen (the call carries on);
     *  the home screen falls through to Home. */
    bool onBack() override;
    /** Showing the app opens the call screen while a call is up and re-claims
     *  Space there; hiding it releases the claim and lets go of the talk
     *  button. */
    void onShow() override;
    void onHide() override;
    /** Releases the Space claim, lets go of the talk button, nulls every widget
     *  handle (the subscriptions outlive the layer) and deletes pending timers. */
    void onClose() override;

protected:
    /** Subscribes, on the lcd task, to the phone's storage, so an incoming call
     *  raises the app even before it was first opened. */
    void appInit() override;
};
