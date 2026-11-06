/**
 * lxst_ring.h — a PCM16 first-in first-out ring over caller-owned memory, for
 * the push-to-talk pre-roll: it holds the most recent samples pushed, and
 * hands them back oldest first. Host-portable.
 */
#pragma once

#include <cstdint>
#include <cstring>

namespace lxsess {

struct PcmRing {
    int16_t* buf = nullptr;
    int      cap = 0;          /* samples buf holds */
    int      head = 0;         /* oldest sample */
    int      count = 0;

    void attach(int16_t* b, int c) { buf = b; cap = b ? c : 0; clear(); }
    void clear() { head = 0; count = 0; }
    int  size() const { return count; }

    /** Append; past cap the oldest samples go. */
    void push(const int16_t* pcm, int n)
    {
        if (cap <= 0 || n <= 0) return;
        if (n >= cap) {
            std::memcpy(buf, pcm + (n - cap), (size_t)cap * 2);
            head = 0;
            count = cap;
            return;
        }
        int over = count + n - cap;
        if (over > 0) drop(over);
        int w = head + count;
        if (w >= cap) w -= cap;
        int first = cap - w < n ? cap - w : n;
        std::memcpy(buf + w, pcm, (size_t)first * 2);
        if (n > first) std::memcpy(buf, pcm + first, (size_t)(n - first) * 2);
        count += n;
    }

    /** Keep only the newest `keep` samples. */
    void trim(int keep) { if (count > keep) drop(count - keep); }

    /** The oldest samples that lie contiguous in buf; their count. */
    int front(const int16_t** p) const
    {
        *p = buf + head;
        int run = cap - head;
        return count < run ? count : run;
    }

    /** Forget the oldest n samples. */
    void drop(int n)
    {
        if (n >= count) { clear(); return; }
        head += n;
        if (head >= cap) head -= cap;
        count -= n;
    }
};

} // namespace lxsess
