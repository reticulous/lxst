/* Host test for the endpoint ownership rules (lxst_owner.h).
 *
 *   g++ -std=c++17 -Wall -I../../src test_owner.cpp -o /tmp/test_owner && /tmp/test_owner
 */
#include "lxst_owner.h"

#include <cstdio>

using namespace lxsess;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main()
{
    /* Start: device owns, nothing held, no channel. */
    {
        Ownership o;
        CHECK(!o.browserOwns());
        CHECK(!o.channel());
        CHECK(!o.keyed());
        /* Device keys key the device. */
        CHECK(!o.key(TALK_DEV_PTT, true));
        CHECK(o.keyed());
        CHECK(!o.key(TALK_DEV_PTT, false));
        CHECK(!o.keyed());
        CHECK(!o.key(TALK_DEV_CMD, true));
        CHECK(o.keyed());
        CHECK(!o.key(TALK_DEV_CMD, false));
        /* A browser key with no channel is ignored. */
        CHECK(!o.key(TALK_BROWSER, true));
        CHECK(!o.held(TALK_BROWSER));
        CHECK(!o.browserOwns());
    }

    /* Channel open takes it; close gives it back. */
    {
        Ownership o;
        CHECK(o.channelUp());
        CHECK(o.browserOwns());
        CHECK(o.channel());
        CHECK(!o.keyed());
        CHECK(!o.channelUp());                   /* a replacing channel: still the browser */
        CHECK(o.channelDown());
        CHECK(!o.browserOwns());
        CHECK(!o.channel());
        CHECK(!o.channelDown());
    }

    /* Browser key keys the browser only; never the device. */
    {
        Ownership o;
        o.channelUp();
        CHECK(!o.key(TALK_BROWSER, true));       /* already the owner */
        CHECK(o.keyed());
        CHECK(!o.key(TALK_BROWSER, false));
        CHECK(!o.keyed());
        CHECK(o.browserOwns());                  /* release never moves ownership */
    }

    /* Device key takes it back and clears the browser's held key. */
    {
        Ownership o;
        o.channelUp();
        o.key(TALK_BROWSER, true);
        CHECK(o.key(TALK_DEV_SPACE, true));
        CHECK(!o.browserOwns());
        CHECK(!o.held(TALK_BROWSER));
        CHECK(o.keyed());                        /* the device key that took it */
        CHECK(!o.key(TALK_DEV_SPACE, false));
        CHECK(!o.keyed());
        /* A browser key press takes it again. */
        CHECK(o.key(TALK_BROWSER, true));
        CHECK(o.browserOwns());
        CHECK(o.keyed());
    }

    /* Browser key while a device key is held: the device key is released and
     * keys nothing after the channel closes until it is pressed again. */
    {
        Ownership o;
        o.channelUp();
        o.key(TALK_DEV_PTT, true);               /* device takes it back */
        CHECK(!o.browserOwns());
        CHECK(o.key(TALK_BROWSER, true));
        CHECK(o.browserOwns());
        CHECK(!o.held(TALK_DEV_PTT));
        CHECK(o.channelDown());
        CHECK(!o.browserOwns());
        CHECK(!o.keyed());
        CHECK(!o.key(TALK_DEV_PTT, false));      /* the late release */
        CHECK(!o.key(TALK_DEV_PTT, true));       /* pressed again: keyed, owner unchanged */
        CHECK(o.keyed());
    }

    /* Only a device down edge takes it: a repeated down does not. */
    {
        Ownership o;
        o.key(TALK_DEV_CMD, true);
        o.channelUp();                           /* browser takes it, releases the device key */
        CHECK(o.key(TALK_DEV_CMD, true));        /* recorded as released, so this is an edge */
        CHECK(!o.browserOwns());
        o.key(TALK_BROWSER, true);
        CHECK(o.browserOwns());
        CHECK(!o.key(TALK_DEV_CMD, false));
        CHECK(o.browserOwns());
    }

    /* Channel close while the browser key is held releases it. */
    {
        Ownership o;
        o.channelUp();
        o.key(TALK_BROWSER, true);
        o.channelDown();
        CHECK(!o.held(TALK_BROWSER));
        CHECK(!o.keyed());
        /* A new channel starts released. */
        o.channelUp();
        CHECK(!o.keyed());
    }

    /* A replacing channel releases the old channel's held key. */
    {
        Ownership o;
        o.channelUp();
        o.key(TALK_BROWSER, true);
        CHECK(!o.channelUp());
        CHECK(!o.keyed());
    }

    /* The browser yields: a device key takes it, the browser closes its
     * channel on OWNER 0 (no change: the device already owns), and only a
     * reopened channel takes it back. A browser key with the channel closed
     * does nothing. */
    {
        Ownership o;
        CHECK(o.channelUp());
        CHECK(o.key(TALK_DEV_PTT, true));
        CHECK(!o.browserOwns());
        CHECK(!o.channelDown());
        CHECK(!o.browserOwns());
        CHECK(o.keyed());                        /* the device spurt goes on */
        CHECK(!o.key(TALK_BROWSER, true));
        CHECK(!o.browserOwns());
        CHECK(!o.key(TALK_DEV_PTT, false));
        CHECK(o.channelUp());
        CHECK(o.browserOwns());
        CHECK(!o.keyed());
        CHECK(!o.key(TALK_BROWSER, true));       /* the press that reopened it */
        CHECK(o.keyed());
    }

    /* A channel open while the device holds its key takes the endpoint and
     * releases the device key. */
    {
        Ownership o;
        o.key(TALK_DEV_SPACE, true);
        CHECK(o.channelUp());
        CHECK(!o.held(TALK_DEV_SPACE));
        CHECK(!o.keyed());
    }

    /* releaseDevice leaves ownership and the browser's key alone. */
    {
        Ownership o;
        o.key(TALK_DEV_PTT, true);
        o.releaseDevice();
        CHECK(!o.keyed());
        CHECK(!o.browserOwns());
        o.channelUp();
        o.key(TALK_BROWSER, true);
        o.releaseDevice();
        CHECK(o.keyed());
    }

    /* lxst.cmd.claim "device": takes the endpoint like a device key's down
     * edge, keys nothing, releases the browser's key; idempotent. */
    {
        Ownership o;
        CHECK(!o.claimDevice());                    /* the device has it already */
        o.channelUp();
        o.key(TALK_BROWSER, true);
        CHECK(o.keyed());
        CHECK(o.claimDevice());
        CHECK(!o.browserOwns());
        CHECK(!o.keyed());
        CHECK(!o.held(TALK_BROWSER));
        CHECK(!o.held(TALK_DEV_PTT) && !o.held(TALK_DEV_SPACE) && !o.held(TALK_DEV_CMD));
        CHECK(!o.claimDevice());
        CHECK(o.channel());                         /* the channel stays open */
        /* A device key afterwards keys without a change of owner. */
        CHECK(!o.key(TALK_DEV_CMD, true));
        CHECK(o.keyed());
    }

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_owner: all passed\n");
    return 0;
}
