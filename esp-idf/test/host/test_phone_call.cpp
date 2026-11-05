/* Host test for the call state machine (lxst_phone_call.h), both sides.
 *
 *   g++ -std=c++17 -Wall -Wextra -I../../src test_phone_call.cpp ../../src/lxst_phone_call.cpp ../../src/lxst_phone_wire.cpp -o /tmp/test_phone_call && /tmp/test_phone_call
 */
#include "lxst_phone_call.h"
#include "lxst_phone_wire.h"

#include <cstdio>
#include <string>

using namespace lxphone;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* The queued actions as one line, e.g. "S4 O40.1 T1". */
static std::string acts(Call& c)
{
    Act a[Call::MAX_ACTS];
    int n = c.take(a, Call::MAX_ACTS);
    std::string s;
    char b[32];
    for (int i = 0; i < n; ++i) {
        if (!s.empty()) s += ' ';
        switch (a[i].k) {
        case A_SIGNAL:
            s += 'S';
            for (int k = 0; k < a[i].n; ++k) {
                std::snprintf(b, sizeof b, k ? ",%x" : "%x", a[i].s[k]);
                s += b;
            }
            break;
        case A_IDENTIFY: s += "I"; break;
        case A_OPEN:     std::snprintf(b, sizeof b, "O%x.%u", a[i].arg & 0xff, a[i].arg >> 8); s += b; break;
        case A_PROFILE:  std::snprintf(b, sizeof b, "P%x.%u", a[i].arg & 0xff, a[i].arg >> 8); s += b; break;
        case A_TONE:     s += "T" + std::to_string(a[i].arg); break;
        case A_LIVE:     s += "L"; break;
        case A_CLOSE:    s += "C"; break;
        case A_TEARDOWN: s += "X" + std::to_string(a[i].arg); break;
        case A_END:      s += std::string("E:") + resultName((Result)a[i].arg); break;
        }
    }
    return s;
}

static void sigs(Call& c, std::initializer_list<uint32_t> l, int64_t now)
{
    uint32_t v[8];
    int n = 0;
    for (uint32_t x : l) v[n++] = x;
    c.signals(v, n, now);
}

#define EXPECT(c, s) do { std::string got = acts(c); if (got != (s)) { \
    std::printf("FAIL %s:%d  got \"%s\" want \"%s\"\n", __FILE__, __LINE__, got.c_str(), (s)); failures++; } } while (0)

static void callerHappy()
{
    Call c;
    c.dial(0);
    CHECK(c.ph == PH_DIAL);
    EXPECT(c, "");
    c.linkActive(1000, 10000);
    CHECK(c.ph == PH_AWAIT);
    sigs(c, { ST_AVAILABLE }, 1100);
    EXPECT(c, "I");
    sigs(c, { ST_AVAILABLE }, 1150);   /* identifies once */
    EXPECT(c, "");
    c.profile = 0x30;                  /* the task's auto choice */
    sigs(c, { ST_RINGING }, 1200);
    CHECK(c.ph == PH_RINGBACK);
    EXPECT(c, "O30.1 T2 S12f,f1");
    sigs(c, { ST_CONNECTING }, 5000);
    CHECK(c.ph == PH_CONNECTING);
    EXPECT(c, "T0 L S6");
    sigs(c, { ST_ESTABLISHED }, 5100);
    CHECK(c.ph == PH_LIVE && c.liveAt == 5100);
    EXPECT(c, "");
    /* a mid-call switch from the peer follows */
    sigs(c, { 0xFF + 0x40, 0xF0 + 2 }, 6000);
    CHECK(c.profile == 0x40 && c.mode == 2);
    EXPECT(c, "P40.1 P40.2");
    /* an unknown profile is ignored */
    sigs(c, { 0xFF + 0x45 }, 6100);
    EXPECT(c, "");
    CHECK(c.hangup(9000));
    EXPECT(c, "X0 C E:answered");
    CHECK(c.ph == PH_NONE);
}

