/**
 * lxst_phone_call.h — one LXST telephony call as a state machine, either side.
 *
 * Pure: the task feeds it events with the time (ms, any monotonic clock) and
 * carries out the actions it queues, in order. It holds no handles and does
 * no I/O, so the host tests drive it directly.
 *
 *   caller: dial → linkActive → [AVAILABLE] identify → [RINGING] open + ring-back
 *           + prefs → [CONNECTING] live + ESTABLISHED → [ESTABLISHED] talking
 *   callee: ring (RINGING + ring tone) → answer: CONNECTING, live, ESTABLISHED
 *
 * A callee's links before identification (AVAILABLE, the identify deadline,
 * BUSY for a refused caller) are the task's: several may be pending at once,
 * and only the one that identifies first, and is let in, becomes the call.
 *
 * A call never runs at 0x70: it runs at 0x80 instead (runProfile) and tells
 * the peer with PREFERRED_PROFILE 0x80, which the peer follows.
 *
 * Plain C++17 with no platform includes.
 */
#pragma once

#include <cstdint>

#include "lxst_phone_wire.h"

namespace lxphone {

enum Dir : uint8_t { DIR_OUT, DIR_IN };

enum Phase : uint8_t {
    PH_NONE,        /* no call                                                  */
    PH_DIAL,        /* caller: resolving the callee, path, link establishment   */
    PH_AWAIT,       /* caller: link up, waiting for the callee to ring          */
    PH_RINGBACK,    /* caller: the callee is ringing                            */
    PH_RINGING,     /* callee: ringing here                                     */
    PH_CONNECTING,  /* caller: answered, waiting for the callee's ESTABLISHED   */
    PH_LIVE,        /* both talk                                                */
    PH_BUSYTONE,    /* caller: busy or rejected; the busy tone plays, link gone */
};

enum Result : uint8_t {
    R_NONE,         /* nothing to log */
    R_ANSWERED, R_MISSED, R_DECLINED, R_REJECTED, R_BUSY, R_NO_ANSWER, R_FAILED, R_CANCELLED,
};

/** The call-log word for a result ("answered" …), "" for R_NONE. */
const char* resultName(Result r);

/** What the call log records for an identified caller turned away with BUSY:
 *  R_MISSED when an allowed caller found a call up, R_NONE (no entry) for a
 *  blocked or not-allowed one. */
Result refusedResult(bool callUp, bool allowed);

enum ActKind : uint8_t {
    A_SIGNAL,       /* send n signals s[0..n) in one packet                     */
    A_IDENTIFY,     /* identify on the link                                     */
    A_OPEN,         /* connect the session's call port (pending) with profile, mode */
    A_PROFILE,      /* the session's call profile changes to profile, mode      */
    A_TONE,         /* tone arg                                                 */
    A_LIVE,         /* the call takes the session                               */
    A_CLOSE,        /* disconnect the session's call port                       */
    A_TEARDOWN,     /* tear the link down after arg ms (0 = now)                */
    A_END,          /* the call is over with `result`                          */
};

/* Session tones (the values of the session's tone opcode). */
enum : uint8_t { TONE_NONE = 0, TONE_RING = 1, TONE_RINGBACK = 2, TONE_BUSY = 3 };

struct Act {
    ActKind  k;
    uint8_t  n = 0;
    uint16_t s[2] = { 0, 0 };
    uint32_t arg = 0;
};

constexpr int32_t RING_MS      = 60000;   /* callee rings this long             */
constexpr int32_t WAIT_MS      = 70000;   /* caller: dial to ESTABLISHED        */
constexpr int32_t IDENTIFY_MS  = 30000;   /* callee: link up to identification (the task's) */
constexpr int32_t BUSY_TONE_MS = 4000;
/* A signal sent right before a teardown needs this long to leave before the
 * link goes. */
constexpr int32_t SIGNAL_GRACE_MS = 1000;

/* A live call whose link has heard nothing (keepalives included) for this long
 * shows "Connection may be lost" with the time since, so the user can hang
 * up. Silence alone never ends a call: a muted or microphone-less end sends
 * no frames. */
constexpr int64_t SILENT_WARN_MS = 90000;

/** How long a live call's link may hear nothing before a new incoming call
 *  takes its place, the old one ending "Connection lost". A live peer is
 *  heard at least once per keepalive interval (an initiator sends one after
 *  that long without sending, a responder answers ours), so one interval
 *  plus max(15 s, 4 RTT) for a late packet, never under SILENT_WARN_MS.
 *  keepaliveS 0 (not known yet) counts as the RNS maximum, 360 s. A
 *  responder cannot probe its initiator, and the link's own stale timer
 *  waits two intervals and a grace (up to ~12 min on a slow path). */
int64_t deadAfterMs(int keepaliveS, int rttMs);

class Call {
public:
    Dir      dir = DIR_OUT;
    Phase    ph = PH_NONE;
    uint8_t  profile = 0x40;
    uint8_t  mode = 1;
    bool     sessionOpen = false;
    bool     identifySent = false;
    Result   result = R_NONE;
    int64_t  liveAt = -1;      /* when it went live, -1 never */

