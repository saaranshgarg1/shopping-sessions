#pragma once
// v5 reducer: half-hour bin sessions from unsorted events + click coverage over session indices.
#include "core.hpp"
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace v5 {

// purchase is 0 for none, otherwise index+1 into the shared Purchase vector.
struct BinEvent { uint32_t time, purchase; };
struct Purchase { uint32_t category, price; };
struct Click { ss::SV id; int64_t time; uint32_t campaign; };
struct Daily { int32_t day; int64_t sessions, events, revenue; uint32_t category; bool has_category;
    bool operator==(const Daily&) const = default; };

// Reusable across people; callers keep one per thread.
struct Scratch {
    struct DayAcc {
        bool touched = false, has_cat = false;
        int64_t sessions = 0, events = 0, revenue = 0;
        uint32_t begin = 0, end = 0, best_cat = 0;
        int64_t best_rev = -1;
    };
    std::vector<uint32_t> bmin, bmax, bcount, bsess, occ;
    std::vector<int64_t> brev;
    std::vector<int64_t> sstart, srev, sevents;
    std::vector<DayAcc> day;
    std::vector<uint32_t> day_touched;
    std::vector<uint64_t> cat_key;
    std::vector<int64_t> cat_val;
    std::vector<uint32_t> cat_used;
    std::vector<const Click*> by_l;
    std::vector<uint32_t> by_l_touched;
    std::vector<uint64_t> cov;
    size_t cov_words = 0;
    std::vector<std::pair<uint32_t, size_t>> cov_touched;
    std::vector<size_t> campaign_slot;
};

// OR (fn) every word touched by bit range [b,e) with mask m.
template<class F> inline void for_range(uint64_t* words, size_t b, size_t e, F fn) {
    for (size_t w = b / 64; w <= (e - 1) / 64; ++w) {
        uint64_t m = ~0ULL;
        if (w == b / 64) m &= ~0ULL << (b % 64);
        if (w == (e - 1) / 64 && e % 64) m &= (1ULL << (e % 64)) - 1;
        fn(words[w], m);
    }
}

inline void reduce(const BinEvent* events, size_t event_count,
                   const std::vector<Purchase>& purchases,
                   const Click* clicks, size_t click_count,
                   int64_t window_start, size_t bin_count,
                   const std::vector<ss::SV>& categories,
                   size_t campaigns, int64_t first_day, size_t days,
                   std::vector<Daily>& daily, std::vector<ss::Stats>& stats,
                   Scratch& sc) {
    if (sc.bcount.size() < bin_count) {
        sc.bmin.resize(bin_count); sc.bmax.resize(bin_count);
        sc.bcount.resize(bin_count); sc.brev.resize(bin_count); sc.bsess.resize(bin_count);
    }
    if (sc.day.size() < days) sc.day.resize(days);
    for (size_t i = 0; i < event_count; ++i) {
        uint32_t b = uint32_t(events[i].time / ss::GAP);
        if (b >= bin_count) continue;
        if (!sc.bcount[b]) {
            sc.bmin[b] = sc.bmax[b] = events[i].time; sc.bcount[b] = 1; sc.brev[b] = 0;
            sc.occ.push_back(b);
        } else {
            if (events[i].time < sc.bmin[b]) sc.bmin[b] = events[i].time;
            if (events[i].time > sc.bmax[b]) sc.bmax[b] = events[i].time;
            ++sc.bcount[b];
        }
        if (events[i].purchase) sc.brev[b] += purchases[events[i].purchase - 1].price;
    }
    std::sort(sc.occ.begin(), sc.occ.end());
    sc.sstart.clear(); sc.srev.clear(); sc.sevents.clear();
    uint32_t cur_max = 0;
    for (uint32_t b : sc.occ) {
        if (sc.sstart.empty() || sc.bmin[b] - cur_max > ss::GAP) {
            sc.sstart.push_back(sc.bmin[b]); sc.srev.push_back(0); sc.sevents.push_back(0);
        }
        size_t s = sc.sstart.size() - 1;
        sc.bsess[b] = uint32_t(s);
        sc.srev[s] += sc.brev[b];
        sc.sevents[s] += sc.bcount[b];
        cur_max = sc.bmax[b];
    }
    size_t nsess = sc.sstart.size();

    for (size_t i = 0; i < nsess; ++i) {
        int64_t d = ss::day_of(window_start + sc.sstart[i]) - first_day;
        if (d < 0 || d >= int64_t(days)) continue;
        auto& da = sc.day[size_t(d)];
        if (!da.touched) { da.touched = true; da.begin = uint32_t(i); sc.day_touched.push_back(uint32_t(d)); }
        da.end = uint32_t(i + 1);
        ++da.sessions; da.events += sc.sevents[i]; da.revenue += sc.srev[i];
    }

    if (sc.cat_key.empty()) { sc.cat_key.assign(16, 0); sc.cat_val.assign(16, 0); }
    auto cat_add = [&](uint64_t key) -> int64_t& {
        if ((sc.cat_used.size() + 1) * 2 > sc.cat_key.size()) {
            std::vector<uint64_t> ok(sc.cat_key.size() * 2, 0);
            std::vector<int64_t> ov(ok.size(), 0);
            sc.cat_used.clear();
            for (size_t j = 0; j < sc.cat_key.size(); ++j) if (sc.cat_key[j]) {
                size_t at = ss::mix(sc.cat_key[j]) & (ok.size() - 1);
                while (ok[at]) at = (at + 1) & (ok.size() - 1);
                ok[at] = sc.cat_key[j]; ov[at] = sc.cat_val[j];
                sc.cat_used.push_back(uint32_t(at));
            }
            sc.cat_key.swap(ok); sc.cat_val.swap(ov);
        }
        size_t at = ss::mix(key) & (sc.cat_key.size() - 1);
        while (sc.cat_key[at] && sc.cat_key[at] != key) at = (at + 1) & (sc.cat_key.size() - 1);
        if (!sc.cat_key[at]) { sc.cat_key[at] = key; sc.cat_val[at] = 0; sc.cat_used.push_back(uint32_t(at)); }
        return sc.cat_val[at];
    };
    for (size_t i = 0; i < event_count; ++i) {
        if (!events[i].purchase) continue;
        uint32_t b = uint32_t(events[i].time / ss::GAP);
        if (b >= bin_count) continue;
        int64_t d = ss::day_of(window_start + sc.sstart[sc.bsess[b]]) - first_day;
        if (d < 0 || d >= int64_t(days)) continue;
        const Purchase& p = purchases[events[i].purchase - 1];
        auto& da = sc.day[size_t(d)];
        if (!da.has_cat) { da.has_cat = true; da.best_rev = -1; }
        cat_add((uint64_t(d + 1) << 32) | p.category) += p.price;
    }
    for (uint32_t at : sc.cat_used) {
        uint64_t key = sc.cat_key[at];
        auto& da = sc.day[size_t(key >> 32) - 1];
        uint32_t cat = uint32_t(key);
        int64_t v = sc.cat_val[at];
        if (v > da.best_rev || (v == da.best_rev && categories[cat] < categories[da.best_cat])) {
            da.best_rev = v; da.best_cat = cat;
        }
    }
    for (uint32_t d : sc.day_touched) {
        auto& da = sc.day[d];
        daily.push_back({int32_t(first_day + d), da.sessions, da.events, da.revenue, da.best_cat, da.has_cat});
    }

    if (nsess) {
        if (sc.by_l.size() < nsess) sc.by_l.resize(nsess, nullptr);
        sc.cov_words = (nsess + 63) / 64;
        if (sc.campaign_slot.size()<campaigns) sc.campaign_slot.resize(campaigns,SIZE_MAX);
        auto better = [](const Click* a, const Click* b) {
            return a->time > b->time || (a->time == b->time && a->id < b->id);
        };
        for (size_t i = 0; i < click_count; ++i) {
            const Click& c = clicks[i];
            if (!c.campaign || c.campaign>=campaigns) continue;
            size_t l = size_t(std::lower_bound(sc.sstart.begin(), sc.sstart.end(), c.time) - sc.sstart.begin());
            if (l >= nsess) continue;
            size_t r = size_t(std::upper_bound(sc.sstart.begin(), sc.sstart.end(), c.time + ss::WEEK) - sc.sstart.begin());
            if (r <= l) continue;
            const Click*& best = sc.by_l[l];
            if (!best || better(&c, best)) {
                if (!best) sc.by_l_touched.push_back(uint32_t(l));
                best = &c;
            }
            size_t slot = sc.campaign_slot[c.campaign];
            if (slot == SIZE_MAX) {
                slot = sc.cov_touched.size() * sc.cov_words;
                if (sc.cov.size() < slot + sc.cov_words)
                    sc.cov.resize(std::max(slot + sc.cov_words, sc.cov.size() * 2));
                std::memset(&sc.cov[slot], 0, sc.cov_words * 8);
                sc.cov_touched.push_back({c.campaign, slot});
                sc.campaign_slot[c.campaign]=slot;
            }
            for_range(sc.cov.data() + slot, l, r, [](uint64_t& w, uint64_t m) { w |= m; });
        }
        const Click* best = nullptr;
        for (size_t i = 0; i < nsess; ++i) {
            if (sc.by_l[i] && (!best || better(sc.by_l[i], best))) best = sc.by_l[i];
            int64_t d = ss::day_of(window_start + sc.sstart[i]) - first_day;
            if (d < 0 || d >= int64_t(days)) continue;
            uint32_t camp = best && sc.sstart[i] - best->time <= ss::WEEK ? best->campaign : 0;
            if (camp < campaigns) {
                auto& st = stats[camp * days + size_t(d)];
                ++st.sessions; st.purchasing += sc.srev[i] > 0; st.revenue += sc.srev[i];
            }
        }
        for (auto& t : sc.cov_touched) {
            if (t.first >= campaigns) continue;
            for (uint32_t d : sc.day_touched) {
                auto& da = sc.day[d];
                uint32_t pc = 0;
                for_range(sc.cov.data() + t.second, da.begin, da.end,
                          [&](uint64_t& w, uint64_t m) { pc += __builtin_popcountll(w & m); });
                if (pc) stats[t.first * days + d].assisted += pc;
            }
        }
    }

    for (uint32_t b : sc.occ) sc.bcount[b] = 0;
    sc.occ.clear();
    for (uint32_t d : sc.day_touched) sc.day[d] = {};
    sc.day_touched.clear();
    for (uint32_t at : sc.cat_used) sc.cat_key[at] = 0;
    sc.cat_used.clear();
    for (uint32_t l : sc.by_l_touched) sc.by_l[l] = nullptr;
    sc.by_l_touched.clear();
    for (auto& t : sc.cov_touched) sc.campaign_slot[t.first]=SIZE_MAX;
    sc.cov_touched.clear();
}

} // namespace v5
