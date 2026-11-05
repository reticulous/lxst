/**
 * lxst_phone_lcd.cpp — the on-device Phone app (LVGL).
 *
 *   home ──tap a log / peer / contact row, or dial──▶ (phone dials) ──▶ call
 *    ▲  └──tap the call banner────────────────────────────────────────▶ call
 *    └──Back / the call is over────────────────────────────────────────┘
 *   any app ──lxst.phone.state becomes "ringing"──▶ call (Answer · Reject)
 *
 * Two screens on the one app layer, one shown at a time:
 *   - Home: a banner while a call is up (tap to return to it), our number
 *     (lxst.phone.dest), the dial field and Call, the phone's refusal
 *     (lxst.phone.cmd.error), then the call log newest first (direction and
 *     outcome, tap to call back), the peers heard announcing a phone, and the
 *     LXMF contacts (read from lxmf's own s.lxmf.id.<n>.contacts.*).
 *   - Call: one screen that follows lxst.phone.state. Ringing: the caller,
 *     a pulsing handset, Reject and Answer. Dialling, ring-back, connecting:
 *     the status line and Cancel. Live: the peer, the timer, the profile and
 *     mode, Mute and Hang up, and in half duplex a hold-to-talk bar that
 *     shows the microphone level (lxstMicLevel()) along its foot while
 *     talking and writes lxst.cmd.ptt 1/0 (the board's talk key works everywhere; Space
 *     is claimed here while no text field has focus). Ended: the status for
 *     the phone's three seconds, then home.
 *
 * An incoming call raises the app over whatever is on the screen
 * (lcdShowProgram), waking the screen and closing any dialog first. On the
 * ringing screen Answer has the focus, so Enter answers; Backspace (and Esc
 * where the keyboard sends one) rejects. Back leaves the call screen for home
 * without touching the call.
 *
 * Answer and Call write lxst.cmd.claim = "device" first, so the device's
 * microphone and speaker take the call, then lxst.phone.cmd.
 *
 * Storage is the API. Everything runs on the lcd task: the subscriptions are
 * made from it (appInit hops there), so their callbacks may touch LVGL. They
 * only mark what changed; one debounced timer folds a burst into one refresh.
 * List rows are reconciled, not rebuilt.
 */
#include "lcd.h"
#include "lcd_app.h"
#include "lcd_keys.h"
#include "lxst.h"
#include "lxst_phone_app.h"
#include "storage.h"
#include "esp_timer.h"

#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

const char* const APP_NAME = "Phone";

/* ---- look & feel: partyline's palette, so the voice apps read as one family ---- */

const uint32_t C_BG      = 0x101418;   /* app background */
const uint32_t C_SUB     = 0x9098A0;   /* secondary text */
const uint32_t C_ACCENT  = 0x2563A0;   /* primary buttons, section titles */
const uint32_t C_AMBER   = 0xE8A33A;   /* errors, failed calls */
const uint32_t C_RULE    = 0x232B33;   /* row and bar rules */
const uint32_t C_OK      = 0x36C06A;   /* answer, talking, a live call */
const uint32_t C_OKDN    = 0x24854A;   /* answer, pressed */
const uint32_t C_SEL     = 0x1B2A3C;   /* selected / focused row */
const uint32_t C_BAR     = 0x0C1014;   /* talk bar, idle */
const uint32_t C_FIELD   = 0x2F3B47;   /* field and ghost-button borders */
const uint32_t C_PRES    = 0x6C7783;   /* placeholders */
const uint32_t C_ACT     = 0xCFD8E3;   /* ghost-button text */
const uint32_t C_STOP    = 0xC8423C;   /* reject, hang up, cancel */
const uint32_t C_STOPDN  = 0x8E2C28;   /* the same, pressed */
const uint32_t C_FAIL    = 0xE06060;   /* missed calls */
const uint32_t C_DIALOG  = 0x20262E;   /* toast */
const uint32_t C_TALKRDY = 0x172331;   /* talk bar, ready */
const uint32_t C_TALKDN  = 0x1D4F80;   /* talk bar, held before the session keys */
const uint32_t C_LEVEL   = 0x9FE0B0;   /* the microphone level along the talk bar's foot */
const uint32_t C_RINGBG  = 0x173A24;   /* the call banner while ringing */

const lv_font_t* kFont      = &lv_font_montserrat_12_latin;
const lv_font_t* kBold      = &lv_font_montserrat_12_latin;
const lv_font_t* kSmall     = &lv_font_montserrat_12_latin;
const lv_font_t* kSmallBold = &lv_font_montserrat_12_latin;
const lv_font_t* kBtn       = &lv_font_montserrat_12_latin;
const lv_font_t* kBig       = &lv_font_montserrat_12_latin;
const lv_font_t* kTimer     = &lv_font_montserrat_12_latin;
void refreshFont() {
    kFont      = lcdFont(LcdFace::UI,      lcdPx(14));
    kBold      = lcdFont(LcdFace::UI_BOLD, lcdPx(14));
    kSmall     = lcdFont(LcdFace::UI,      lcdPx(11));
    kSmallBold = lcdFont(LcdFace::UI_BOLD, lcdPx(11));
    kBtn       = lcdFont(LcdFace::UI_BOLD, lcdPx(12));
    kBig       = lcdFont(LcdFace::UI_BOLD, lcdPx(20));
    kTimer     = lcdFont(LcdFace::UI,      lcdPx(18));
}

int hdrH()        { return lcdPx(26); }
int bigIconPx()   { return lcdPx(44); }
int btnIconPx()   { return lcdPx(16); }
int rowIconPx()   { return lcdPx(14); }

const uint32_t DEBOUNCE_MS = 80;       /* storage bursts fold into one refresh */
const uint32_t TOAST_MS    = 3500;
const uint32_t RING_MS     = 350;      /* the ringing handset's pulse */
const int      LXMF_SLOTS  = 10;       /* LXMF identity slots read for contacts */
const size_t   LOG_MAX     = 30;
const size_t   CONTACTS_MAX = 100;

/* In-app icons (assets/lcd-icons, Material Icons). */
const char* const IC_CALL     = "phone-call";
const char* const IC_HANGUP   = "phone-hangup";
const char* const IC_MIC      = "phone-mic";
const char* const IC_MIC_OFF  = "phone-mic-off";
const char* const IC_IN       = "phone-in";
const char* const IC_OUT      = "phone-out";
const char* const IC_MISSED   = "phone-missed";
const char* const IC_MISS_OUT = "phone-missed-out";

/* ---- text ---- */

bool zeroWidth(uint32_t cp) {
    return (cp >= 0x300 && cp <= 0x36F) || (cp >= 0x200B && cp <= 0x200F) ||
           (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0x1F3FB && cp <= 0x1F3FF) ||
           (cp >= 0xE0000 && cp <= 0xE007F);
}

/* Network text made drawable in the UI font: controls and zero-width
 * modifiers dropped, each run of glyphless codepoints shown as one '?',
 * line breaks folded to a space. */
std::string printable(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    bool inRun = false;
    size_t i = 0, n = in.size();
    while (i < n) {
        uint8_t b = (uint8_t)in[i];
        uint32_t cp; size_t len;
        if      (b < 0x80)           { cp = b;        len = 1; }
        else if ((b & 0xE0) == 0xC0) { cp = b & 0x1F; len = 2; }
        else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; len = 3; }
        else if ((b & 0xF8) == 0xF0) { cp = b & 0x07; len = 4; }
        else { i++; continue; }
        if (i + len > n) break;
        bool ok = true;
        for (size_t k = 1; k < len; k++) {
            uint8_t c = (uint8_t)in[i + k];
            if ((c & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (c & 0x3F);
        }
        if (!ok) { i++; continue; }
        size_t at = i;
        i += len;
        if (cp == '\n' || cp == '\r' || cp == '\t') { out += ' '; inRun = false; continue; }
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F) || zeroWidth(cp)) continue;
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(kFont, &g, cp, 0) || g.is_placeholder) {
            if (!inRun) out += '?';
            inRun = true;
            continue;
        }
        inRun = false;
        out.append(in.data() + at, len);
    }
    return out;
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
bool isHex(const std::string& s, size_t len) {
    if (s.size() != len) return false;
    for (char c : s) if (!isxdigit((unsigned char)c)) return false;
    return true;
}
std::string fours(const std::string& hex) {
    std::string out;
    for (size_t i = 0; i < hex.size(); i += 4) {
        if (i) out += ' ';
        out += hex.substr(i, 4);
    }
    return out;
}
/* The head of a hash, as a stand-in for a name nobody announced. */
std::string hashHead(const std::string& hex) {
    return hex.empty() ? std::string("unknown") : fours(hex.substr(0, 8)) + " \xE2\x80\xA6";
}
bool startsWith(const char* s, const char* p) { return strncmp(s, p, strlen(p)) == 0; }
const char* DOT = " \xC2\xB7 ";   /* " · " */

