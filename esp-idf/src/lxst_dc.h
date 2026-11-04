/**
 * lxst_dc.h — what one message from the browser on "lxst:2" is (lxst.h).
 * Host-portable.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include "av_hdr.h"
#include "lxst.h"

namespace lxsess {

enum DcMsg : uint8_t { DC_DROP, DC_PTT, DC_AUDIO };

/** [LXST_DC_PTT][down] is exactly two bytes. A microphone block is an audio
 *  av_hdr_t whose codec `decodable` accepts, then at least one payload byte.
 *  Anything else is dropped. */
inline DcMsg dcClassify(const uint8_t* m, size_t n, bool (*decodable)(uint8_t codec))
{
    if (n == 2 && m[0] == LXST_DC_PTT) return DC_PTT;
    if (n <= sizeof(av_hdr_t)) return DC_DROP;
    const av_hdr_t* h = (const av_hdr_t*)m;
    if (h->type != AV_TYPE_AUDIO || !decodable(h->codec)) return DC_DROP;
    return DC_AUDIO;
}

} // namespace lxsess
