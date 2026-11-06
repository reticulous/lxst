/**
 * lxst_phone — LXST telephony: calls to and from any LXST phone (Sideband,
 * the LXST reference telephone, a Partyline dial-in) on the lxst voice session.
 *
 *   caller                                         callee (lxst.telephony, SINGLE IN)
 *     ── LINK ───────────────────────────────────────▶
 *     ◀─ {0: [AVAILABLE 3]} ───────────────────────────
 *     ── LINKIDENTIFY ───────────────────────────────▶
 *     ◀─ {0: [RINGING 4]}    (or [BUSY 0] + teardown) ─   busy, blocked, not allowed
 *     ── {0: [0xFF + profile, 0xF0 + mode]} ─────────▶
 *     ◀─ {0: [CONNECTING 5]} ──────────────────────────   answered
 *     ── {0: [ESTABLISHED 6]} ───────────────────────▶
 *     ◀─ {0: [ESTABLISHED 6]} ─────────────────────────
 *     ◀▶ {1: frame} · {1: [frame, …]}  each way; we send ceil(60 / frame_ms)
 *     ◀▶ {0: [0xFF + profile]} · {0: [0xF0 + mode]}       mid-call switch
 *     ◀─ {0: [REJECTED 1]} + teardown ─────────────────   declined while ringing
 *        teardown ─────────────────────────────────────   hang-up, ring timeout
 *
 * The lxphone task hosts lxst.telephony on the phone's identity, holds the
 * one call's Link, and drives the session's call port (LXST_PORT_CALL):
 * connected pending when the call rings, LXST_OP_TONE for the ring, ring-back
 * and busy tones, LXST_OP_LIVE when it is answered. Received frames go to the
 * session as speaker 1, unsequenced; each batch the session encodes
 * (framesPerPacket frames) goes out as one packet. Everything rides rnsd.h; storage is the API.
 *
 * Persistent:
 *   s.lxst.phone.enabled    1 (default): announce lxst.telephony and take calls;
 *                           0: neither (outgoing calls still work)
 *   s.lxst.phone.identity   "" (default: the only LXMF identity, else the first,
 *                           else the node's own) · an LXMF slot "0", "1" … · "node"
 *   s.lxst.phone.allow      all (default) · contacts (the LXMF contacts of any
 *                           identity) · none
 *   s.lxst.phone.blocked    space-separated 32-hex identity hashes: always BUSY
 *   s.lxst.phone.profile    auto (default) · ulbw · vlbw · lbw · mq · hq · shq ·
 *                           ull · ll. auto: Codec2 700C when the link's first hop
 *                           is LoRa or its round trip passes 4 s, Codec2 3200
 *                           past 1.5 s, else Opus medium. A callee takes the caller's.
 *   s.lxst.phone.log.<n>.{dir,peer,name,at,dur,result}
 *                           the 30 newest calls, newest = highest n. dir in · out;
 *                           peer the identity hash; at Unix seconds (0: clock
 *                           unset); dur "m:ss" talked ("" never connected); result
 *                           answered · missed · declined · rejected · busy ·
 *                           no answer · failed · cancelled
 * RAM:
 *   lxst.phone.state        off · idle · dialling · ringback · ringing ·
 *                           connecting · live · ended (3 s, 10 s with
 *                           ptt_redial, then idle)
 *   lxst.phone.dir / .peer / .peer_dest / .peer_name   the current or last call
 *   lxst.phone.status       one finished line ("Calling Alice…", "Incoming call" …)
 *   lxst.phone.timer        "m:ss" since the call went live, each second; "" else
 *   lxst.phone.muted        1 while our microphone is muted for the call
 *   lxst.phone.profile      the call's profile ("Opus medium, 60 ms"); "" no call
 *   lxst.phone.mode         full duplex · half duplex; "" no call
 *   lxst.phone.ptt_redial   1 while ended after an outgoing call the route
 *                           carried only half duplex for: a push-to-talk call
 *                           to the same peer would fit
 *   lxst.phone.test_hop_bps written by a tester: nonzero stands in for the
 *                           call link's first-hop bitrate (0, unset: off)
 *   lxst.phone.dest / .ident  our lxst.telephony destination and identity hash
 *   lxst.phone.peers.<dest>.{ident,name,hops,heard}
 *                           phones heard announcing (the newest 32; heard Unix s)
 * Commands (consumed; a refusal's reason lands on lxst.phone.cmd.error, which
 * the next accepted command clears):
 *   lxst.phone.cmd          call <hash> · call <hash> ptt (push to talk: half
 *                           duplex throughout) · answer · reject · hangup ·
 *                           mute 1 · mute 0 · announce. <hash>: an
 *                           lxst.telephony destination, an identity, or an
 *                           LXMF delivery destination (spaces ignored, 64 hex
 *                           cut to 32).
 */
#pragma once

#include <cstdint>
#include "service.h"

/** The lxphone task's ITS name. */
#define LXST_PHONE_TASK "lxphone"

/** Where rnsd connects the Links that come in for lxst.telephony (a server
 *  port of the lxphone task; the connect payload is rnsd_link_incoming_t). */
constexpr uint16_t LXST_PHONE_LINK_PORT = 1;

/** Bring-up service: seeds s.lxst.phone.* defaults, registers the `phone` CLI
 *  verb and hands the lxphone task to the RNS lifecycle. */
class LxstPhoneService : public Service {
public:
    void onInit() override;
};