/* A wall clock that has been set (after 2020). */
bool clockSet(time_t t) { return t > 1577836800; }

std::string age(long secs) {
    if (secs < 0) secs = 0;
    char b[16];
    if      (secs < 60)    snprintf(b, sizeof b, "%ld s", secs);
    else if (secs < 3600)  snprintf(b, sizeof b, "%ld min", secs / 60);
    else if (secs < 86400) snprintf(b, sizeof b, "%ld h", secs / 3600);
    else                   snprintf(b, sizeof b, "%ld d", secs / 86400);
    return b;
}

/* When a call was: the time today, else the day. */
std::string whenText(long at) {
    time_t now = time(nullptr);
    if (!at || !clockSet(now)) return "";
    time_t t = (time_t)at;
    struct tm a, b;
    localtime_r(&t, &a);
    localtime_r(&now, &b);
    char out[16];
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) strftime(out, sizeof out, "%H:%M", &a);
    else strftime(out, sizeof out, "%d %b", &a);
    std::string s = out;
    return s[0] == '0' ? s.substr(1) : s;
}

std::string hopsText(int h) { return std::to_string(h) + (h == 1 ? " hop" : " hops"); }

/* "<prefix><id>.<field>", no further dot: the id and the field. */
bool splitRec(const char* key, size_t pfx, std::string& id, const char*& field) {
    if (!key || strlen(key) <= pfx) return false;
    const char* tail = key + pfx;
    const char* dot = strchr(tail, '.');
    if (!dot || dot == tail || strchr(dot + 1, '.')) return false;
    id.assign(tail, dot - tail);
    field = dot + 1;
    return true;
}

/* ---- the phone's state ---- */

std::string phState()  { return storageGetStr("lxst.phone.state", "off"); }
std::string phMode()   { return storageGetStr("lxst.phone.mode", ""); }
bool phMuted()         { return storageGetInt("lxst.phone.muted", 0) != 0; }
/* A call is being set up, ringing or live. */
bool callActive(const std::string& st) {
    return st == "dialling" || st == "ringback" || st == "ringing" || st == "connecting" || st == "live";
}
/* The call screen has something to show. */
bool callShown(const std::string& st) { return callActive(st) || st == "ended"; }
bool halfDuplex() { return phMode() == "half duplex"; }

/* Every command from the device first takes the endpoint for it. */
void phoneCmd(const std::string& cmd, bool claim) {
    if (claim) storageSet("lxst.cmd.claim", "device");
    storageSet("lxst.phone.cmd", cmd.c_str());
}

/* ---- model (lcd task only) ---- */

struct Peer { std::string dest, ident, name; int hops = 0; long heard = 0; };
struct LogRec { long n = 0, at = 0; std::string dir, peer, name, dur, result; };
struct Contact { std::string hash, name; };

std::map<std::string, Peer>   g_peers;
std::map<long, LogRec>        g_log;
std::map<std::string, Contact> g_contacts;

std::map<std::string, Peer>* s_peerAcc = nullptr;
void peerLeaf(const char* key, const char* val) {
    std::string id; const char* f;
    if (!s_peerAcc || !splitRec(key, sizeof("lxst.phone.peers.") - 1, id, f)) return;
    id = lower(id);
    if (!isHex(id, 32)) return;
    Peer& p = (*s_peerAcc)[id];
    p.dest = id;
    std::string v = val ? val : "";
    if      (!strcmp(f, "ident")) p.ident = lower(v);
    else if (!strcmp(f, "name"))  p.name = v;
    else if (!strcmp(f, "hops"))  p.hops = atoi(v.c_str());
    else if (!strcmp(f, "heard")) p.heard = atol(v.c_str());
}
void loadPeers() {
    std::map<std::string, Peer> acc;
    s_peerAcc = &acc;
    storageForEach("lxst.phone.peers.", peerLeaf);
    s_peerAcc = nullptr;
    g_peers.swap(acc);
}

std::map<long, LogRec>* s_logAcc = nullptr;
void logLeaf(const char* key, const char* val) {
    std::string id; const char* f;
    if (!s_logAcc || !splitRec(key, sizeof("s.lxst.phone.log.") - 1, id, f)) return;
    char* end = nullptr;
    long n = strtol(id.c_str(), &end, 10);
    if (end == id.c_str() || *end) return;
    LogRec& r = (*s_logAcc)[n];
    r.n = n;
    std::string v = val ? val : "";
    if      (!strcmp(f, "dir"))    r.dir = v;
    else if (!strcmp(f, "peer"))   r.peer = lower(v);
    else if (!strcmp(f, "name"))   r.name = v;
    else if (!strcmp(f, "at"))     r.at = atol(v.c_str());
    else if (!strcmp(f, "dur"))    r.dur = v;
    else if (!strcmp(f, "result")) r.result = v;
}
void loadLog() {
    std::map<long, LogRec> acc;
    s_logAcc = &acc;
    storageForEach("s.lxst.phone.log.", logLeaf);
    s_logAcc = nullptr;
    g_log.swap(acc);
}

/* LXMF contacts, straight from lxmf's storage: every account's address book
 * (s.lxmf.id.<n>.contacts.<peer>.*), groups and our own addresses left out. */
struct ContactAcc { std::map<std::string, Contact> out; std::map<std::string, std::string> nick, disp; std::set<std::string> groups; };
ContactAcc* s_conAcc = nullptr;
size_t s_conPfx = 0;
void contactLeaf(const char* key, const char* val) {
    std::string id; const char* f;
    if (!s_conAcc || !splitRec(key, s_conPfx, id, f)) return;
    id = lower(id);
    if (!isHex(id, 32)) return;
    std::string v = val ? val : "";
    if      (!strcmp(f, "nick"))         { if (!v.empty()) s_conAcc->nick[id] = v; }
    else if (!strcmp(f, "display_name")) { if (!v.empty()) s_conAcc->disp[id] = v; }
    else if (!strcmp(f, "group"))        { if (atoi(v.c_str())) s_conAcc->groups.insert(id); }
    s_conAcc->out[id].hash = id;
}
void loadContacts() {
    ContactAcc acc;
    std::set<std::string> own;
    std::vector<int> slots;
    for (int n = 0; n < LXMF_SLOTS; n++) {
        char k[48];
        snprintf(k, sizeof k, "lxmf.id.%d.dest_hash", n);
        std::string dest = lower(storageGetStr(k, ""));
        if (!isHex(dest, 32)) continue;
        own.insert(dest);
        snprintf(k, sizeof k, "s.lxmf.id.%d.kind", n);
        if (storageGetStr(k, "") == "group") continue;
        slots.push_back(n);
    }
    s_conAcc = &acc;
    for (int n : slots) {
        char pfx[40];
        snprintf(pfx, sizeof pfx, "s.lxmf.id.%d.contacts.", n);
        s_conPfx = strlen(pfx);
        storageForEach(pfx, contactLeaf);
    }
    s_conAcc = nullptr;
    std::map<std::string, Contact> out;
    for (auto& [h, c] : acc.out) {
        if (acc.groups.count(h) || own.count(h)) continue;
        Contact cc = c;
        auto ni = acc.nick.find(h);
        auto di = acc.disp.find(h);
        cc.name = ni != acc.nick.end() ? ni->second : di != acc.disp.end() ? di->second : "";
        out[h] = cc;
    }
    g_contacts.swap(out);
}

/* The name to show for the call's peer: the phone's, else a heard peer's. */
std::string callPeerName() {
    std::string n = printable(storageGetStr("lxst.phone.peer_name", ""));
    if (!n.empty()) return n;
    std::string peer = lower(storageGetStr("lxst.phone.peer", ""));
    std::string pd = lower(storageGetStr("lxst.phone.peer_dest", ""));
    for (auto& [d, p] : g_peers)
        if ((!pd.empty() && d == pd) || (!peer.empty() && p.ident == peer)) {
            n = printable(p.name);
            if (!n.empty()) return n;
        }
    return "";
}
std::string g_dialTarget;   /* the last hash dialled from here */
std::string callPeerHash() {
    std::string h = lower(storageGetStr("lxst.phone.peer", ""));
    if (h.empty()) h = lower(storageGetStr("lxst.phone.peer_dest", ""));
    if (h.empty() && storageGetStr("lxst.phone.dir", "") == "out") h = g_dialTarget;
    return h;
}

