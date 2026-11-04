/* Host test for the LXST msgpack codec against encodings made by RNS's umsgpack.
 *
 *   g++ -std=c++17 -Wall -Wextra -I../../include test_msgpack.cpp ../../src/lxst_msgpack.cpp -o /tmp/test_msgpack && /tmp/test_msgpack
 */
#include "lxst_msgpack.h"
#include "vectors.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using lxstmp::Value;
using lxstmp::Writer;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static bool same(const std::vector<uint8_t>& a, const uint8_t* b, size_t n)
{
    return a.size() == n && std::memcmp(a.data(), b, n) == 0;
}

static uint32_t fnv1a(const std::vector<uint8_t>& d)
{
    uint32_t h = 0x811C9DC5u;
    for (uint8_t b : d) h = (h ^ b) * 0x01000193u;
    return h;
}

static bool dec(const uint8_t* p, size_t n, Value& v) { return lxstmp::decode(p, n, v); }

/* Every proper prefix of a valid encoding must fail to decode. */
static void truncations(const uint8_t* p, size_t n)
{
    for (size_t k = 0; k < n; ++k) {
        Value v;
        if (dec(p, k, v)) { std::printf("FAIL truncated to %zu of %zu decoded\n", k, n); failures++; return; }
    }
}

int main()
{
    /* ── integers: write matches umsgpack byte for byte; read gives the value back ── */
    for (const auto& t : INT_VECS) {
        Writer w;
        if (t.neg) w.sint(t.i);
        else w.uint(t.u);
        CHECK(same(w.out, t.b, t.n));
        if (!t.neg && t.u <= (uint64_t)INT64_MAX) {
            Writer s;
            s.sint((int64_t)t.u);
            CHECK(same(s.out, t.b, t.n));
        }
        Value v;
        CHECK(dec(t.b, t.n, v));
        if (t.neg) {
            CHECK(v.kind == Value::INT && v.i == t.i);
        } else {
            CHECK(v.kind == Value::UINT && v.u == t.u);
        }
        int64_t i;
        CHECK(v.getInt(i) == (t.neg || t.u <= (uint64_t)INT64_MAX));
        truncations(t.b, t.n);
    }
    /* Non-negative values in the signed forms read back as the same UINT. */
    {
        const uint8_t d0[] = { 0xd0, 0x05 }, d3[] = { 0xd3, 0, 0, 0, 0, 0, 0, 1, 0 };
        Value v;
        CHECK(dec(d0, 2, v) && v.kind == Value::UINT && v.u == 5);
        CHECK(dec(d3, 9, v) && v.kind == Value::UINT && v.u == 256);
    }

    /* ── str / bin / array / map at every size boundary ── */
    for (const auto& t : SIZED_VECS) {
        Writer w;
        if (t.kind == 's') {
            std::string s;
            for (uint32_t k = 0; k < t.count; ++k) s.push_back((char)('a' + k % 26));
            w.str(s);
        } else if (t.kind == 'b') {
            std::vector<uint8_t> b(t.count);
            for (uint32_t k = 0; k < t.count; ++k) b[k] = (uint8_t)k;
            w.bin(b.data(), b.size());
        } else if (t.kind == 'a') {
            w.array(t.count);
            for (uint32_t k = 0; k < t.count; ++k) w.nil();
        } else {
            w.map(t.count);
            for (uint32_t k = 0; k < t.count; ++k) { w.uint(k); w.nil(); }
        }
        CHECK(w.out.size() == t.total);
        CHECK(fnv1a(w.out) == t.fnv);
        CHECK(std::memcmp(w.out.data(), t.b, t.n) == 0);

        Value v;
        CHECK(dec(w.out.data(), w.out.size(), v));
        if (t.kind == 's') {
            CHECK(v.isStr() && v.len == t.count);
            CHECK(t.count == 0 || v.data[t.count - 1] == (uint8_t)('a' + (t.count - 1) % 26));
        } else if (t.kind == 'b') {
            CHECK(v.isBin() && v.len == t.count);
            CHECK(t.count == 0 || v.data[t.count - 1] == (uint8_t)(t.count - 1));
        } else if (t.kind == 'a') {
            CHECK(v.isArray() && v.size() == t.count);
            CHECK(t.count == 0 || v.item(t.count - 1)->isNil());
        } else {
            CHECK(v.isMap() && v.size() == t.count);
            CHECK(t.count == 0 || (v.at((int64_t)t.count - 1) && v.at((int64_t)t.count - 1)->isNil()));
            CHECK(v.at((int64_t)t.count) == nullptr);
        }
        if (w.out.size() <= 300) truncations(w.out.data(), w.out.size());
    }

    /* ── floats: read both widths; write matches umsgpack ── */
    for (const auto& t : FLOAT_VECS) {
        Value v;
        CHECK(dec(t.b, t.n, v));
        CHECK(v.isFloat() && v.single == t.single && v.f == t.v);
        double d;
        CHECK(v.getNumber(d) && d == t.v);
        int64_t i;
        CHECK(!v.getInt(i));
        Writer w;
        if (t.single) w.float32((float)t.v);
        else w.float64(t.v);
        CHECK(same(w.out, t.b, t.n));
        truncations(t.b, t.n);
    }

    /* ── ext: every width decodes to an opaque EXT ── */
    for (const auto& t : EXT_VECS) {
        Value v;
        CHECK(dec(t.b, t.n, v));
        CHECK(v.isExt() && v.ext_type == t.type && v.len == t.len);
        truncations(t.b, t.n);
    }
    {
        Value v;
        CHECK(dec(vec_ext_in_map, sizeof vec_ext_in_map, v));
        CHECK(v.isMap() && v.size() == 3);
        CHECK(v.at(1) && v.at(1)->isExt() && v.at(1)->ext_type == 5 && v.at(1)->len == 3);
        int64_t i;
        CHECK(v.at(2) && v.at(2)->getInt(i) && i == 7);
        CHECK(v.at(3) && v.at(3)->size() == 2 && v.at(3)->item(1)->strEq("after"));
    }

    /* ── nil and bools ── */
    {
        Value v;
        CHECK(dec(vec_nil, 1, v) && v.isNil() && !v.truthy());
        CHECK(dec(vec_true, 1, v) && v.isBool() && v.b && v.truthy());
        CHECK(dec(vec_false, 1, v) && v.isBool() && !v.b && !v.truthy());
        Writer w;
        w.nil(); w.boolean(true); w.boolean(false);
        const uint8_t want[] = { 0xc0, 0xc3, 0xc2 };
        CHECK(same(w.out, want, 3));
    }

    /* ── a HELLO-like str-keyed map ── */
    {
        Value v;
        CHECK(dec(vec_str_keys, sizeof vec_str_keys, v));
        CHECK(v.isMap() && v.size() == 6);
        CHECK(v.at("name") && v.at("name")->str() == "h\xc3\xa9llo w\xc3\xb6rld \xe2\x9c\x93");
        int64_t i;
        CHECK(v.at("sid") && v.at("sid")->getInt(i) && i == 3);
        CHECK(v.at("motd") && v.at("motd")->isStr() && v.at("motd")->len == 0);
        double d;
        CHECK(v.at("rtt") && v.at("rtt")->getNumber(d) && d == 12.5);
        CHECK(v.at("hops") && v.at("hops")->isNil());
        CHECK(v.at("nope") == nullptr && v.at(1) == nullptr);

        Writer w;   /* same insertion order as the Python dict */
        w.map(6);
        w.str("name"); w.str("h\xc3\xa9llo w\xc3\xb6rld \xe2\x9c\x93");
        w.str("sid"); w.sint(3);
        w.str("motd"); w.str("");
        w.str("ver"); w.sint(1);
        w.str("rtt"); w.float64(12.5);
        w.str("hops"); w.nil();
        CHECK(same(w.out, vec_str_keys, sizeof vec_str_keys));
        truncations(vec_str_keys, sizeof vec_str_keys);
    }

    /* ── nesting ── */
    {
        Value v;
        CHECK(dec(vec_nested, sizeof vec_nested, v));
        const Value* a = v.at(0);
        CHECK(a && a->size() == 4);
        CHECK(a->item(1)->item(1)->item(2)->isBool());
        int64_t i;
        CHECK(a->item(2)->getInt(i) && i == -5);
        CHECK(a->item(3)->kind == Value::UINT && a->item(3)->u == (1ull << 40));
        CHECK(v.at("k") && v.at("k")->at("x") && v.at("k")->at("x")->isBin() && v.at("k")->at("x")->len == 2);
    }

    /* ── a FRAMES packet: bins borrow the input ── */
    {
        Value v;
        CHECK(dec(vec_frames, sizeof vec_frames, v));
        const Value* f = v.at(1);
        CHECK(f && f->size() == 2 && f->item(0)->isBin() && f->item(0)->len == 82);
        CHECK(f->item(0)->data >= vec_frames && f->item(0)->data < vec_frames + sizeof vec_frames);
        CHECK(f->item(0)->data[0] == 2 && f->item(0)->data[81] == 79);
        CHECK(v.at(16) && v.at(16)->u == 65535);
        Writer w;
        w.map(2); w.uint(1); w.array(2);
        w.bin(f->item(0)->data, 82); w.bin(f->item(1)->data, 82);
        w.uint(16); w.uint(65535);
        CHECK(same(w.out, vec_frames, sizeof vec_frames));
    }

    /* ── duplicate keys: the last one wins on lookup ── */
    {
        const uint8_t d[] = { 0x82, 0x01, 0x0a, 0x01, 0x14 };
        Value v;
        CHECK(dec(d, sizeof d, v) && v.at(1) && v.at(1)->u == 20);
    }

    /* ── malformed input ── */
    {
        Value v;
        const uint8_t c1[] = { 0xc1 };
        CHECK(!dec(c1, 1, v));
        CHECK(!dec(nullptr, 0, v));
        const uint8_t trailing[] = { 0x01, 0x02 };
        CHECK(!dec(trailing, 2, v));
        const uint8_t huge_array[] = { 0xdd, 0xff, 0xff, 0xff, 0xff, 0xc0 };   /* claims 4G items */
        CHECK(!dec(huge_array, sizeof huge_array, v));
        const uint8_t huge_map[] = { 0xdf, 0x80, 0x00, 0x00, 0x00, 0x01, 0xc0 };
        CHECK(!dec(huge_map, sizeof huge_map, v));
        const uint8_t huge_str[] = { 0xdb, 0x00, 0x01, 0x00, 0x00, 'a' };
        CHECK(!dec(huge_str, sizeof huge_str, v));
        const uint8_t bad_utf8[] = { 0xa2, 0xc3, 0x28 };
        CHECK(!dec(bad_utf8, sizeof bad_utf8, v));
        const uint8_t overlong[] = { 0xa2, 0xc0, 0xaf };
        CHECK(!dec(overlong, sizeof overlong, v));
        const uint8_t surrogate[] = { 0xa3, 0xed, 0xa0, 0x80 };
        CHECK(!dec(surrogate, sizeof surrogate, v));
        const uint8_t bin_not_utf8[] = { 0xc4, 0x02, 0xc3, 0x28 };   /* bin carries anything */
        CHECK(dec(bin_not_utf8, sizeof bin_not_utf8, v) && v.isBin());
        /* 16 levels of nesting decode, 17 do not */
        std::vector<uint8_t> deep(16, 0x91);
        deep.push_back(0xc0);
        CHECK(dec(deep.data(), deep.size(), v));
        deep.insert(deep.begin(), 0x91);
        CHECK(!dec(deep.data(), deep.size(), v));
        /* a map missing its last value */
        const uint8_t half_map[] = { 0x82, 0x01, 0x02, 0x03 };
        CHECK(!dec(half_map, sizeof half_map, v));
    }

    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("test_msgpack: all passed\n");
    return 0;
}