static void callerBusy()
{
    Call c;
    c.dial(0);
    c.linkActive(100, 10000);
    sigs(c, { ST_BUSY }, 200);
    CHECK(c.ph == PH_BUSYTONE);
    EXPECT(c, "X0 O40.1 T3");
    c.tick(200 + BUSY_TONE_MS - 1);
    EXPECT(c, "");
    CHECK(c.deadline() == 200 + BUSY_TONE_MS);
    c.linkClosed(300);                 /* our own teardown coming back */
    EXPECT(c, "");
    c.tick(200 + BUSY_TONE_MS);
    EXPECT(c, "C E:busy");

    Call r;
    r.dial(0);
    r.linkActive(100, 10000);
    sigs(r, { ST_AVAILABLE }, 150);
    sigs(r, { ST_RINGING }, 200);
    acts(r);
    sigs(r, { ST_REJECTED }, 3000);
    EXPECT(r, "X0 T3");
    CHECK(r.hangup(3500));             /* hang-up stops the tone at once */
    EXPECT(r, "C E:rejected");
}

static void callerTimeouts()
{
    Call c;
    c.dial(0);
    c.tick(WAIT_MS - 1);
    EXPECT(c, "");
    c.tick(WAIT_MS);
    EXPECT(c, "X0 E:failed");

    Call a;
    a.dial(0);
    a.linkActive(1000, 10000);
    CHECK(a.deadline() == 11000);
    a.tick(11000);
    EXPECT(a, "X0 E:failed");

    Call r;
    r.dial(0);
    r.linkActive(1000, 10000);
    sigs(r, { ST_RINGING }, 2000);
    acts(r);
    CHECK(r.deadline() == WAIT_MS);
    r.tick(WAIT_MS);
    EXPECT(r, "X0 C E:no answer");

    Call l;                            /* the callee's ring timeout closes the link */
    l.dial(0);
    l.linkActive(1000, 10000);
    sigs(l, { ST_RINGING }, 2000);
    acts(l);
    l.linkClosed(62000);
    EXPECT(l, "C E:no answer");

    Call f;                            /* no path, no link */
    f.dial(0);
    f.linkClosed(30000);
    EXPECT(f, "E:failed");

    Call x;
    x.dial(0);
    x.linkActive(10, 10000);
    sigs(x, { ST_RINGING }, 20);
    acts(x);
    CHECK(x.hangup(30));
    EXPECT(x, "X0 C E:cancelled");
}

static void calleeHappy()
{
    Call c;
    c.ring(500);
    CHECK(c.ph == PH_RINGING && c.dir == DIR_IN);
    EXPECT(c, "O40.1 S4 T1");          /* the session port before RINGING */
    /* the caller's preference; a status code ends the list while ringing */
    sigs(c, { 0xFF + 0x30, 0xF0 + 1, ST_BUSY, 0xF0 + 2 }, 600);
    CHECK(c.profile == 0x30 && c.mode == 1);
    EXPECT(c, "P30.1");
    CHECK(c.answer(3000));
    CHECK(c.ph == PH_LIVE && c.liveAt == 3000);
    EXPECT(c, "S5 T0 L S6");
    CHECK(!c.answer(3100));
    sigs(c, { ST_ESTABLISHED }, 3200); /* the caller's, ignored */
    EXPECT(c, "");
    sigs(c, { 0xF0 + 2 }, 4000);
    EXPECT(c, "P30.2");
    c.linkClosed(9000);
    EXPECT(c, "C E:answered");
}

static void calleeRefusals()
{
    Call r;
    r.ring(100);
    acts(r);
    CHECK(r.hangup(200));
    EXPECT(r, "S1 X1000 C E:declined");

    Call m;
    m.ring(100);
    acts(m);
    CHECK(m.deadline() == 100 + RING_MS);
    m.tick(100 + RING_MS);
    EXPECT(m, "X0 C E:missed");

    Call g;                            /* the caller gave up while ringing */
    g.ring(100);
    acts(g);
    g.linkClosed(5000);
    EXPECT(g, "C E:missed");

    Call f;                            /* the session port refused: BUSY, never RINGING */
    f.ring(100);
    Act a[Call::MAX_ACTS];
    CHECK(f.take(a, 1) == 1 && a[0].k == A_OPEN);
    f.openFailed(100);
    EXPECT(f, "S0 X1000 E:failed");
    CHECK(f.ph == PH_NONE && !f.sessionOpen);

    Call n;
    CHECK(!n.hangup(0));
    CHECK(!n.answer(0));
}