/* ---- widgets (partyline's) ---- */

lv_obj_t* s_layer = nullptr;

lv_obj_t* mkLabel(lv_obj_t* parent, const std::string& txt, uint32_t color, const lv_font_t* f) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f ? f : kFont, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt.c_str());
    return l;
}
lv_obj_t* mkFill(lv_obj_t* row, const std::string& txt, uint32_t color, const lv_font_t* f) {
    lv_obj_t* l = mkLabel(row, txt, color, f);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(l, 1);
    lv_obj_set_flex_grow(l, 1);
    lv_obj_set_height(l, lv_font_get_line_height(f ? f : kFont));
    return l;
}
lv_obj_t* mkWrapFull(lv_obj_t* parent, const std::string& txt, uint32_t color, const lv_font_t* f) {
    lv_obj_t* l = mkLabel(parent, txt, color, f);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    return l;
}
/* A one-line label across its parent, centred, cut with an ellipsis. */
lv_obj_t* mkCentre(lv_obj_t* parent, uint32_t color, const lv_font_t* f) {
    lv_obj_t* l = mkLabel(parent, "", color, f);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}
lv_obj_t* mkBox(lv_obj_t* parent) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    return o;
}
lv_obj_t* mkRowBox(lv_obj_t* parent, int gap) {
    lv_obj_t* o = mkBox(parent);
    lv_obj_set_width(o, lv_pct(100));
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, gap, 0);
    return o;
}
lv_obj_t* mkColBox(lv_obj_t* parent) {
    lv_obj_t* o = mkBox(parent);
    lv_obj_set_width(o, lv_pct(100));
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    return o;
}
lv_obj_t* mkScreen() {
    lv_obj_t* o = lv_obj_create(s_layer);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(o, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}
lv_obj_t* mkBody(lv_obj_t* screen) {
    lv_obj_t* b = lv_obj_create(screen);
    lv_obj_remove_style_all(b);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(b, lcdPx(6), 0);
    lv_obj_set_style_pad_ver(b, lcdPx(4), 0);
    lv_obj_set_style_pad_row(b, lcdPx(2), 0);
    return b;
}
lv_obj_t* mkActionBar(lv_obj_t* screen) {
    lv_obj_t* bar = mkRowBox(screen, lcdPx(8));
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, lcdPx(8), 0);
    lv_obj_set_style_pad_ver(bar, lcdPx(6), 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(C_RULE), 0);
    return bar;
}
void focusRing(lv_obj_t* o) {
    lv_obj_set_style_outline_width(o, lcdPx(2), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(o, lv_color_hex(0x7FA6FF), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(o, 1, LV_STATE_FOCUS_KEY);
}
void showIf(lv_obj_t* o, bool on) {
    if (!o) return;
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}
bool onScreenNow(lv_obj_t* o) {
    for (; o; o = lv_obj_get_parent(o)) if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
    return true;
}

/* ---- in-app icons: rasterized off the lcd task, so an image that is not yet
 * cached is re-sourced by a short settle timer once it lands ---- */

struct IconRef { lv_obj_t* img; const char* base; int px; bool set; };
std::vector<IconRef> g_icons;
lv_timer_t* g_iconTimer = nullptr;
int g_iconTicks = 0;

bool sourceIcon(IconRef& r) {
    const lv_image_dsc_t* dsc = lcdIconDsc(r.base, r.px);
    if (dsc) { lv_image_set_src(r.img, dsc); r.set = true; }
    else lcdIconRequest(r.base, r.px);
    return r.set;
}
void iconTimerCb(lv_timer_t*) {
    bool all = true;
    for (auto& r : g_icons) if (!r.set && !sourceIcon(r)) all = false;
    if (all || ++g_iconTicks >= 50) {   /* ~4 s cap */
        lv_timer_delete(g_iconTimer);
        g_iconTimer = nullptr;
    }
}
void iconSettle() {
    g_iconTicks = 0;
    if (!g_iconTimer) g_iconTimer = lv_timer_create(iconTimerCb, 80, nullptr);
}
void onIconGone(lv_event_t* e) {
    lv_obj_t* o = lv_event_get_target_obj(e);
    g_icons.erase(std::remove_if(g_icons.begin(), g_icons.end(),
                                 [o](const IconRef& r) { return r.img == o; }), g_icons.end());
}
/* Point an icon image at another glyph and tint (the rasters are monochrome). */
void setIcon(lv_obj_t* img, const char* base, uint32_t color) {
    if (!img) return;
    lv_obj_set_style_image_recolor(img, lv_color_hex(color), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    for (auto& r : g_icons) {
        if (r.img != img) continue;
        if (r.base != base) { r.base = base; r.set = false; }
        if (!r.set && !sourceIcon(r)) iconSettle();
        return;
    }
}
lv_obj_t* mkIcon(lv_obj_t* parent, const char* base, int px, uint32_t color) {
    lv_obj_t* img = lv_image_create(parent);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(img, px, px);
    g_icons.push_back({ img, base, px, false });
    lv_obj_add_event_cb(img, onIconGone, LV_EVENT_DELETE, nullptr);
    setIcon(img, base, color);
    return img;
}

enum BtnStyle { BTN_PRIMARY, BTN_GHOST, BTN_GO, BTN_STOP };

lv_obj_t* mkButton(lv_obj_t* parent, const std::string& text, BtnStyle st, lv_event_cb_t cb,
                   lv_obj_t** lblOut = nullptr, const char* icon = nullptr, lv_obj_t** iconOut = nullptr) {
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(b, lcdPx(4), 0);
    lv_obj_set_style_pad_ver(b, lcdPx(4), 0);
    lv_obj_set_style_pad_hor(b, lcdPx(10), 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, lcdPx(5), 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    uint32_t tc = 0xFFFFFF, bg = C_ACCENT, dn = 0x1D4F80;
    if (st == BTN_GO)   { bg = C_OK;   dn = C_OKDN; }
    if (st == BTN_STOP) { bg = C_STOP; dn = C_STOPDN; }
    if (st == BTN_GHOST) {
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(C_FIELD), 0);
        tc = C_ACT; dn = C_SEL;
    } else {
        lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    }
    lv_obj_set_style_bg_color(b, lv_color_hex(dn), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    /* A toggle that is on (Mute) is amber-edged. */
    lv_obj_set_style_border_color(b, lv_color_hex(C_AMBER), LV_STATE_CHECKED);
    focusRing(b);
    if (icon) {
        lv_obj_t* img = mkIcon(b, icon, btnIconPx(), tc);
        if (iconOut) *iconOut = img;
    }
    lv_obj_t* l = mkLabel(b, text, tc, kBtn);
    if (lblOut) *lblOut = l;
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), b);
    return b;
}

lv_obj_t* mkRow(lv_obj_t* parent) {
    lv_obj_t* r = lv_button_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(r, lcdPx(3), 0);
    lv_obj_set_style_pad_hor(r, lcdPx(6), 0);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(C_RULE), 0);
    for (lv_state_t s : { (lv_state_t)LV_STATE_FOCUSED, (lv_state_t)LV_STATE_PRESSED }) {
        lv_obj_set_style_bg_color(r, lv_color_hex(C_SEL), s);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, s);
    }
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), r);
    return r;
}

lv_obj_t* mkSection(lv_obj_t* parent, const std::string& text) {
    lv_obj_t* l = mkLabel(parent, text, C_ACCENT, kSmallBold);
    lv_obj_set_style_pad_top(l, lcdPx(6), 0);
    lv_obj_set_style_pad_left(l, lcdPx(6), 0);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    return l;
}

lv_obj_t* mkField(lv_obj_t* parent, const char* placeholder) {
    lv_obj_t* ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_style_text_font(ta, kFont, 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(ta, lv_color_white(), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(C_PRES), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(C_FIELD), 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(C_ACCENT), LV_STATE_FOCUSED);
    lv_obj_set_style_radius(ta, lcdPx(4), 0);
    lv_obj_set_style_pad_ver(ta, lcdPx(3), 0);
    lv_obj_set_style_pad_hor(ta, lcdPx(6), 0);
    lv_obj_set_width(ta, 1);
    lv_obj_set_flex_grow(ta, 1);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), ta);
    lcdKeyboardAttach(ta);
    return ta;
}

