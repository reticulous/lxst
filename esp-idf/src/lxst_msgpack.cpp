#include "lxst_msgpack.h"

#include <cstring>

namespace lxstmp {

void Writer::be(uint64_t v, int bytes)
{
    for (int k = bytes - 1; k >= 0; --k) out.push_back((uint8_t)(v >> (8 * k)));
}

void Writer::uint(uint64_t v)
{
    if (v < 128)                { out.push_back((uint8_t)v); }
    else if (v <= 0xff)         { out.push_back(0xcc); be(v, 1); }
    else if (v <= 0xffff)       { out.push_back(0xcd); be(v, 2); }
    else if (v <= 0xffffffffu)  { out.push_back(0xce); be(v, 4); }
    else                        { out.push_back(0xcf); be(v, 8); }
}

void Writer::sint(int64_t v)
{
    if (v >= 0) { uint((uint64_t)v); return; }
    if (v >= -32)              { out.push_back((uint8_t)(int8_t)v); }
    else if (v >= -128)        { out.push_back(0xd0); be((uint64_t)v, 1); }
    else if (v >= -32768)      { out.push_back(0xd1); be((uint64_t)v, 2); }
    else if (v >= -2147483648LL) { out.push_back(0xd2); be((uint64_t)v, 4); }
    else                       { out.push_back(0xd3); be((uint64_t)v, 8); }
}

void Writer::float32(float v)
{
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    out.push_back(0xca);
    be(bits, 4);
}

void Writer::float64(double v)
{
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    out.push_back(0xcb);
    be(bits, 8);
}

void Writer::str(const char* p, size_t n)
{
    if (n < 32)              { out.push_back((uint8_t)(0xa0 | n)); }
    else if (n <= 0xff)      { out.push_back(0xd9); be(n, 1); }
    else if (n <= 0xffff)    { out.push_back(0xda); be(n, 2); }
    else                     { out.push_back(0xdb); be(n, 4); }
    out.insert(out.end(), (const uint8_t*)p, (const uint8_t*)p + n);
}

void Writer::str(const char* s)
{
    str(s, std::strlen(s));
}

void Writer::bin(const uint8_t* p, size_t n)
{
    if (n <= 0xff)           { out.push_back(0xc4); be(n, 1); }
    else if (n <= 0xffff)    { out.push_back(0xc5); be(n, 2); }
    else                     { out.push_back(0xc6); be(n, 4); }
    out.insert(out.end(), p, p + n);
}

void Writer::array(uint32_t n)
{
    if (n < 16)              { out.push_back((uint8_t)(0x90 | n)); }
    else if (n <= 0xffff)    { out.push_back(0xdc); be(n, 2); }
    else                     { out.push_back(0xdd); be(n, 4); }
}

void Writer::map(uint32_t n)
{
    if (n < 16)              { out.push_back((uint8_t)(0x80 | n)); }
    else if (n <= 0xffff)    { out.push_back(0xde); be(n, 2); }
    else                     { out.push_back(0xdf); be(n, 4); }
}

/* ── Value ── */

bool Value::getInt(int64_t& out) const
{
    if (kind == INT) { out = i; return true; }
    if (kind == UINT && u <= (uint64_t)INT64_MAX) { out = (int64_t)u; return true; }
    return false;
}

bool Value::getNumber(double& out) const
{
    switch (kind) {
    case UINT:  out = (double)u; return true;
    case INT:   out = (double)i; return true;
    case FLOAT: out = f; return true;
    default:    return false;
    }
}

bool Value::truthy() const
{
    switch (kind) {
    case BOOL:  return b;
    case UINT:  return u != 0;
    case INT:   return true;
    case FLOAT: return f != 0.0;
    case STR: case BIN: return len != 0;
    case ARRAY: case MAP: return !items.empty();
    case EXT:   return true;
    default:    return false;
    }
}

std::string Value::str() const
{
    if (kind != STR && kind != BIN) return std::string();
    return std::string((const char*)data, len);
}

bool Value::strEq(const char* s) const
{
    if (kind != STR) return false;
    size_t n = std::strlen(s);
    return n == len && std::memcmp(data, s, n) == 0;
}

const Value* Value::atInt(int64_t k) const
{
    if (kind != MAP) return nullptr;
    for (size_t j = items.size(); j >= 2; j -= 2) {
        const Value& key = items[j - 2];
        int64_t v;
        if (key.getInt(v) && v == k) return &items[j - 1];
    }
    return nullptr;
}

const Value* Value::at(const char* k) const
{
    if (kind != MAP) return nullptr;
    for (size_t j = items.size(); j >= 2; j -= 2)
        if (items[j - 2].strEq(k)) return &items[j - 1];
    return nullptr;
}

bool validUtf8(const uint8_t* p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        if (c < 0x80) { i++; continue; }
        int more;
        uint32_t cp, min;
        if ((c & 0xe0) == 0xc0)      { more = 1; cp = c & 0x1f; min = 0x80; }
        else if ((c & 0xf0) == 0xe0) { more = 2; cp = c & 0x0f; min = 0x800; }
        else if ((c & 0xf8) == 0xf0) { more = 3; cp = c & 0x07; min = 0x10000; }
        else return false;
        if (n - i <= (size_t)more) return false;
        for (int k = 1; k <= more; ++k) {
            if ((p[i + k] & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (p[i + k] & 0x3f);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        i += (size_t)more + 1;
    }
    return true;
}

namespace {

struct Reader {
    const uint8_t* p;
    size_t         n;
    size_t         i = 0;

    bool be(int bytes, uint64_t& v)
    {
        if ((size_t)bytes > n - i) return false;
        v = 0;
        for (int k = 0; k < bytes; ++k) v = (v << 8) | p[i++];
        return true;
    }

    bool payload(Value& v, Value::Kind kind, uint64_t len)
    {
        if (len > n - i) return false;
        v.kind = kind;
        v.data = p + i;
        v.len = (uint32_t)len;
        i += (size_t)len;
        return kind != Value::STR || validUtf8(v.data, v.len);
    }

    bool container(Value& v, Value::Kind kind, uint64_t count, int depth)
    {
        v.kind = kind;
        uint64_t items = kind == Value::MAP ? count * 2 : count;
        if (items > n - i) return false;   /* every item is at least a byte */
        v.items.resize((size_t)items);
        for (auto& e : v.items)
            if (!item(e, depth + 1)) return false;
        return true;
    }

    bool ext(Value& v, uint64_t len)
    {
        if (i >= n) return false;
        v.ext_type = (int8_t)p[i++];
        return payload(v, Value::EXT, len);
    }

    bool item(Value& v, int depth)
    {
        if (depth > MAX_DEPTH || i >= n) return false;
        uint8_t b = p[i++];
        uint64_t a;
        if (b <= 0x7f) { v.kind = Value::UINT; v.u = b; return true; }
        if (b >= 0xe0) { v.kind = Value::INT; v.i = (int8_t)b; return true; }
        if ((b & 0xf0) == 0x80) return container(v, Value::MAP, b & 0x0f, depth);
        if ((b & 0xf0) == 0x90) return container(v, Value::ARRAY, b & 0x0f, depth);
        if ((b & 0xe0) == 0xa0) return payload(v, Value::STR, b & 0x1f);
        switch (b) {
        case 0xc0: v.kind = Value::NIL; return true;
        case 0xc2: v.kind = Value::BOOL; v.b = false; return true;
        case 0xc3: v.kind = Value::BOOL; v.b = true; return true;
        case 0xc4: return be(1, a) && payload(v, Value::BIN, a);
        case 0xc5: return be(2, a) && payload(v, Value::BIN, a);
        case 0xc6: return be(4, a) && payload(v, Value::BIN, a);
        case 0xc7: return be(1, a) && ext(v, a);
        case 0xc8: return be(2, a) && ext(v, a);
        case 0xc9: return be(4, a) && ext(v, a);
        case 0xca: {
            if (!be(4, a)) return false;
            uint32_t bits = (uint32_t)a;
            float fl;
            std::memcpy(&fl, &bits, 4);
            v.kind = Value::FLOAT; v.single = true; v.f = fl;
            return true;
        }
        case 0xcb: {
            if (!be(8, a)) return false;
            std::memcpy(&v.f, &a, 8);
            v.kind = Value::FLOAT;
            return true;
        }
        case 0xcc: if (!be(1, a)) return false; v.kind = Value::UINT; v.u = a; return true;
        case 0xcd: if (!be(2, a)) return false; v.kind = Value::UINT; v.u = a; return true;
        case 0xce: if (!be(4, a)) return false; v.kind = Value::UINT; v.u = a; return true;
        case 0xcf: if (!be(8, a)) return false; v.kind = Value::UINT; v.u = a; return true;
        case 0xd0: if (!be(1, a)) return false; v.i = (int8_t)a;  break;
        case 0xd1: if (!be(2, a)) return false; v.i = (int16_t)a; break;
        case 0xd2: if (!be(4, a)) return false; v.i = (int32_t)a; break;
        case 0xd3: if (!be(8, a)) return false; v.i = (int64_t)a; break;
        case 0xd4: return ext(v, 1);
        case 0xd5: return ext(v, 2);
        case 0xd6: return ext(v, 4);
        case 0xd7: return ext(v, 8);
        case 0xd8: return ext(v, 16);
        case 0xd9: return be(1, a) && payload(v, Value::STR, a);
        case 0xda: return be(2, a) && payload(v, Value::STR, a);
        case 0xdb: return be(4, a) && payload(v, Value::STR, a);
        case 0xdc: return be(2, a) && container(v, Value::ARRAY, a, depth);
        case 0xdd: return be(4, a) && container(v, Value::ARRAY, a, depth);
        case 0xde: return be(2, a) && container(v, Value::MAP, a, depth);
        case 0xdf: return be(4, a) && container(v, Value::MAP, a, depth);
        default:   return false;   /* 0xc1 is never used */
        }
        /* signed forms: a non-negative value is the same integer as UINT */
        if (v.i >= 0) { v.kind = Value::UINT; v.u = (uint64_t)v.i; }
        else v.kind = Value::INT;
        return true;
    }
};

}  // namespace

bool decode(const uint8_t* p, size_t n, Value& out)
{
    Reader r{ p, n };
    out = Value{};
    if (!r.item(out, 0)) return false;
    return r.i == n;
}

}  // namespace lxstmp