/* A live call has no deadline in either mode: a muted or mic-less peer sends
 * nothing, and only the link closing ends the call. */
static void silence()
{
    Call c;                            /* callee, full duplex */
    c.ring(0);
    c.answer(1000);
    acts(c);
    CHECK(c.ph == PH_LIVE && c.deadline() == 0);
    c.tick(1000 + 3600000);
    EXPECT(c, "");
    CHECK(c.ph == PH_LIVE);
    c.linkClosed(1000 + 3600001);
    EXPECT(c, "C E:answered");
    CHECK(c.ph == PH_NONE);

    Call o;                            /* caller, full duplex */
    o.dial(0);
    o.linkActive(10, 10000);
    sigs(o, { ST_AVAILABLE }, 20);
    sigs(o, { ST_RINGING }, 30);
    sigs(o, { ST_CONNECTING }, 40);
    sigs(o, { ST_ESTABLISHED }, 50);
    acts(o);
    CHECK(o.ph == PH_LIVE && o.deadline() == 0);
    o.tick(50 + 3600000);
    EXPECT(o, "");
    CHECK(o.ph == PH_LIVE);

    Call h;                            /* half duplex */
    h.ring(0);
    sigs(h, { 0xF0 + MODE_HALF_DUPLEX }, 10);
    h.answer(1000);
    acts(h);
    CHECK(h.deadline() == 0);
    h.tick(1000 + 3600000);
    EXPECT(h, "");
    CHECK(h.ph == PH_LIVE);
}

static void refusedLog()
{
    CHECK(refusedResult(true, true) == R_MISSED);    /* busy here: they called, we were busy */
    CHECK(refusedResult(true, false) == R_NONE);     /* blocked or not allowed stay out */
    CHECK(refusedResult(false, false) == R_NONE);
    CHECK(refusedResult(false, true) == R_NONE);     /* not refused at all */
    CHECK(std::string(resultName(refusedResult(true, true))) == "missed");
}

/* A call never runs at 0x70: it runs at 0x80 and says so with
 * PREFERRED_PROFILE 0x80 (0xFF + 0x80 = 0x17f). */
static void noUll()
{
    Call e;                            /* callee: the caller prefers 0x70 */
    e.ring(0);
    acts(e);
    sigs(e, { 0xFF + 0x70, 0xF0 + 1 }, 100);
    CHECK(e.profile == 0x80);
    EXPECT(e, "P80.1");                /* nothing sent while ringing */
    CHECK(e.answer(1000));
    EXPECT(e, "S5 T0 L S6 S17f");      /* the preference right after ESTABLISHED */

    Call g;                            /* callee: 0x70, then 0x40 before answering */
    g.ring(0);
    sigs(g, { 0xFF + 0x70 }, 100);
    sigs(g, { 0xFF + 0x40 }, 200);
    acts(g);
    CHECK(g.answer(1000));
    EXPECT(g, "S5 T0 L S6");
    CHECK(g.profile == 0x40);

    Call o;                            /* caller: our own choice is 0x70 */
    o.dial(0);
    o.linkActive(10, 10000);
    sigs(o, { ST_AVAILABLE }, 20);
    acts(o);
    o.profile = 0x70;
    sigs(o, { ST_RINGING }, 30);
    EXPECT(o, "O80.1 T2 S17f,f1");
    sigs(o, { ST_CONNECTING }, 40);
    sigs(o, { ST_ESTABLISHED }, 50);
    acts(o);
    /* the remote switches mid-call to 0x70: stay at 0x80 and answer */
    sigs(o, { 0xFF + 0x70 }, 60);
    CHECK(o.profile == 0x80);
    EXPECT(o, "S17f");
    sigs(o, { 0xFF + 0x40 }, 70);
    EXPECT(o, "P40.1");
    sigs(o, { 0xFF + 0x70 }, 80);       /* from 0x40: switch to 0x80 and answer */
    CHECK(o.profile == 0x80);
    EXPECT(o, "P80.1 S17f");
}