struct Header { lv_obj_t* bar = nullptr; lv_obj_t* back = nullptr; lv_obj_t* title = nullptr; lv_obj_t* right = nullptr; };
Header mkHeader(lv_obj_t* screen, lv_event_cb_t onBack) {
    Header h;
    h.bar = mkRowBox(screen, lcdPx(6));
    lv_obj_set_height(h.bar, hdrH());
    lv_obj_set_style_pad_hor(h.bar, lcdPx(6), 0);
    lv_obj_set_style_border_side(h.bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(h.bar, 1, 0);
    lv_obj_set_style_border_color(h.bar, lv_color_hex(C_RULE), 0);
    if (onBack) {
        h.back = mkLabel(h.bar, LV_SYMBOL_LEFT, C_ACT, kFont);
        lv_obj_add_flag(h.back, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(h.back, lcdPx(12));
        lv_obj_add_event_cb(h.back, onBack, LV_EVENT_CLICKED, nullptr);
        focusRing(h.back);
        if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), h.back);
    }
    h.title = mkFill(h.bar, "", 0xFFFFFF, kBold);
    h.right = mkLabel(h.bar, "", C_SUB, kSmall);
    return h;
}

void onFreeString(lv_event_t* e) { delete static_cast<std::string*>(lv_event_get_user_data(e)); }
void bindString(lv_obj_t* o, lv_event_cb_t cb, const std::string& s) {
    auto* p = new std::string(s);
    lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, p);
    lv_obj_add_event_cb(o, onFreeString, LV_EVENT_DELETE, p);
}
const std::string& boundString(lv_event_t* e) { return *static_cast<std::string*>(lv_event_get_user_data(e)); }

/* ---- deferred focus (partyline's): a tapped row takes the focus on
 * click-release, after our handlers, so focus is placed a beat later. A text
 * field gets it only where the board has keys. ---- */

void updateSpaceClaim();

lv_obj_t* g_focusTarget = nullptr;
lv_timer_t* g_focusTimer = nullptr;
void focusTimerCb(lv_timer_t*) {
    g_focusTimer = nullptr;   /* one-shot: LVGL frees it after this returns */
    if (g_focusTarget && lv_obj_is_valid(g_focusTarget) && onScreenNow(g_focusTarget) && lcdInputGroup())
        lv_group_focus_obj(g_focusTarget);
    g_focusTarget = nullptr;
    updateSpaceClaim();
}
void deferFocus(lv_obj_t* o) {
    if (!o) return;
    if (lcdKeyboardOnScreen() && lv_obj_check_type(o, &lv_textarea_class)) return;
    g_focusTarget = o;
    if (g_focusTimer) return;
    g_focusTimer = lv_timer_create(focusTimerCb, 40, nullptr);
    lv_timer_set_repeat_count(g_focusTimer, 1);
}

/* ---- toast: a self-clearing line at the top, never in the way of a tap ---- */

lv_obj_t*   s_toast = nullptr;
lv_obj_t*   s_toastLbl = nullptr;
lv_timer_t* g_toastTimer = nullptr;

void showToast(const std::string& text, uint32_t color) {
    if (!s_layer || text.empty()) return;
    if (!s_toast) {
        s_toast = lv_obj_create(s_layer);
        lv_obj_remove_style_all(s_toast);
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_FLOATING);
        lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_width(s_toast, lv_pct(90));
        lv_obj_set_height(s_toast, LV_SIZE_CONTENT);
        lv_obj_align(s_toast, LV_ALIGN_TOP_MID, 0, hdrH() + lcdPx(4));
        lv_obj_set_style_bg_color(s_toast, lv_color_hex(C_DIALOG), 0);
        lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_toast, lcdPx(6), 0);
        lv_obj_set_style_pad_all(s_toast, lcdPx(6), 0);
        lv_obj_set_style_border_side(s_toast, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(s_toast, lcdPx(3), 0);
        s_toastLbl = mkWrapFull(s_toast, "", 0xFFFFFF, kFont);
    }
    lv_obj_set_style_border_color(s_toast, lv_color_hex(color), 0);
    lv_label_set_text(s_toastLbl, text.c_str());
    lv_obj_move_foreground(s_toast);
    if (g_toastTimer) lv_timer_delete(g_toastTimer);
    g_toastTimer = lv_timer_create([](lv_timer_t*) {
        g_toastTimer = nullptr;   /* one-shot: LVGL frees it after this returns */
        if (s_toast) { lv_obj_delete(s_toast); s_toast = nullptr; s_toastLbl = nullptr; }
    }, TOAST_MS, nullptr);
    lv_timer_set_repeat_count(g_toastTimer, 1);
}

/* ---- screens ---- */

enum Screen { SC_HOME, SC_CALL };
Screen g_screen = SC_HOME;
bool   g_shown = false;

lv_obj_t* s_home = nullptr;
lv_obj_t* s_call = nullptr;

void refreshHome();
void refreshCall();
void pttRelease();

void stopLevel();

void showScreen(Screen sc) {
    if (g_screen == SC_CALL && sc != SC_CALL) { pttRelease(); stopLevel(); }
    g_screen = sc;
    showIf(s_home, sc == SC_HOME);
    showIf(s_call, sc == SC_CALL);
    if (s_toast) lv_obj_move_foreground(s_toast);
    updateSpaceClaim();
}

/* ---- the Space claim ----
 * Claimed while the call screen is up in the foreground over a live
 * half-duplex call, the screen is awake and no text field holds the focus. */

bool g_spaceClaimed = false;

bool textFocused() {
    lv_group_t* g = lcdInputGroup();
    lv_obj_t* f = g ? lv_group_get_focused(g) : nullptr;
    return f && lv_obj_check_type(f, &lv_textarea_class) && onScreenNow(f);
}

void updateSpaceClaim() {
    bool want = s_layer && g_shown && g_screen == SC_CALL && phState() == "live" && halfDuplex() &&
                storageGetInt("sys.standby", 0) == 0 && !textFocused();
    if (want == g_spaceClaimed) return;
    g_spaceClaimed = want;
    lcdKeyClaim(LCD_KEY_SPACE, want);
}

void releaseSpace() {
    if (!g_spaceClaimed) return;
    g_spaceClaimed = false;
    lcdKeyClaim(LCD_KEY_SPACE, false);
}

/* ---- placing a call ---- */

/* A typed or picked hash as the phone takes it: spaces dropped, lower case,
 * 32 hex (or 64, whose leading half the phone uses). "" if it is neither. */
std::string dialable(const std::string& in) {
    std::string h;
    for (char c : in) if (c != ' ') h += (char)tolower((unsigned char)c);
    return isHex(h, 32) || isHex(h, 64) ? h : "";
}

void placeCall(const std::string& target) {
    std::string h = dialable(target);
    if (h.empty()) { showToast("A number is 32 hexadecimal characters.", C_AMBER); return; }
    g_dialTarget = h.substr(0, 32);
    phoneCmd("call " + h, true);
}

/* ---- home ---- */

Header    g_homeHdr;
lv_obj_t* s_banner = nullptr;
lv_obj_t* s_bannerLbl = nullptr;
lv_obj_t* s_numLbl = nullptr;
lv_obj_t* s_noteLbl = nullptr;
lv_obj_t* s_dialTa = nullptr;
lv_obj_t* s_errLbl = nullptr;
lv_obj_t* s_rows = nullptr;
bool g_homeStale = false;

enum RowKind { RK_SECT, RK_EMPTY, RK_CALL };
struct RowSpec {
    RowKind kind = RK_EMPTY;
    std::string key, sig, target, title, right, sub;
    const char* icon = nullptr;
    uint32_t iconColor = C_SUB, color = 0xFFFFFF;
};
struct ListRow { std::string key, sig; lv_obj_t* obj = nullptr; };
std::vector<ListRow> g_rowsH;

/* Reconcile `list` to `target`: a row whose key and signature still match is
 * kept, a changed one is recreated, a gone one deleted, the rest reordered. */
