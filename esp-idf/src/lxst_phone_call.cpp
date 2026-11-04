/**
 * lxst_phone_call.cpp — the call state machine. See lxst_phone_call.h.
 */
#include "lxst_phone_call.h"
#include "lxst_phone_wire.h"

namespace lxphone {

const char* resultName(Result r)
{
    switch (r) {
    case R_ANSWERED:  return "answered";
    case R_MISSED:    return "missed";
    case R_DECLINED:  return "declined";
    case R_REJECTED:  return "rejected";
    case R_BUSY:      return "busy";
    case R_NO_ANSWER: return "no answer";
    case R_FAILED:    return "failed";
    case R_CANCELLED: return "cancelled";
    default:          return "";
    }
}

Result refusedResult(bool callUp, bool allowed)
{
    return callUp && allowed ? R_MISSED : R_NONE;
}

void Call::push(const Act& a)
{
    if (nActs < MAX_ACTS) acts[nActs++] = a;
}

void Call::sig1(uint16_t s)
{
    Act a{ A_SIGNAL };
    a.n = 1;
    a.s[0] = s;
    push(a);
}

void Call::sig2(uint16_t x, uint16_t y)
{
    Act a{ A_SIGNAL };
    a.n = 2;
    a.s[0] = x;
    a.s[1] = y;
    push(a);
}

void Call::act(ActKind k, uint32_t arg)
{
    Act a{ k };
    a.arg = arg;
    push(a);
}

int Call::take(Act* out, int cap)
{
    int n = nActs < cap ? nActs : cap;
    for (int i = 0; i < n; ++i) out[i] = acts[i];
    for (int i = n; i < nActs; ++i) acts[i - n] = acts[i];
    nActs -= n;
    return n;
}

void Call::end(Result r, bool teardown, uint32_t delayMs)
{
    if (teardown) act(A_TEARDOWN, delayMs);
    if (sessionOpen) act(A_CLOSE);
    sessionOpen = false;
    result = r;
    act(A_END, r);
    ph = PH_NONE;
}

void Call::busyTone(Result r, int64_t now)
{
    result = r;
    act(A_TEARDOWN, 0);
    if (!sessionOpen) {
        act(A_OPEN, profile | (uint32_t)mode << 8);
        sessionOpen = true;
    }
    act(A_TONE, TONE_BUSY);
    ph = PH_BUSYTONE;
    toneUntil_ = now + BUSY_TONE_MS;
}

void Call::pref(uint32_t s)
{
    uint32_t v = sigValue(s);
    if (sigKind(s) == SIG_PROFILE) {
        if (!profileInfo(v)) return;
        uint8_t run = runProfile((uint8_t)v);
        /* A profile the node runs otherwise is answered with its own
         * preference: right after ESTABLISHED when ringing here, else now. */
        bool owe = run != v;
        hopWant = 0;
        if (ph == PH_RINGING) profOwed_ = owe;
        if (run != profile) {
            profile = run;
            if (sessionOpen) act(A_PROFILE, profile | (uint32_t)mode << 8);
        }
        if (owe && ph != PH_RINGING) sig1((uint16_t)(PREFERRED_PROFILE + profile));
        return;
    } else {
        if ((v != MODE_FULL_DUPLEX && v != MODE_HALF_DUPLEX) || v == mode) return;
        mode = (uint8_t)v;
        hopHalf = false;
        if (ph == PH_RINGING) modeOwed_ = false;
    }
    if (sessionOpen) act(A_PROFILE, profile | (uint32_t)mode << 8);
}

void Call::dial(int64_t now)
{
    dir = DIR_OUT;
    ph = PH_DIAL;
    profile = DEFAULT_PROFILE;
    mode = MODE_FULL_DUPLEX;
    sessionOpen = identifySent = profOwed_ = modeOwed_ = hopHalf = false;
    hopWant = 0;
    result = R_NONE;
    liveAt = -1;
    waitBy_ = now + WAIT_MS;
    ringBy_ = toneUntil_ = 0;
}

void Call::linkActive(int64_t now, int32_t ringBoundMs)
{
    if (ph != PH_DIAL) return;
    ph = PH_AWAIT;
    ringBy_ = now + ringBoundMs;
}

void Call::ring(int64_t now)
{
    dir = DIR_IN;
    ph = PH_RINGING;
    profile = DEFAULT_PROFILE;
    mode = MODE_FULL_DUPLEX;
    sessionOpen = identifySent = profOwed_ = modeOwed_ = hopHalf = false;
    hopWant = 0;
    result = R_NONE;
    liveAt = -1;
    waitBy_ = toneUntil_ = 0;
    ringBy_ = now + RING_MS;
    act(A_OPEN, profile | (uint32_t)mode << 8);
    sessionOpen = true;
    sig1(ST_RINGING);
    act(A_TONE, TONE_RING);
}

void Call::openFailed(int64_t now)
{
    nActs = 0;
    sessionOpen = false;
    if (dir == DIR_IN && ph == PH_RINGING) {
        sig1(ST_BUSY);
        end(R_FAILED, true, SIGNAL_GRACE_MS);
        return;
    }
    hangup(now);
}

void Call::signals(const uint32_t* s, int n, int64_t now)
{
    for (int i = 0; i < n; ++i) {
        if (ph == PH_NONE || ph == PH_DIAL || ph == PH_BUSYTONE) return;
        uint32_t sg = s[i];
        if (sigKind(sg) != SIG_STATUS) {
            pref(sg);
            continue;
        }
        if (dir == DIR_IN) {
            if (ph == PH_RINGING) return;   /* until answered, a status code ends the list */
            if (sg == ST_BUSY || sg == ST_REJECTED) { end(R_ANSWERED, true, 0); return; }
            continue;
        }
        switch (sg) {
        case ST_BUSY:
        case ST_REJECTED:
            if (ph == PH_LIVE) { end(R_ANSWERED, true, 0); return; }
            busyTone(sg == ST_BUSY ? R_BUSY : R_REJECTED, now);
            return;
        case ST_AVAILABLE:
            if (ph == PH_AWAIT && !identifySent) {
                identifySent = true;
                act(A_IDENTIFY);
            }
            break;
        case ST_RINGING:
            if (ph == PH_AWAIT) {
                ph = PH_RINGBACK;
                profile = runProfile(profile);
                act(A_OPEN, profile | (uint32_t)mode << 8);
                sessionOpen = true;
                act(A_TONE, TONE_RINGBACK);
                sig2((uint16_t)(PREFERRED_PROFILE + profile), (uint16_t)(PREFERRED_MODE + mode));
            }
            break;
        case ST_CONNECTING:
            if (ph == PH_AWAIT || ph == PH_RINGBACK) {
                if (!sessionOpen) {
                    act(A_OPEN, profile | (uint32_t)mode << 8);
                    sessionOpen = true;
                }
                act(A_TONE, TONE_NONE);
                act(A_LIVE);
                sig1(ST_ESTABLISHED);
                ph = PH_CONNECTING;
            }
            break;
        case ST_ESTABLISHED:
            if (ph == PH_CONNECTING) {
                ph = PH_LIVE;
                liveAt = now;
            }
            break;
        default:
            break;
        }
    }
}

bool Call::answer(int64_t now)
{
    if (ph != PH_RINGING) return false;
    sig1(ST_CONNECTING);
    act(A_TONE, TONE_NONE);
    act(A_LIVE);
    sig1(ST_ESTABLISHED);
    prefs(profOwed_, modeOwed_);
    profOwed_ = modeOwed_ = false;
    ph = PH_LIVE;
    liveAt = now;
    return true;
}

bool Call::hangup(int64_t)
{
    switch (ph) {
    case PH_NONE:
        return false;
    case PH_DIAL:
    case PH_AWAIT:
    case PH_RINGBACK:
    case PH_CONNECTING:
        end(R_CANCELLED, true, 0);
        return true;
    case PH_RINGING:
        sig1(ST_REJECTED);
        end(R_DECLINED, true, SIGNAL_GRACE_MS);
        return true;
    case PH_LIVE:
        end(R_ANSWERED, true, 0);
        return true;
    case PH_BUSYTONE:
        end(result, false, 0);
        return true;
    }
    return false;
}

void Call::prefs(bool prof, bool md)
{
    if (prof && md) sig2((uint16_t)(PREFERRED_PROFILE + profile), (uint16_t)(PREFERRED_MODE + mode));
    else if (prof)  sig1((uint16_t)(PREFERRED_PROFILE + profile));
    else if (md)    sig1((uint16_t)(PREFERRED_MODE + mode));
}

void Call::setProfile(uint8_t id, uint8_t m)
{
    if (!profileInfo(id) || (m != MODE_FULL_DUPLEX && m != MODE_HALF_DUPLEX)) return;
    bool prof = id != profile, md = m != mode;
    if (!prof && !md) return;
    if (md) hopHalf = m == MODE_HALF_DUPLEX;
    profile = id;
    mode = m;
    if (sessionOpen) act(A_PROFILE, profile | (uint32_t)mode << 8);
    switch (ph) {
    case PH_RINGING:
        profOwed_ = profOwed_ || prof;
        modeOwed_ = modeOwed_ || md;
        break;
    case PH_RINGBACK:
    case PH_CONNECTING:
    case PH_LIVE:
        prefs(prof, md);
        break;
    default:
        break;
    }
}

bool Call::fail(int64_t)
{
    switch (ph) {
    case PH_NONE:
    case PH_BUSYTONE:
        return false;
    case PH_RINGING:
        sig1(ST_BUSY);
        end(R_FAILED, true, SIGNAL_GRACE_MS);
        return true;
    default:
        end(R_FAILED, true, 0);
        return true;
    }
}

void Call::linkClosed(int64_t)
{
    switch (ph) {
    case PH_DIAL:
    case PH_AWAIT:
    case PH_CONNECTING: end(R_FAILED, false, 0); break;
    case PH_RINGBACK:   end(R_NO_ANSWER, false, 0); break;
    case PH_LIVE:       end(R_ANSWERED, false, 0); break;
    case PH_RINGING:    end(R_MISSED, false, 0); break;
    default:            break;   /* none, or the busy tone after our own teardown */
    }
}

void Call::tick(int64_t now)
{
    switch (ph) {
    case PH_DIAL:
    case PH_AWAIT:
    case PH_RINGBACK:
    case PH_CONNECTING:
        if (now >= waitBy_) end(ph == PH_RINGBACK ? R_NO_ANSWER : R_FAILED, true, 0);
        else if (ph == PH_AWAIT && now >= ringBy_) end(R_FAILED, true, 0);
        break;
    case PH_RINGING:
        if (now >= ringBy_) end(R_MISSED, true, 0);
        break;
    case PH_BUSYTONE:
        if (now >= toneUntil_) end(result, false, 0);
        break;
    default:
        break;
    }
}

int64_t Call::deadline() const
{
    switch (ph) {
    case PH_DIAL:
    case PH_RINGBACK:
    case PH_CONNECTING: return waitBy_;
    case PH_AWAIT:      return ringBy_ < waitBy_ ? ringBy_ : waitBy_;
    case PH_RINGING:    return ringBy_;
    case PH_BUSYTONE:   return toneUntil_;
    default:            return 0;
    }
}

}  // namespace lxphone
