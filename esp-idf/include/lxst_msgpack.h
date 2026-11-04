/**
 * lxst_msgpack.h — the small msgpack codec LXST-borne protocols need.
 *
 * Writes nil, bool, integers (always in the smallest encoding, as Python's
 * umsgpack does), float32/float64, str, bin, arrays and maps. Reads all of
 * those plus ext types, which are kept as opaque EXT values so a packet that
 * carries one still decodes. A str must be valid UTF-8, as umsgpack demands.
 *
 * Decoding does not copy: STR, BIN and EXT values point into the input
 * buffer, which must outlive the Value tree.
 *
 * Plain C++17 with no platform includes, so the host tests build it as-is.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace lxstmp {

/* ── encoding ── */

class Writer {
public:
    std::vector<uint8_t> out;

    void nil()                              { out.push_back(0xc0); }
    void boolean(bool b)                    { out.push_back(b ? 0xc3 : 0xc2); }
    /** Any integer; non-negative ones take the unsigned forms. */
    void sint(int64_t v);
    void uint(uint64_t v);
    void float32(float v);
    void float64(double v);
    void str(const char* p, size_t n);
    void str(const std::string& s)          { str(s.data(), s.size()); }
    void str(const char* s);
    void bin(const uint8_t* p, size_t n);
    void array(uint32_t n);
    void map(uint32_t n);

private:
    void be(uint64_t v, int bytes);
};

/* ── decoding ──
 *
 * A decoded value is a tree. Maps keep their entries in wire order, keys are
 * values too, and lookups return the last entry with a matching key (a
 * Python dict keeps the last duplicate). */

struct Value {
    enum Kind : uint8_t { NONE, NIL, BOOL, UINT, INT, FLOAT, STR, BIN, ARRAY, MAP, EXT };
    Kind               kind = NONE;
    bool               b = false;      /* BOOL                                         */
    bool               single = false; /* FLOAT: it came as float32                    */
    int8_t             ext_type = 0;   /* EXT                                          */
    uint32_t           len = 0;        /* STR, BIN, EXT: payload length                */
    const uint8_t*     data = nullptr; /* STR, BIN, EXT: payload, inside the input     */
    union {
        uint64_t u;                    /* UINT: every non-negative integer             */
        int64_t  i;                    /* INT: negative integers only                  */
        double   f;                    /* FLOAT                                        */
    };
    std::vector<Value> items;          /* ARRAY elements; MAP key, value, key, value … */

    Value() : u(0) {}

    bool isNil() const    { return kind == NIL; }
    bool isBool() const   { return kind == BOOL; }
    bool isInt() const    { return kind == UINT || kind == INT; }
    bool isFloat() const  { return kind == FLOAT; }
    bool isNumber() const { return isInt() || isFloat(); }
    bool isStr() const    { return kind == STR; }
    bool isBin() const    { return kind == BIN; }
    bool isArray() const  { return kind == ARRAY; }
    bool isMap() const    { return kind == MAP; }
    bool isExt() const    { return kind == EXT; }

    /** An integer that fits int64_t. */
    bool getInt(int64_t& out) const;
    /** An integer or float as a double. */
    bool getNumber(double& out) const;
    /** Python truthiness: nil, false, 0, 0.0 and empty str/bin/array/map are false. */
    bool truthy() const;

    /** STR or BIN payload as a std::string (empty for other kinds). */
    std::string str() const;
    /** STR equal to a NUL-terminated string. */
    bool strEq(const char* s) const;

    /** ARRAY element count, or MAP entry count. 0 for other kinds. */
    size_t size() const   { return kind == ARRAY ? items.size() : kind == MAP ? items.size() / 2 : 0; }
    /** ARRAY element i, or nullptr. */
    const Value* item(size_t i) const { return kind == ARRAY && i < items.size() ? &items[i] : nullptr; }
    /** MAP entry i's key / value, or nullptr. */
    const Value* key(size_t i) const   { return kind == MAP && 2 * i + 1 < items.size() ? &items[2 * i] : nullptr; }
    const Value* value(size_t i) const { return kind == MAP && 2 * i + 1 < items.size() ? &items[2 * i + 1] : nullptr; }
    /** The value under an integer key, or nullptr. */
    const Value* atInt(int64_t key) const;
    template <typename T, typename std::enable_if<std::is_integral<T>::value, int>::type = 0>
    const Value* at(T key) const { return atInt((int64_t)key); }
    /** The value under a str key, or nullptr. */
    const Value* at(const char* key) const;
};

/** Nesting deeper than this fails to decode. */
constexpr int MAX_DEPTH = 16;

/** Decode exactly one value from p[0..n). False on malformed or truncated
 *  input, invalid UTF-8 in a str, the reserved byte 0xc1, trailing bytes, or
 *  nesting deeper than MAX_DEPTH. */
bool decode(const uint8_t* p, size_t n, Value& out);

/** True when p[0..n) is valid UTF-8 as Python decodes it (no overlong forms,
 *  no surrogates, nothing above U+10FFFF). */
bool validUtf8(const uint8_t* p, size_t n);

}  // namespace lxstmp