void applyRows(lv_obj_t* list, std::vector<ListRow>& model, const std::vector<RowSpec>& target,
               lv_obj_t* (*build)(lv_obj_t*, const RowSpec&)) {
    std::map<std::string, size_t> have;
    for (size_t i = 0; i < model.size(); i++) if (model[i].obj) have.emplace(model[i].key, i);
    std::vector<char> used(model.size(), 0);
    std::vector<ListRow> out;
    out.reserve(target.size());
    for (auto& t : target) {
        lv_obj_t* obj = nullptr;
        auto it = have.find(t.key);
        if (it != have.end() && !used[it->second]) {
            used[it->second] = 1;
            ListRow& ex = model[it->second];
            if (ex.sig == t.sig) obj = ex.obj;
            else lv_obj_delete(ex.obj);
        }
        out.push_back({ t.key, t.sig, obj });
    }
    for (size_t i = 0; i < model.size(); i++)
        if (!used[i] && model[i].obj) lv_obj_delete(model[i].obj);
    model.swap(out);
    for (size_t i = 0; i < model.size(); i++)
        if (!model[i].obj) model[i].obj = build(list, target[i]);
    int32_t idx = 0;
    for (auto& r : model) if (r.obj) lv_obj_move_to_index(r.obj, idx++);
}

RowSpec sectSpec(const std::string& t) {
    RowSpec s; s.kind = RK_SECT; s.key = "\x01.s." + t; s.sig = t; s.title = t; return s;
}
RowSpec emptySpec(const std::string& k, const std::string& t) {
    RowSpec s; s.kind = RK_EMPTY; s.key = "\x01.e." + k; s.sig = t; s.title = t; return s;
}

void onCallRow(lv_event_t* e) { placeCall(boundString(e)); }

lv_obj_t* buildHomeRow(lv_obj_t* list, const RowSpec& s) {
    switch (s.kind) {
    case RK_SECT:
        return mkSection(list, s.title);
    case RK_EMPTY: {
        lv_obj_t* l = mkWrapFull(list, s.title, C_SUB, kSmall);
        lv_obj_set_style_pad_hor(l, lcdPx(6), 0);
        lv_obj_set_style_pad_ver(l, lcdPx(3), 0);
        return l;
    }
    default: {
        lv_obj_t* row = mkRow(list);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, lcdPx(6), 0);
        bindString(row, onCallRow, s.target);
        if (s.icon) mkIcon(row, s.icon, rowIconPx(), s.iconColor);
        lv_obj_t* col = mkBox(row);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_width(col, 1);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_t* l1 = mkRowBox(col, lcdPx(6));
        mkFill(l1, s.title, s.color, kFont);
        if (!s.right.empty()) mkLabel(l1, s.right, C_SUB, kSmall);
        if (!s.sub.empty()) {
            lv_obj_t* l2 = mkLabel(col, s.sub, C_SUB, kSmall);
            lv_label_set_long_mode(l2, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_width(l2, lv_pct(100));
            lv_obj_set_height(l2, lv_font_get_line_height(kSmall));
        }
        return row;
    }
    }
}

/* The log's glyph and tint for a call's direction and outcome. */
void logLook(const LogRec& r, const char*& icon, uint32_t& color) {
    bool in = r.dir == "in";
    if (r.result == "answered") { icon = in ? IC_IN : IC_OUT; color = C_OK; return; }
    if (in) { icon = IC_MISSED; color = r.result == "missed" ? C_FAIL : C_SUB; return; }
    icon = IC_MISS_OUT;
    color = r.result == "cancelled" ? C_SUB : C_AMBER;
}

void rebuildRows() {
    if (!s_rows) return;
    g_homeStale = false;
    std::vector<RowSpec> t;

    t.push_back(sectSpec("RECENT"));
    size_t shown = 0;
    for (auto it = g_log.rbegin(); it != g_log.rend() && shown < LOG_MAX; ++it) {
        const LogRec& r = it->second;
        if (!isHex(r.peer, 32)) continue;
        RowSpec s;
        s.kind = RK_CALL;
        s.key = "l." + std::to_string(r.n);
        s.target = r.peer;
        std::string name = printable(r.name);
        s.title = name.empty() ? hashHead(r.peer) : name;
        if (name.empty()) s.color = C_ACT;
        s.right = whenText(r.at);
        s.sub = (r.dir == "in" ? "Incoming" : "Outgoing") + std::string(DOT) + r.result;
        if (!r.dur.empty()) s.sub += DOT + r.dur;
        logLook(r, s.icon, s.iconColor);
        s.sig = s.title + "|" + s.right + "|" + s.sub + "|" + s.icon;
        t.push_back(std::move(s));
        shown++;
    }
    if (!shown) t.push_back(emptySpec("log", "No calls yet."));

    std::vector<const Peer*> peers;
    for (auto& [d, p] : g_peers) peers.push_back(&p);
    std::sort(peers.begin(), peers.end(), [](const Peer* a, const Peer* b) { return a->heard > b->heard; });
    t.push_back(sectSpec("PHONES HEARD"));
    time_t now = time(nullptr);
    for (auto* p : peers) {
        RowSpec s;
        s.kind = RK_CALL;
        s.key = "p." + p->dest;
        s.target = p->dest;
        std::string name = printable(p->name);
        s.title = name.empty() ? hashHead(p->ident.empty() ? p->dest : p->ident) : name;
        if (name.empty()) s.color = C_ACT;
        s.right = hopsText(p->hops);
        if (p->heard && clockSet(now)) s.right += DOT + age((long)now - p->heard);
        s.icon = IC_CALL;
        /* Age alone never churns a row. */
        s.sig = s.title + "|" + std::to_string(p->hops) + "|" + std::to_string(p->heard / 60);
        t.push_back(std::move(s));
    }
    if (peers.empty()) t.push_back(emptySpec("peers", "Phones appear here as they announce."));

    std::vector<const Contact*> cons;
    for (auto& [h, c] : g_contacts) cons.push_back(&c);
    std::sort(cons.begin(), cons.end(), [](const Contact* a, const Contact* b) {
        if (a->name.empty() != b->name.empty()) return b->name.empty();
        return lower(a->name) < lower(b->name);
    });
    if (cons.size() > CONTACTS_MAX) cons.resize(CONTACTS_MAX);
    if (!cons.empty()) t.push_back(sectSpec("LXMF CONTACTS"));
    for (auto* c : cons) {
        RowSpec s;
        s.kind = RK_CALL;
        s.key = "c." + c->hash;
        s.target = c->hash;
        std::string name = printable(c->name);
        s.title = name.empty() ? hashHead(c->hash) : name;
        if (name.empty()) s.color = C_ACT;
        s.right = name.empty() ? "" : fours(c->hash.substr(0, 8));
        s.sig = s.title + "|" + s.right;
        t.push_back(std::move(s));
    }
    applyRows(s_rows, g_rowsH, t, buildHomeRow);
}

/* The banner, our number, the note, the refusal. */
void refreshHome() {
    if (!s_home) return;
    std::string st = phState();
    if (callActive(st) || st == "ended") {
        std::string who = callPeerName();
        if (who.empty()) who = hashHead(callPeerHash());
        std::string text;
        if (st == "ringing") text = "Incoming call" + std::string(DOT) + who + DOT + "tap to answer";
        else if (st == "live") text = who + DOT + storageGetStr("lxst.phone.timer", "") + DOT + "tap to return";
        else text = printable(storageGetStr("lxst.phone.status", "")) + DOT + "tap to return";
        lv_label_set_text(s_bannerLbl, text.c_str());
        lv_obj_set_style_bg_color(s_banner, lv_color_hex(st == "ringing" || st == "live" ? C_RINGBG : C_SEL), 0);
        showIf(s_banner, true);
    } else {
        showIf(s_banner, false);
    }
    std::string dest = lower(storageGetStr("lxst.phone.dest", ""));
    lv_label_set_text(s_numLbl, dest.empty() ? "\xE2\x80\x94" : fours(dest).c_str());

    std::string note;
    if (st == "off") note = "The phone has no identity yet.";
    if (storageGetInt("s.lxst.phone.enabled", 1) == 0) note = "Not taking calls (Settings, Voice). Calls out still work.";
    lv_label_set_text(s_noteLbl, note.c_str());
    showIf(s_noteLbl, !note.empty());

    std::string err = printable(storageGetStr("lxst.phone.cmd.error", ""));
    lv_label_set_text(s_errLbl, err.c_str());
    showIf(s_errLbl, !err.empty());
    lv_label_set_text(g_homeHdr.right, st == "idle" || st == "off" ? "" : st.c_str());
}

