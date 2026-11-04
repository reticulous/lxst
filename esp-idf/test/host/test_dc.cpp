/* Host test for the browser channel's message classification (lxst_dc.h).
 *
 *   g++ -std=c++17 -Wall -I../../src -I../../include -Istub -I../../../../audio/esp-idf test_dc.cpp -o /tmp/test_dc && /tmp/test_dc
 */
#include "lxst_dc.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace lxsess;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* pcm16, ulaw16k, adpcm */
static bool decodable(uint8_t c) { return c == 0 || c == 2 || c == 3; }

static std::vector<uint8_t> block(uint8_t type, uint8_t codec, size_t payload, uint8_t fill)
{
    std::vector<uint8_t> m(sizeof(av_hdr_t) + payload, fill);
    avHdrFill((av_hdr_t*)m.data(), type, codec, 1234, 60);
    return m;
}

int main()
{
    /* The talk key: exactly two bytes. */
    {
        uint8_t d[2] = { LXST_DC_PTT, 1 };
        CHECK(dcClassify(d, 2, decodable) == DC_PTT);
        CHECK(dcClassify(d, 1, decodable) == DC_DROP);
        uint8_t l[3] = { LXST_DC_PTT, 1, 0 };
        CHECK(dcClassify(l, 3, decodable) == DC_DROP);
    }

    /* lxst → browser control coming back is dropped. */
    {
        uint8_t o[2] = { LXST_DC_OWNER, 1 };
        uint8_t m[2] = { LXST_DC_MIC, 1 };
        CHECK(dcClassify(o, 2, decodable) == DC_DROP);
        CHECK(dcClassify(m, 2, decodable) == DC_DROP);
    }

    /* Audio blocks with a header. */
    {
        auto p = block(AV_TYPE_AUDIO, 0, 640, 0x80);
        CHECK(dcClassify(p.data(), p.size(), decodable) == DC_AUDIO);
        auto u = block(AV_TYPE_AUDIO, 2, 320, 0x81);
        CHECK(dcClassify(u.data(), u.size(), decodable) == DC_AUDIO);
        auto a = block(AV_TYPE_AUDIO, 3, 1, 0);
        CHECK(dcClassify(a.data(), a.size(), decodable) == DC_AUDIO);
    }

    /* A header with no payload, a codec not decoded, video: dropped. */
    {
        auto e = block(AV_TYPE_AUDIO, 0, 0, 0);
        CHECK(dcClassify(e.data(), e.size(), decodable) == DC_DROP);
        auto u8 = block(AV_TYPE_AUDIO, 1, 160, 0);
        CHECK(dcClassify(u8.data(), u8.size(), decodable) == DC_DROP);
        auto bad = block(AV_TYPE_AUDIO, 9, 160, 0);
        CHECK(dcClassify(bad.data(), bad.size(), decodable) == DC_DROP);
        auto v = block(AV_TYPE_VIDEO, 0, 160, 0);
        CHECK(dcClassify(v.data(), v.size(), decodable) == DC_DROP);
    }

    /* Header-less PCM is dropped, whatever its first byte: never a talk key or
     * any other control message. */
    {
        std::vector<uint8_t> raw(640);
        for (uint8_t first : { (uint8_t)0x00, (uint8_t)0x01, (uint8_t)0x80, (uint8_t)0x81,
                               (uint8_t)0x82, (uint8_t)0xff }) {
            std::memset(raw.data(), 0x11, raw.size());
            raw[0] = first;
            raw[1] = 0x55;          /* not a decodable codec, as raw PCM's second byte may be */
            CHECK(dcClassify(raw.data(), raw.size(), decodable) == DC_DROP);
        }
    }

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_dc: all passed\n");
    return 0;
}
