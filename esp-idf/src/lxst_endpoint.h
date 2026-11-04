/**
 * lxst_endpoint.h — where the session's microphone PCM comes from and where its
 * mix goes. Encoding, batching, jitter and mixing see only this interface.
 *
 * All calls and callbacks run on the lxst task. PCM is mono int16 at the rate
 * the endpoint was opened with.
 */
#pragma once

#include <cstdint>

class LxstEndpointSink {
public:
    virtual ~LxstEndpointSink() {}
    /** A block of microphone PCM (any length). */
    virtual void onMic(const int16_t* pcm, int n) = 0;
};

class LxstEndpoint {
public:
    virtual ~LxstEndpoint() {}
    /** Bring up the playback path for a session at the session's sample rate
     *  and playout block. False when it cannot. */
    virtual bool open(LxstEndpointSink* sink, int rate, int blockMs) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    /** Start or stop wanting the microphone. Idempotent; a connection it needs
     *  is made by service(). */
    virtual void capture(bool on) = 0;
    virtual bool capturing() const = 0;
    /** Play one block. False when the endpoint dropped it. Never connects. */
    virtual bool play(const int16_t* pcm, int n) = 0;
    /** Bring back a connection that is down, rate-limited; called from the
     *  session's service loop, never from the playout path. */
    virtual void service() {}
};

/** Largest block either endpoint carries, in samples (the engine's ceiling). */
constexpr int LXST_EP_MAX_BLOCK = 1920;

/** The device's own audio engine (the audio task, port AUDIO_STREAM_PORT). */
LxstEndpoint* lxstDeviceEndpoint();

/** What the browser channel reports to the session. */
class LxstBrowserEvents {
public:
    virtual ~LxstBrowserEvents() {}
    /** A channel connected (up; also when a newer one replaces the older) or
     *  closed. */
    virtual void onBrowserChannel(bool up) = 0;
    /** The browser's talk key. */
    virtual void onBrowserPtt(bool down) = 0;
};

/** The browser on LXST_PORT_BROWSER. lxstBrowserStart opens the port; call it
 *  on the lxst task after itsServerInit. */
LxstEndpoint* lxstBrowserEndpoint();
bool lxstBrowserStart(LxstBrowserEvents* ev);
bool lxstBrowserConnected();
/** Tell the browser who owns the endpoint (sent on change and on connect). */
void lxstBrowserOwner(bool browser);
/** Send pending control messages, and report a replaced channel whose
 *  successor never connected as closed; call after every ITS poll. */
void lxstBrowserFlush();