void onDial(lv_event_t*) {
    if (!s_dialTa) return;
    std::string h = dialable(lv_textarea_get_text(s_dialTa));
    if (h.empty()) {
        lv_obj_set_style_border_color(s_dialTa, lv_color_hex(C_AMBER), 0);
        showToast("A number is 32 hexadecimal characters.", C_AMBER);
        return;
    }
    lv_textarea_set_text(s_dialTa, "");
    lcdKeyboardClose();
    placeCall(h);
}

void openCall();

void buildHome() {
    s_home = mkScreen();
    g_homeHdr = mkHeader(s_home, nullptr);
    lv_label_set_text(g_homeHdr.title, APP_NAME);
    lv_obj_t* body = mkBody(s_home);
    lv_obj_set_style_pad_hor(body, 0, 0);

    s_banner = lv_button_create(body);
    lv_obj_remove_style_all(s_banner);
    lv_obj_set_size(s_banner, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_banner, lcdPx(6), 0);
    lv_obj_set_flex_flow(s_banner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_banner, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_banner, lcdPx(6), 0);
    lv_obj_remove_flag(s_banner, LV_OBJ_FLAG_SCROLLABLE);
    focusRing(s_banner);
    mkIcon(s_banner, IC_CALL, btnIconPx(), C_OK);
    s_bannerLbl = mkFill(s_banner, "", 0xFFFFFF, kBold);
    lv_obj_add_event_cb(s_banner, [](lv_event_t*) { openCall(); }, LV_EVENT_CLICKED, nullptr);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), s_banner);
    showIf(s_banner, false);

    lv_obj_t* num = mkRowBox(body, lcdPx(6));
    lv_obj_set_style_pad_hor(num, lcdPx(6), 0);
    lv_obj_set_style_pad_top(num, lcdPx(3), 0);
    mkLabel(num, "Your number", C_SUB, kSmall);
    s_numLbl = mkFill(num, "", C_ACT, kSmall);

    s_noteLbl = mkWrapFull(body, "", C_AMBER, kSmall);
    lv_obj_set_style_pad_hor(s_noteLbl, lcdPx(6), 0);

    lv_obj_t* dial = mkRowBox(body, lcdPx(6));
    lv_obj_set_style_pad_hor(dial, lcdPx(6), 0);
    lv_obj_set_style_pad_ver(dial, lcdPx(3), 0);
    s_dialTa = mkField(dial, "Call a number (32 hex)");
    lv_textarea_set_accepted_chars(s_dialTa, "0123456789abcdefABCDEF ");
    lv_textarea_set_max_length(s_dialTa, 80);
    lv_obj_add_event_cb(s_dialTa, onDial, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(s_dialTa, [](lv_event_t*) {
        if (s_dialTa) lv_obj_set_style_border_color(s_dialTa, lv_color_hex(C_FIELD), 0);
    }, LV_EVENT_VALUE_CHANGED, nullptr);
    mkButton(dial, "Call", BTN_GO, onDial, nullptr, IC_CALL);

    s_errLbl = mkWrapFull(body, "", C_AMBER, kSmall);
    lv_obj_set_style_pad_hor(s_errLbl, lcdPx(6), 0);

    s_rows = mkColBox(body);
}

void goHome() {
    showScreen(SC_HOME);
    loadContacts();
    rebuildRows();
    refreshHome();
    deferFocus(s_dialTa);
}

/* ---- the call ---- */

Header    g_callHdr;
lv_obj_t* s_cIcon = nullptr;
lv_obj_t* s_cName = nullptr;
lv_obj_t* s_cHash = nullptr;
lv_obj_t* s_cStatus = nullptr;
lv_obj_t* s_cTimer = nullptr;
lv_obj_t* s_cProf = nullptr;
lv_obj_t* s_talkRow = nullptr;
lv_obj_t* s_talk = nullptr;
lv_obj_t* s_talkLbl = nullptr;
lv_obj_t* s_talkLvl = nullptr;          /* the microphone level along the bar's foot */
lv_timer_t* g_lvlTimer = nullptr;       /* runs only while talking on the call screen */
lv_obj_t* s_bReject = nullptr;
lv_obj_t* s_bAnswer = nullptr;
lv_obj_t* s_bMute = nullptr;
lv_obj_t* s_bMuteLbl = nullptr;
lv_obj_t* s_bMuteIcon = nullptr;
lv_obj_t* s_bHang = nullptr;
lv_obj_t* s_bHangLbl = nullptr;
lv_timer_t* g_ringTimer = nullptr;
int  g_ringPhase = 0;
bool g_pttHeld = false;
bool g_canTalk = false;
std::string g_callShape;    /* which buttons the call screen shows; a change moves the focus */

void pttRelease() {
    if (!g_pttHeld) return;
    g_pttHeld = false;
    storageSet("lxst.cmd.ptt", "0");
}

/* The level fill: lxstMicLevel() as a share of the bar's inner width, inset
 * from the rounded corners. */
void drawLevel() {
    if (!s_talk || !s_talkLvl) return;
    int inset = lcdPx(4);
    int w = lv_obj_get_width(s_talk) - 2 * inset;
    int fill = w > 0 ? w * lxstMicLevel() / 100 : 0;
    lv_obj_set_width(s_talkLvl, fill);
    showIf(s_talkLvl, fill > 0);
}
void stopLevel() {
    if (g_lvlTimer) { lv_timer_delete(g_lvlTimer); g_lvlTimer = nullptr; }
    showIf(s_talkLvl, false);
}
void lvlTimerCb(lv_timer_t*) { drawLevel(); }
/* The meter runs while we transmit and the call screen is in front. */
void levelFollow(bool talking) {
    if (talking && s_layer && g_shown && g_screen == SC_CALL) {
        if (!g_lvlTimer) { g_lvlTimer = lv_timer_create(lvlTimerCb, 60, nullptr); drawLevel(); }
    } else {
        stopLevel();
    }
}

/* What the talk bar says and whether holding it talks. */
void refreshTalk() {
    if (!s_talk || !s_talkLbl) return;
    bool show = phState() == "live" && halfDuplex();
    showIf(s_talkRow, show);
    g_canTalk = false;
    if (!show) { pttRelease(); stopLevel(); return; }
    bool talking = storageGetInt("lxst.talking", 0) != 0;
    std::string text;
    uint32_t bg = C_BAR, fg = C_SUB, border = C_RULE;
    if (phMuted()) {
        text = "Muted" + std::string(DOT) + "unmute to talk";
    } else if (talking) {
        text = LV_SYMBOL_AUDIO "  Talking";
        bg = C_OK; fg = 0x0A0F0A; border = C_OK;
        g_canTalk = true;
    } else {
        bool mic = lcdKeyDeclared(LCD_KEY_PTT), space = lcdKeyDeclared(LCD_KEY_SPACE);
        text = mic && space ? "Hold Mic, Space or here to talk" : mic ? "Hold Mic or here to talk"
             : space ? "Hold Space or here to talk" : "Hold here to talk";
        fg = C_ACT; border = C_ACCENT; bg = C_TALKRDY;
        g_canTalk = true;
    }
    levelFollow(talking && !phMuted());
    if (g_pttHeld && !talking && g_canTalk) bg = C_TALKDN;
    if (!g_canTalk) pttRelease();
    lv_label_set_text(s_talkLbl, text.c_str());
    lv_obj_set_style_text_color(s_talkLbl, lv_color_hex(fg), 0);
    lv_obj_set_style_bg_color(s_talk, lv_color_hex(bg), 0);
    lv_obj_set_style_border_color(s_talk, lv_color_hex(border), 0);
}

void onTalkEvent(lv_event_t* e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) {
        if (!g_canTalk || g_pttHeld) return;
        g_pttHeld = true;
        storageSet("lxst.cmd.ptt", "1");
        refreshTalk();
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        pttRelease();
        refreshTalk();
    }
}

void ringTimerCb(lv_timer_t*) {
    if (!s_cIcon) return;
    g_ringPhase = (g_ringPhase + 1) % 4;
    /* A shake, then a rest: the handset rings in bursts. */
    int dx = g_ringPhase == 0 ? -lcdPx(3) : g_ringPhase == 1 ? lcdPx(3) : 0;
    lv_obj_set_style_translate_x(s_cIcon, dx, 0);
    lv_obj_set_style_image_recolor(s_cIcon, lv_color_hex(g_ringPhase < 2 ? C_OK : 0xFFFFFF), 0);
}
void ringing(bool on) {
    if (on && !g_ringTimer) {
        g_ringPhase = 0;
        g_ringTimer = lv_timer_create(ringTimerCb, RING_MS, nullptr);
    } else if (!on && g_ringTimer) {
        lv_timer_delete(g_ringTimer);
        g_ringTimer = nullptr;
        if (s_cIcon) lv_obj_set_style_translate_x(s_cIcon, 0, 0);
    }
}