    /* Queued actions, oldest first; the task takes them with take(). */
    static constexpr int MAX_ACTS = 16;
    Act      acts[MAX_ACTS];
    int      nActs = 0;

    /** Caller: start dialling. */
    void dial(int64_t now);
    /** Caller: the link is up. RINGING must come within ringBoundMs. */
    void linkActive(int64_t now, int32_t ringBoundMs);
    /** Callee: an identified, allowed caller's link becomes the call; it
     *  rings for RING_MS. */
    void ring(int64_t now);
    /** Signals from the peer, in packet order. */
    void signals(const uint32_t* s, int n, int64_t now);
    /** Callee: the user answers. False when not ringing. */
    bool answer(int64_t now);
    /** The user ends the call: reject while ringing here, cancel while
     *  calling, hang up while live. False with no call. */
    bool hangup(int64_t now);
    /** The session's call port refused an A_OPEN. The actions queued after
     *  that A_OPEN are dropped (the task drops those it has already taken)
     *  and replaced: a callee that has not rung yet sends BUSY and fails,
     *  anything else hangs up. */
    void openFailed(int64_t now);
    /** The first hop moves the call to profile `id` (one the node runs) in
     *  mode `m`: the open session follows, and the peer is told what changed
     *  with PREFERRED_PROFILE and PREFERRED_MODE in one packet — now once
     *  the callee rings, after ESTABLISHED when ringing here, and with
     *  RINGING while the caller still waits for it. A move from full to half
     *  duplex sets hopHalf; a move back to full clears it. */
    void setProfile(uint8_t id, uint8_t m);
    /** The call is half duplex only because its first hop carries no
     *  full-duplex profile; cleared when the peer prefers a mode itself. */
    bool hopHalf = false;
    /** The profile a first-hop step-down replaced, 0 for none: the task moves
     *  back to it when the hop carries it again. Cleared when the peer
     *  prefers a profile itself. */
    uint8_t hopWant = 0;
    /** The call may run full duplex: it is, or it is half duplex only
     *  because of its first hop. */
    bool fullOk() const { return mode == MODE_FULL_DUPLEX || hopHalf; }
    /** The task ends the call because the route cannot carry it: R_FAILED and
     *  the link torn down; a callee still ringing tells the caller BUSY
     *  first. False with no call. */
    bool fail(int64_t now);
    /** The link closed, or failed to come up. */
    void linkClosed(int64_t now);
    /** Run the deadlines. */
    void tick(int64_t now);
    /** The next deadline, or 0 with none. */
    int64_t deadline() const;

    /** Move the queued actions out. */
    int take(Act* out, int cap);

private:
    int64_t waitBy_ = 0, ringBy_ = 0, toneUntil_ = 0;
    /* callee: preferences owed to the caller after ESTABLISHED */
    bool    profOwed_ = false, modeOwed_ = false;

    void push(const Act& a);
    void sig1(uint16_t s);
    void sig2(uint16_t a, uint16_t b);
    void act(ActKind k, uint32_t arg = 0);
    void end(Result r, bool teardown, uint32_t delayMs);
    void busyTone(Result r, int64_t now);
    void pref(uint32_t s);
    /** One packet of our current profile and/or mode preference. */
    void prefs(bool prof, bool md);
};

}  // namespace lxphone
