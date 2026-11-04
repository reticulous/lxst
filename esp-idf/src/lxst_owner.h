/**
 * lxst_owner.h — who owns the session endpoint, and whether the owner's talk
 * key is held. Host-portable.
 *
 * The most recently active surface owns it:
 *   browser channel opens (or a newer one replaces it)   → browser
 *   browser talk key down                                 → browser
 *   device talk key down edge (any device source)         → device
 *   lxst.cmd.claim "device"                               → device
 *   browser channel closes                                → device
 * Key releases never move ownership. Taking ownership releases the other
 * side's held keys, so a key held across a change of owner keys nothing until
 * it is pressed again; the browser's key is also released whenever its
 * channel opens or closes.
 */
#pragma once

#include <cstdint>

namespace lxsess {

enum TalkSrc : uint8_t {
    TALK_DEV_PTT = 0,   /* the board's held talk key            */
    TALK_DEV_SPACE,     /* the board's held space key           */
    TALK_DEV_CMD,       /* lxst.cmd.ptt (the on-device button)  */
    TALK_BROWSER,       /* LXST_DC_PTT on the browser channel   */
    TALK_SRCS
};

class Ownership {
public:
    bool browserOwns() const { return m_browser; }
    bool channel() const { return m_chan; }
    bool held(TalkSrc s) const { return m_down[s]; }

    /* Each event returns true when the owner changed. */
    bool channelUp()
    {
        m_chan = true;
        m_down[TALK_BROWSER] = false;
        return take(true);
    }

    bool channelDown()
    {
        m_chan = false;
        m_down[TALK_BROWSER] = false;
        return take(false);
    }

    bool key(TalkSrc s, bool down)
    {
        if (s >= TALK_SRCS) return false;
        if (s == TALK_BROWSER) {
            if (!m_chan) return false;
            m_down[s] = down;
            return down && take(true);
        }
        bool edge = down && !m_down[s];
        m_down[s] = down;
        return edge && take(false);
    }

    /** lxst.cmd.claim "device": the device takes the endpoint as on a device
     *  key's down edge, without keying anything. */
    bool claimDevice() { return take(false); }

    /** Release every device source (a session starts and ends released). */
    void releaseDevice()
    {
        m_down[TALK_DEV_PTT] = m_down[TALK_DEV_SPACE] = m_down[TALK_DEV_CMD] = false;
    }

    /** The owner's talk key is held. */
    bool keyed() const
    {
        if (m_browser) return m_down[TALK_BROWSER];
        return m_down[TALK_DEV_PTT] || m_down[TALK_DEV_SPACE] || m_down[TALK_DEV_CMD];
    }

private:
    bool m_browser = false;
    bool m_chan = false;
    bool m_down[TALK_SRCS] = {};

    bool take(bool browser)
    {
        if (browser == m_browser) return false;
        m_browser = browser;
        if (browser) releaseDevice();
        else m_down[TALK_BROWSER] = false;
        return true;
    }
};

}  // namespace lxsess
