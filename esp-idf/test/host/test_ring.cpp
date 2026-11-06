/* Host test for the pre-roll ring (lxsess::PcmRing): it keeps exactly the
 * newest samples across wraps, trims to the pre-roll, and drains oldest first
 * while new samples keep arriving.
 *
 *   g++ -std=c++17 -Wall -Wextra -I../../src test_ring.cpp -o /tmp/test_ring && /tmp/test_ring
 */
#include "lxst_ring.h"

#include <cstdio>
#include <vector>

using lxsess::PcmRing;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* Drain up to n samples, oldest first. */
static std::vector<int16_t> pop(PcmRing& r, int n)
{
    std::vector<int16_t> out;
    while (n > 0 && r.size()) {
        const int16_t* p;
        int run = r.front(&p);
        if (run > n) run = n;
        out.insert(out.end(), p, p + run);
        r.drop(run);
        n -= run;
    }
    return out;
}

int main()
{
    std::vector<int16_t> mem(100);
    PcmRing r;
    r.attach(mem.data(), (int)mem.size());

    /* Idle: blocks of 30 trimmed to 70 keep exactly the newest 70, across wraps. */
    int16_t next = 0;
    for (int b = 0; b < 17; b++) {
        int16_t blk[30];
        for (auto& s : blk) s = next++;
        r.push(blk, 30);
        r.trim(70);
        CHECK(r.size() == (b == 0 ? 30 : b == 1 ? 60 : 70));
    }
    {
        PcmRing c = r;
        auto v = pop(c, 1000);
        bool ok = v.size() == 70;
        for (size_t i = 0; ok && i < v.size(); i++) ok = v[i] == (int16_t)(next - 70 + (int)i);
        CHECK(ok);
    }

    /* Keyed: push a block, drain two, until empty; the output is one unbroken run. */
    std::vector<int16_t> sent;
    int16_t first = (int16_t)(next - 70);
    int rounds = 0;
    while (r.size() && rounds < 100) {
        int16_t blk[30];
        for (auto& s : blk) s = next++;
        r.push(blk, 30);
        CHECK(r.size() <= 100);
        auto v = pop(r, 60);
        sent.insert(sent.end(), v.begin(), v.end());
        rounds++;
    }
    bool ok = !sent.empty();
    for (size_t i = 0; ok && i < sent.size(); i++) ok = sent[i] == (int16_t)(first + (int)i);
    CHECK(ok);
    CHECK(sent.back() == (int16_t)(next - 1));
    std::printf("drained %zu samples in %d blocks of 30\n", sent.size(), rounds);

    /* A push past cap keeps the newest cap samples. */
    std::vector<int16_t> big(250);
    for (int i = 0; i < 250; i++) big[i] = (int16_t)(1000 + i);
    r.push(big.data(), 250);
    auto v = pop(r, 1000);
    CHECK(v.size() == 100 && v.front() == 1150 && v.back() == 1249);

    /* Overflowing a partly full ring drops the oldest. */
    r.clear();
    for (int i = 0; i < 9; i++) {
        int16_t blk[15];
        for (int k = 0; k < 15; k++) blk[k] = (int16_t)(i * 15 + k);
        r.push(blk, 15);
    }
    v = pop(r, 1000);
    CHECK(v.size() == 100 && v.front() == 35 && v.back() == 134);

    /* Unattached: nothing held. */
    PcmRing z;
    z.push(big.data(), 10);
    CHECK(z.size() == 0);

    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("test_ring: all passed\n");
    return 0;
}