void onAnswer(lv_event_t*) { phoneCmd("answer", true); }
void onReject(lv_event_t*) { phoneCmd("reject", false); }
void onHang(lv_event_t*)   { pttRelease(); phoneCmd("hangup", false); }
void onMute(lv_event_t*)   { phoneCmd(phMuted() ? "mute 0" : "mute 1", false); }

/* On the ringing screen Backspace (and a keyboard's Esc) rejects. Run ahead of
 * the shell's Esc-is-Back hook, which then never sees the key. */
void onRingKey(lv_event_t* e) {
    uint32_t k = lv_event_get_key(e);
    if (k != LV_KEY_BACKSPACE && k != LV_KEY_ESC) return;
    if (phState() != "ringing") return;
    lv_event_stop_processing(e);
    phoneCmd("reject", false);
}

void refreshCall() {
    if (!s_call) return;
    std::string st = phState();
    std::string dir = storageGetStr("lxst.phone.dir", "");
    bool live = st == "live", ring = st == "ringing", ended = st == "ended";
    bool setup = callActive(st) && !live && !ring;

    std::string name = callPeerName();
    std::string hash = callPeerHash();
    lv_label_set_text(s_cName, (name.empty() ? (ring ? std::string("Unknown caller") : hashHead(hash)) : name).c_str());
    lv_label_set_text(s_cHash, hash.empty() ? "" : fours(hash).c_str());

    std::string status = printable(storageGetStr("lxst.phone.status", ""));
    if (status.empty()) status = ring ? "Incoming call" : st;
    lv_label_set_text(s_cStatus, status.c_str());
    bool doubt = live && status != "Connected";
    lv_obj_set_style_text_color(s_cStatus, lv_color_hex(doubt ? C_AMBER : live ? C_OK : ended ? C_SUB : 0xFFFFFF), 0);

    std::string timer = storageGetStr("lxst.phone.timer", "");
    lv_label_set_text(s_cTimer, timer.c_str());
    showIf(s_cTimer, live && !timer.empty());

    std::string prof = printable(storageGetStr("lxst.phone.profile", ""));
    std::string mode = phMode();
    if (!mode.empty()) prof += (prof.empty() ? "" : DOT) + mode;
    lv_label_set_text(s_cProf, prof.c_str());
    showIf(s_cProf, live && !prof.empty());

    showIf(s_cIcon, !live);
    setIcon(s_cIcon, ended ? IC_HANGUP : IC_CALL, ended ? C_STOP : ring ? C_OK : 0xFFFFFF);
    ringing(ring);

    lv_label_set_text(g_callHdr.title, ring ? "Incoming call" : dir == "out" ? "Outgoing call" : "Call");

    showIf(s_bReject, ring);
    showIf(s_bAnswer, ring);
    showIf(s_bMute, live);
    showIf(s_bHang, live || setup);
    lv_label_set_text(s_bHangLbl, live ? "Hang up" : "Cancel");
    if (live) {
        bool m = phMuted();
        lv_label_set_text(s_bMuteLbl, m ? "Unmute" : "Mute");
        setIcon(s_bMuteIcon, m ? IC_MIC_OFF : IC_MIC, C_ACT);
        if (m) lv_obj_add_state(s_bMute, LV_STATE_CHECKED); else lv_obj_remove_state(s_bMute, LV_STATE_CHECKED);
    }
    refreshTalk();

    std::string shape = ring ? "ring" : live ? "live" : setup ? "setup" : "over";
    if (shape != g_callShape && g_screen == SC_CALL) {
        g_callShape = shape;
        deferFocus(ring ? s_bAnswer : live ? s_bMute : setup ? s_bHang : g_callHdr.back);
    }
    updateSpaceClaim();
}