/* setProfile: the first hop moves the call's profile and mode; the peer
 * hears what changed in one packet, a ringing callee after ESTABLISHED. */
static void hopFit()
{
    const uint8_t F = MODE_FULL_DUPLEX, H = MODE_HALF_DUPLEX;

    Call o;                            /* caller: half duplex before RINGING */
    o.dial(0);
    o.linkActive(10, 10000);
    sigs(o, { ST_AVAILABLE }, 20);
    acts(o);
    o.setProfile(0x10, H);
    EXPECT(o, "");                     /* nothing to tell yet */
    CHECK(o.hopHalf && o.fullOk());
    sigs(o, { ST_RINGING }, 30);
    EXPECT(o, "O10.2 T2 S10f,f2");     /* 0xF2 in the RINGING reply */
    sigs(o, { ST_CONNECTING }, 40);
    sigs(o, { ST_ESTABLISHED }, 50);
    acts(o);
    o.setProfile(0x10, F);             /* the hop carries full duplex again */
    EXPECT(o, "P10.1 Sf1");
    CHECK(!o.hopHalf && o.mode == F);
    o.setProfile(0x10, F);             /* no change: nothing */
    EXPECT(o, "");
    o.setProfile(0x20, H);             /* both change: one packet */
    EXPECT(o, "P20.2 S11f,f2");
    CHECK(o.hopHalf);
    o.setProfile(0x10, H);             /* profile only */
    EXPECT(o, "P10.2 S10f");
    CHECK(o.hopHalf);
    /* the peer prefers a mode itself: no longer half for the hop */
    sigs(o, { 0xF0 + F }, 60);
    EXPECT(o, "P10.1");
    CHECK(!o.hopHalf && o.fullOk());
    sigs(o, { 0xF0 + H }, 70);
    EXPECT(o, "P10.2");
    CHECK(!o.hopHalf && !o.fullOk());  /* the peer chose half duplex */

    Call e;                            /* callee: owed until ESTABLISHED */
    e.ring(0);
    acts(e);
    sigs(e, { 0xFF + 0x40, 0xF0 + F }, 100);
    acts(e);
    e.setProfile(0x10, H);
    EXPECT(e, "P10.2");
    CHECK(e.answer(1000));
    EXPECT(e, "S5 T0 L S6 S10f,f2");
    CHECK(e.mode == H && e.hopHalf);

    Call m;                            /* callee: only the mode owed */
    m.ring(0);
    sigs(m, { 0xFF + 0x10, 0xF0 + F }, 100);
    acts(m);
    m.setProfile(0x10, H);
    CHECK(m.answer(1000));
    EXPECT(m, "P10.2 S5 T0 L S6 Sf2");

    Call d;                            /* the caller asked half duplex itself */
    d.ring(0);
    sigs(d, { 0xFF + 0x10, 0xF0 + H }, 100);
    CHECK(d.mode == H && !d.hopHalf && !d.fullOk());

    Call f;                            /* nothing fits: fail ends the call */
    f.ring(0);
    acts(f);
    CHECK(f.fail(100));
    EXPECT(f, "S0 X1000 C E:failed");
    CHECK(!f.fail(200));
}

/* How long a call's link may hear nothing before a new call takes its place. */
static void deadBound()
{
    CHECK(deadAfterMs(360, 2000) == 360000 + 15000);   /* the 12-min stale case: 375 s */
    CHECK(deadAfterMs(360, 8000) == 360000 + 32000);   /* 4 RTT past 15 s */
    CHECK(deadAfterMs(0, 0) == 375000);                /* unknown: the RNS maximum */
    CHECK(deadAfterMs(5, 30) == SILENT_WARN_MS);       /* never under the warning */
    CHECK(deadAfterMs(60, 250) == SILENT_WARN_MS);
    CHECK(deadAfterMs(103, 500) == 118000);
    CHECK(deadAfterMs(-1, -5) == 375000);
}

int main()
{
    deadBound();
    hopFit();
    noUll();
    callerHappy();
    callerBusy();
    callerTimeouts();
    calleeHappy();
    calleeRefusals();
    silence();
    refusedLog();
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("test_phone_call: all passed\n");
    return 0;
}
