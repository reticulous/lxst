/**
 * lxst_arb.h — who has the session: the room consumer (port 1) or the call
 * (port 3). Host-portable.
 *
 *   room connects           accepted unless a room is connected; held at once
 *                           when a call is live
 *   call connects           accepted unless a call is connected; pending
 *   call goes live          the call has the session; a connected room is held
 *   call disconnects        a held room resumes
 *   room disconnects        forgotten (a held room leaves nothing to resume)
 *
 * A pending call never touches the room.
 */
#pragma once

namespace lxsess {

class Arbiter {
public:
    enum Who { NONE, ROOM, CALL };

    bool room() const { return m_room; }
    bool call() const { return m_call; }
    bool callLive() const { return m_live; }
    bool held() const { return m_room && m_live; }
    Who  active() const { return m_live ? CALL : m_room ? ROOM : NONE; }

    /** lxst.state */
    const char* state() const { return m_live ? "call" : m_room ? "live" : "idle"; }

    /** False: refused. *heldNow: the room is held from the start. */
    bool roomConnect(bool* heldNow)
    {
        if (m_room) return false;
        m_room = true;
        if (heldNow) *heldNow = m_live;
        return true;
    }

    void roomDisconnect() { m_room = false; }

    bool callConnect()
    {
        if (m_call) return false;
        m_call = true;
        m_live = false;
        return true;
    }

    /** OP_LIVE. True when the call went live now (false: no call, or already
     *  live). *holdRoom: a connected room is to be held. */
    bool goLive(bool* holdRoom)
    {
        if (holdRoom) *holdRoom = false;
        if (!m_call || m_live) return false;
        m_live = true;
        if (holdRoom) *holdRoom = m_room;
        return true;
    }

    /** The call port went away. True when the call had been live.
     *  *resumeRoom: a held room gets the session back. */
    bool callDisconnect(bool* resumeRoom)
    {
        bool was = m_live;
        if (resumeRoom) *resumeRoom = was && m_room;
        m_call = m_live = false;
        return was;
    }

private:
    bool m_room = false, m_call = false, m_live = false;
};

}  // namespace lxsess