void buildCall() {
    s_call = mkScreen();
    g_callHdr = mkHeader(s_call, [](lv_event_t*) { goHome(); });

    lv_obj_t* body = mkBody(s_call);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(body, lcdPx(3), 0);
    s_cIcon = mkIcon(body, IC_CALL, bigIconPx(), 0xFFFFFF);
    s_cName = mkCentre(body, 0xFFFFFF, kBig);
    s_cHash = mkCentre(body, C_SUB, kSmall);
    s_cStatus = mkCentre(body, 0xFFFFFF, kFont);
    s_cTimer = mkCentre(body, 0xFFFFFF, kTimer);
    s_cProf = mkCentre(body, C_SUB, kSmall);

    /* The hold-to-talk bar, for a half-duplex call. */
    s_talkRow = mkRowBox(s_call, 0);
    lv_obj_set_style_pad_hor(s_talkRow, lcdPx(8), 0);
    lv_obj_set_style_pad_bottom(s_talkRow, lcdPx(4), 0);
    s_talk = lv_button_create(s_talkRow);
    lv_obj_remove_style_all(s_talk);
    lv_obj_set_height(s_talk, lcdPx(32));
    lv_obj_set_width(s_talk, 1);
    lv_obj_set_flex_grow(s_talk, 1);
    lv_obj_set_style_radius(s_talk, lcdPx(6), 0);
    lv_obj_set_style_bg_opa(s_talk, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_talk, 1, 0);
    lv_obj_remove_flag(s_talk, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_talk, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_ext_click_area(s_talk, lcdPx(4));
    /* The level fill sits under the label, a thin strip along the bar's foot. */
    s_talkLvl = lv_obj_create(s_talk);
    lv_obj_remove_style_all(s_talkLvl);
    lv_obj_remove_flag(s_talkLvl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_talkLvl, 0, lcdPx(3));
    lv_obj_set_style_bg_color(s_talkLvl, lv_color_hex(C_LEVEL), 0);
    lv_obj_set_style_bg_opa(s_talkLvl, LV_OPA_COVER, 0);
    lv_obj_align(s_talkLvl, LV_ALIGN_BOTTOM_LEFT, lcdPx(4), -lcdPx(2));
    lv_obj_add_flag(s_talkLvl, LV_OBJ_FLAG_HIDDEN);
    s_talkLbl = mkLabel(s_talk, "", C_SUB, kBold);
    lv_label_set_long_mode(s_talkLbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_talkLbl, lv_pct(96));
    lv_obj_set_style_text_align(s_talkLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_talkLbl);
    lv_obj_add_event_cb(s_talk, onTalkEvent, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_talk, onTalkEvent, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(s_talk, onTalkEvent, LV_EVENT_PRESS_LOST, nullptr);
    showIf(s_talkRow, false);

    lv_obj_t* bar = mkActionBar(s_call);
    s_bReject = mkButton(bar, "Reject", BTN_STOP, onReject, nullptr, IC_HANGUP);
    s_bMute = mkButton(bar, "Mute", BTN_GHOST, onMute, &s_bMuteLbl, IC_MIC, &s_bMuteIcon);
    s_bHang = mkButton(bar, "Hang up", BTN_STOP, onHang, &s_bHangLbl, IC_HANGUP);
    s_bAnswer = mkButton(bar, "Answer", BTN_GO, onAnswer, nullptr, IC_CALL);
    for (lv_obj_t* b : { s_bReject, s_bMute, s_bHang, s_bAnswer }) {
        lv_obj_set_width(b, 1);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_pad_ver(b, lcdPx(8), 0);
    }
    for (lv_obj_t* b : { s_bReject, s_bAnswer })
        lv_obj_add_event_cb(b, onRingKey, (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), nullptr);
}

void openCall() {
    refreshFont();
    if (!s_call) buildCall();
    g_callShape.clear();
    showScreen(SC_CALL);
    refreshCall();
}

/* ---- live updates ---- */

bool g_dPeers = false, g_dLog = false, g_dCall = false;
lv_timer_t* g_metaTimer = nullptr;
lv_timer_t* g_tickTimer = nullptr;
std::string g_lastState;    /* for the layer: the call screen opens on a call starting */

void metaTimerCb(lv_timer_t*) {
    g_metaTimer = nullptr;
    if (!s_layer) return;
    bool peers = g_dPeers, log = g_dLog, call = g_dCall;
    g_dPeers = g_dLog = g_dCall = false;
    refreshFont();
    if (peers) loadPeers();
    if (log) loadLog();

    std::string st = phState();
    if (st != g_lastState) {
        bool wasUp = callShown(g_lastState);
        g_lastState = st;
        /* A call starting (dialled here or elsewhere) takes the screen; one
         * that is over gives it back once the phone is idle again. */
        if (callActive(st) && !wasUp && g_shown && g_screen != SC_CALL) openCall();
        if (!callShown(st) && g_screen == SC_CALL) goHome();
    }

    if (peers || log) {
        if (g_screen == SC_HOME) rebuildRows();
        else g_homeStale = true;
    }
    if (call || peers) {
        refreshHome();
        if (g_screen == SC_CALL) refreshCall();
    }
}

void scheduleMeta() {
    if (g_metaTimer) return;
    g_metaTimer = lv_timer_create(metaTimerCb, DEBOUNCE_MS, nullptr);
    lv_timer_set_repeat_count(g_metaTimer, 1);
}

/* An incoming call raises the app over whatever is in front: the screen wakes,
 * any dialog closes, and the app's show opens the call screen. */
std::string g_watchState;   /* lcd task, layer or not */
void raiseForCall() {
    lcdNotifyActivity();
    lcdModalCloseAll();
    lcdShowProgram(APP_NAME);
}

void onPhoneChange(const char* key, const char* val) {
    if (!key) return;
    if (!strcmp(key, "lxst.phone.state")) {
        std::string st = val ? val : "";
        bool rang = st == "ringing" && g_watchState != "ringing";
        g_watchState = st;
        if (rang) {
            /* Already on the call screen in front: the refresh below does it. */
            if (!(s_layer && g_shown && g_screen == SC_CALL)) raiseForCall();
        }
    }
    if (!s_layer) return;
    if (!strcmp(key, "lxst.phone.cmd")) return;
    if (!strcmp(key, "lxst.phone.cmd.error")) {
        if (val && *val) showToast(printable(val), C_AMBER);
        refreshHome();
        return;
    }
    if (!strcmp(key, "lxst.phone.timer")) {
        /* Once a second while live: just the clock. */
        if (s_cTimer && g_screen == SC_CALL) {
            lv_label_set_text(s_cTimer, val ? val : "");
            showIf(s_cTimer, val && *val && phState() == "live");
        }
        if (s_banner && phState() == "live") refreshHome();
        return;
    }
    if (startsWith(key, "lxst.phone.peers.")) g_dPeers = true;
    else g_dCall = true;
    scheduleMeta();
}

void onLogChange(const char*, const char*) {
    if (!s_layer) return;
    g_dLog = true;
    scheduleMeta();
}

void onTalkChange(const char*, const char*) {
    if (!s_layer) return;
    refreshTalk();
}

void onStandbyChange(const char*, const char*) {
    if (!s_layer) return;
    if (storageGetInt("sys.standby", 0)) pttRelease();
    updateSpaceClaim();
}

/* Once a second: the Space claim follows focus moves made behind our back,
 * and the talk bar follows a key declared after the screen was built. */
void tickCb(lv_timer_t*) {
    if (!s_layer) return;
    updateSpaceClaim();
    if (g_screen == SC_CALL) refreshTalk();
}

/* ---- entry (lcd task, on a fresh layer) ---- */

void resetHandles() {
    s_home = s_call = nullptr;
    g_homeHdr = Header{};
    s_banner = s_bannerLbl = s_numLbl = s_noteLbl = s_dialTa = s_errLbl = s_rows = nullptr;
    g_rowsH.clear();
    g_homeStale = false;
    s_toast = s_toastLbl = nullptr;
    g_callHdr = Header{};
    s_cIcon = s_cName = s_cHash = s_cStatus = s_cTimer = s_cProf = nullptr;
    s_talkRow = s_talk = s_talkLbl = s_talkLvl = nullptr;
    s_bReject = s_bAnswer = s_bMute = s_bMuteLbl = s_bMuteIcon = s_bHang = s_bHangLbl = nullptr;
    g_callShape.clear();
    g_focusTarget = nullptr;
    g_canTalk = false;
    g_icons.clear();
}

void deleteTimers() {
    if (g_metaTimer)  { lv_timer_delete(g_metaTimer);  g_metaTimer = nullptr; }
    if (g_toastTimer) { lv_timer_delete(g_toastTimer); g_toastTimer = nullptr; }
    if (g_tickTimer)  { lv_timer_delete(g_tickTimer);  g_tickTimer = nullptr; }
    if (g_ringTimer)  { lv_timer_delete(g_ringTimer);  g_ringTimer = nullptr; }
    if (g_iconTimer)  { lv_timer_delete(g_iconTimer);  g_iconTimer = nullptr; }
    if (g_focusTimer) { lv_timer_delete(g_focusTimer); g_focusTimer = nullptr; }
    if (g_lvlTimer)   { lv_timer_delete(g_lvlTimer);   g_lvlTimer = nullptr; }
}

void phoneApp(lv_obj_t* root) {
    refreshFont();
    s_layer = root;
    /* A fresh layer: every handle and guard starts over, so a reopen after a
     * stop or an eviction rebuilds instead of trusting widgets that are gone. */
    resetHandles();
    deleteTimers();
    g_dPeers = g_dLog = g_dCall = false;
    g_screen = SC_HOME;
    g_shown = true;
    g_spaceClaimed = false;
    g_pttHeld = false;
    g_tickTimer = lv_timer_create(tickCb, 1000, nullptr);

    loadPeers();
    loadLog();
    loadContacts();
    g_lastState = phState();
    buildHome();
    buildCall();
    showScreen(SC_HOME);
    rebuildRows();
    refreshHome();
    if (callShown(g_lastState)) openCall();
    else deferFocus(s_dialTa);
}

}  // namespace

LxstPhoneApp::LxstPhoneApp() : LcdApp({ .name = APP_NAME, .iconBasename = "phone" }) {}

void LxstPhoneApp::appInit()
{
    /* Subscribed ON the lcd task, so the callbacks may touch LVGL, and here
     * rather than in onCreate, so an incoming call raises the app even if it
     * was never opened. The subscriptions outlive every layer; each callback
     * tests s_layer before touching a widget. */
    lcdRun([](void*) {
        g_watchState = phState();
        storageSubscribeChanges("lxst.phone.",       onPhoneChange);
        storageSubscribeChanges("s.lxst.phone.enabled", onPhoneChange);
        storageSubscribeChanges("s.lxst.phone.log.", onLogChange);
        storageSubscribeChanges("lxst.talking",      onTalkChange);
        storageSubscribeChanges("sys.standby",       onStandbyChange);
    });
}

void LxstPhoneApp::onCreate(lv_obj_t* root) { phoneApp(root); }

bool LxstPhoneApp::onBack()
{
    if (!s_layer) return false;
    if (g_screen == SC_CALL) { goHome(); return true; }
    return false;
}

void LxstPhoneApp::onShow()
{
    g_shown = true;
    if (!s_layer) return;
    std::string st = phState();
    g_lastState = st;
    if (callShown(st)) {
        if (g_screen != SC_CALL) openCall();
        else refreshCall();
    } else if (g_screen == SC_CALL) {
        goHome();
    } else {
        loadContacts();
        rebuildRows();
        refreshHome();
    }
    if (g_homeStale && g_screen == SC_HOME) rebuildRows();
    updateSpaceClaim();
    /* The shell restores the saved focus around this call: look again once
     * that has settled, and put the ringing screen's focus back on Answer. */
    lv_timer_t* t = lv_timer_create([](lv_timer_t*) {
        if (!s_layer) return;
        if (g_screen == SC_CALL && phState() == "ringing" && s_bAnswer && lcdInputGroup())
            lv_group_focus_obj(s_bAnswer);
        updateSpaceClaim();
    }, 50, nullptr);
    lv_timer_set_repeat_count(t, 1);
}

void LxstPhoneApp::onHide()
{
    g_shown = false;
    if (!s_layer) return;
    pttRelease();
    stopLevel();
    updateSpaceClaim();
}

void LxstPhoneApp::onClose()
{
    pttRelease();
    releaseSpace();
    /* The layer first: it is what every subscription callback tests. */
    s_layer = nullptr;
    deleteTimers();
    resetHandles();
    g_screen = SC_HOME;
}
