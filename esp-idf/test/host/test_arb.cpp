/* Host test for who has the session, room or call (lxst_arb.h).
 *
 *   g++ -std=c++17 -Wall -I../../src test_arb.cpp -o /tmp/test_arb && /tmp/test_arb
 */
#include "lxst_arb.h"

#include <cstdio>
#include <cstring>

using namespace lxsess;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main()
{
    /* Idle; a room alone. */
    {
        Arbiter a;
        CHECK(a.active() == Arbiter::NONE && !std::strcmp(a.state(), "idle") && !a.held());
        bool held = true;
        CHECK(a.roomConnect(&held) && !held);
        CHECK(a.active() == Arbiter::ROOM && !std::strcmp(a.state(), "live"));
        CHECK(!a.roomConnect(&held));                   /* one room at a time */
        a.roomDisconnect();
        CHECK(a.active() == Arbiter::NONE);
        CHECK(a.roomConnect(&held));
    }

    /* A pending call leaves the room alone; live holds it; hang-up resumes it. */
    {
        Arbiter a;
        bool held, hold, resume;
        a.roomConnect(&held);
        CHECK(a.callConnect());
        CHECK(!a.callConnect());                        /* one call at a time */
        CHECK(a.call() && !a.callLive());
        CHECK(a.active() == Arbiter::ROOM && !a.held() && !std::strcmp(a.state(), "live"));
        CHECK(a.goLive(&hold) && hold);
        CHECK(a.active() == Arbiter::CALL && a.held() && !std::strcmp(a.state(), "call"));
        CHECK(!a.goLive(&hold) && !hold);               /* already live */
        CHECK(a.callDisconnect(&resume) && resume);
        CHECK(a.active() == Arbiter::ROOM && !a.held() && !a.call());
    }

    /* A call that never went live: nothing to resume. */
    {
        Arbiter a;
        bool held, resume = true;
        a.roomConnect(&held);
        a.callConnect();
        CHECK(!a.callDisconnect(&resume) && !resume);
        CHECK(a.active() == Arbiter::ROOM);
    }

    /* A live call alone; a room connecting during it is held at once and
     * resumes when the call ends. */
    {
        Arbiter a;
        bool held = false, hold = true, resume;
        CHECK(!a.goLive(&hold) && !hold);               /* no call */
        a.callConnect();
        CHECK(a.goLive(&hold) && !hold);
        CHECK(a.active() == Arbiter::CALL && !a.held() && !std::strcmp(a.state(), "call"));
        CHECK(a.roomConnect(&held) && held && a.held());
        CHECK(a.active() == Arbiter::CALL);
        CHECK(a.callDisconnect(&resume) && resume);
        CHECK(a.active() == Arbiter::ROOM && !std::strcmp(a.state(), "live"));
    }

    /* The held room leaves during the call: nothing resumes. */
    {
        Arbiter a;
        bool held, hold, resume = true;
        a.roomConnect(&held);
        a.callConnect();
        a.goLive(&hold);
        a.roomDisconnect();
        CHECK(!a.held() && a.active() == Arbiter::CALL);
        CHECK(a.callDisconnect(&resume) && !resume);
        CHECK(a.active() == Arbiter::NONE && !std::strcmp(a.state(), "idle"));
        CHECK(a.callConnect());                         /* the next call */
    }

    if (failures) {
        std::printf("test_arb: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_arb: all passed\n");
    return 0;
}
