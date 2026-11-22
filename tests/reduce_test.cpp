// Differential + boundary tests for v5/reduce.hpp against a sort-based brute force.
#include "reduce.hpp"
#include <cassert>
#include <deque>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <string>
using ss::SV, ss::GAP, ss::WEEK, ss::DAY, ss::day_of, ss::Stats;
using v5::BinEvent; using v5::Purchase; using v5::Click; using v5::Daily; using v5::Scratch;
using v5::reduce;
using std::uint32_t;

static bool better(const Click* a, const Click* b) {
    return a->time > b->time || (a->time == b->time && a->id < b->id);
}

struct Acc {
    int64_t sessions = 0, events = 0, revenue = 0;
    std::map<uint32_t, int64_t> cat;
};

// Brute force: sort events, split sessions on consecutive diff > GAP.
static void reference(const std::vector<BinEvent>& ev, const std::vector<Purchase>& pur,
                      const std::vector<Click>& clicks, int64_t window_start,
                      const std::vector<SV>& categories, size_t campaigns,
                      int64_t first_day, size_t days,
                      std::vector<Daily>& daily, std::vector<ss::Stats>& stats) {
    size_t n = ev.size();
    std::vector<uint32_t> idx(n);
    for (size_t i = 0; i < n; ++i) idx[i] = uint32_t(i);
    std::sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) { return ev[a].time < ev[b].time; });
    std::vector<int64_t> sstart, srev, scnt;
    std::vector<size_t> sess_of(n);
    for (size_t i = 0; i < n; ++i) {
        const BinEvent& e = ev[idx[i]];
        if (sstart.empty() || e.time - ev[idx[i - 1]].time > GAP) {
            sstart.push_back(e.time); srev.push_back(0); scnt.push_back(0);
        }
        sess_of[idx[i]] = sstart.size() - 1;
        ++scnt.back();
        if (e.purchase) srev.back() += pur[e.purchase - 1].price;
    }
    std::map<int64_t, Acc> perday;
    for (size_t j = 0; j < sstart.size(); ++j) {
        auto& a = perday[day_of(window_start + sstart[j])];
        ++a.sessions; a.events += scnt[j]; a.revenue += srev[j];
    }
    for (size_t i = 0; i < n; ++i) {
        if (!ev[i].purchase) continue;
        const Purchase& p = pur[ev[i].purchase - 1];
        perday[day_of(window_start + sstart[sess_of[i]])].cat[p.category] += p.price;
    }
    daily.clear();
    for (auto& [d, a] : perday) {
        bool has = false; int64_t best = 0; uint32_t best_cat = 0;
        for (auto& [c, r] : a.cat)
            if (!has || r > best || (r == best && categories[c] < categories[best_cat])) {
                has = true; best = r; best_cat = c;
            }
        daily.push_back({int32_t(d), a.sessions, a.events, a.revenue, best_cat, has});
    }
    stats.assign(campaigns * days, {});
    for (size_t j = 0; j < sstart.size(); ++j) {
        int64_t start = sstart[j];
        const Click* best = nullptr; std::set<uint32_t> assists;
        for (const Click& c : clicks)
            if (c.time <= start && start - c.time <= WEEK) {
                if (!best || better(&c, best)) best = &c;
                assists.insert(c.campaign);
            }
        size_t di = size_t(day_of(window_start+start) - first_day);
        auto& st = stats[(best ? best->campaign : 0u) * days + di];
        ++st.sessions; st.purchasing += srev[j] > 0; st.revenue += srev[j];
        for (uint32_t c : assists) ++stats[c * days + di].assisted;
    }
}

static void compare(const std::vector<Daily>& got, const std::vector<Daily>& want,
                    const std::vector<ss::Stats>& gs, const std::vector<ss::Stats>& ws,
                    const char* what) {
    std::map<int32_t, const Daily*> byday;
    for (const Daily& d : got) {
        assert(byday.find(d.day) == byday.end() && "duplicate day rows");
        byday[d.day] = &d;
    }
    assert(got.size() == want.size() && byday.size() == got.size());
    for (const Daily& w : want) {
        auto it = byday.find(w.day);
        assert(it != byday.end());
        const Daily& g = *it->second;
        assert(g.sessions == w.sessions && g.events == w.events && what);
        assert(g.revenue == w.revenue && g.has_category == w.has_category);
        if (w.has_category) assert(g.category == w.category);
    }
    assert(gs.size() == ws.size());
    for (size_t i = 0; i < gs.size(); ++i) {
        assert(gs[i].sessions == ws[i].sessions);
        assert(gs[i].purchasing == ws[i].purchasing);
        assert(gs[i].revenue == ws[i].revenue);
        assert(gs[i].assisted == ws[i].assisted);
    }
}

static void run_case(const std::vector<BinEvent>& ev, const std::vector<Purchase>& pur,
                     const std::vector<Click>& clicks, int64_t window_start,
                     const std::vector<SV>& categories, size_t campaigns,
                     int64_t first_day, size_t days, Scratch& sc,
                     std::vector<Daily>& daily, std::vector<ss::Stats>& stats) {
    daily.clear(); stats.clear(); stats.assign(campaigns * days, {});
    reduce(ev.data(), ev.size(), pur, clicks.data(), clicks.size(), window_start,
           size_t(ev.empty() ? 0 : (WEEK + DAY * 7) / GAP + 2), // wide enough bin table
           categories, campaigns, first_day, days, daily, stats, sc);
    std::vector<Daily> wd; std::vector<ss::Stats> ws;
    reference(ev, pur, clicks, window_start, categories, campaigns, first_day, days, wd, ws);
    compare(daily, wd, stats, ws, "differential");
}

int main() {
    const std::vector<SV> categories = {"zzz", "alpha", "alpha", "mid", "beta"};
    Scratch sc;
    std::vector<Daily> daily; std::vector<ss::Stats> stats;

    // Zero rows, null pointers: no output, stats untouched.
    reduce(nullptr, 0, {}, nullptr, 0, 0, 10, categories, 6, 0, 14, daily, stats, sc);
    assert(daily.empty());

    // Exact 30-minute merge boundaries: <= GAP merges, > GAP splits.
    {
        auto probe = [&](std::vector<BinEvent> ev, size_t want_sessions) {
            std::vector<Purchase> pur;
            run_case(ev, pur, {}, 0, categories, 6, 0, 14, sc, daily, stats);
            int64_t total = 0;
            for (auto& d : daily) total += d.sessions;
            assert(total == int64_t(want_sessions));
        };
        probe({{0, 0}, {uint32_t(GAP - 1), 0}}, 1);
        probe({{0, 0}, {uint32_t(GAP), 0}}, 1);        // diff == GAP merges
        probe({{0, 0}, {uint32_t(GAP + 1), 0}}, 2);    // +1 ms splits
        probe({{0, 0}, {uint32_t(2 * GAP - 1), 0}}, 2);
        probe({{0, 0}, {uint32_t(2 * GAP), 0}}, 2);
        probe({{0, 0}, {uint32_t(GAP), 0}, {uint32_t(2 * GAP), 0}}, 1); // chain merges
        probe({{uint32_t(GAP), 0}, {0, 0}}, 1);        // unsorted input
    }

    // Seven-day window edges and midnight crossing.
    {
        std::vector<Purchase> pur;
        // One session at rel t, click exactly t-WEEK credits; 1 ms older does not.
        for (int64_t dt : {int64_t(0), DAY / 2, DAY, 3 * DAY}) {
            std::vector<BinEvent> ev{{uint32_t(dt), 0}};
            for (int64_t off : {WEEK, WEEK + 1, WEEK - 1}) {
                std::vector<Click> clicks{{"c1", dt - off, 3}};
                run_case(ev, pur, clicks, 0, categories, 6, 0, 14, sc, daily, stats);
                size_t di = size_t(day_of(dt) / 1) / 1;
                di = size_t(day_of(dt));
                uint32_t want = off <= WEEK ? 3 : 0;
                assert(stats[want * 14 + di].sessions == 1);
                assert(stats[(want == 3 ? 0 : 3) * 14 + di].sessions == 0);
            }
        }
        // Click just past a session start never credits it (t > start).
        std::vector<Click> clicks{{"c1", int64_t(1), 2}};
        run_case({{0, 0}}, pur, clicks, 0, categories, 6, 0, 14, sc, daily, stats);
        assert(stats[0 * 14 + 0].sessions == 1 && stats[2 * 14 + 0].sessions == 0);

        // Midnight: window starts noon day D; events every GAP through D+1 01:00.
        int64_t noon = 19000 * DAY + 12 * 3600000;
        std::vector<BinEvent> ev;
        for (int64_t t = 0; t <= 13 * 3600000; t += GAP) ev.push_back({uint32_t(t), 0});
        run_case(ev, pur, {}, noon, categories, 6, day_of(noon), 14, sc, daily, stats);
        assert(daily.size() == 1 && daily[0].day == int32_t(day_of(noon)) && daily[0].events == int64_t(ev.size()));

        // Pre-1970 floor: negative timestamps still map to one floored day.
        int64_t past = -20000 * DAY - 12 * 3600000;
        run_case(ev, pur, {}, past, categories, 6, day_of(past), 14, sc, daily, stats);
        assert(daily.size() == 1 && daily[0].day == int32_t(day_of(past)) &&
               daily[0].events == int64_t(ev.size()));
    }

    // Same-timestamp click ties: latest time, then bytewise smallest id.
    {
        std::vector<Purchase> pur;
        std::vector<BinEvent> ev{{DAY, 0}};
        std::vector<Click> clicks{{"b", DAY - 1000, 1}, {"a", DAY - 1000, 2}, {"c", DAY - 1000, 3}};
        run_case(ev, pur, clicks, 0, categories, 6, 0, 14, sc, daily, stats);
        assert(stats[2 * 14 + 1].sessions == 1); // "a" wins
        assert(stats[1 * 14 + 1].assisted == 1 && stats[2 * 14 + 1].assisted == 1);
        assert(stats[3 * 14 + 1].assisted == 1);
        // Later time beats smaller id.
        clicks.push_back({"zz", DAY - 999, 4});
        run_case(ev, pur, clicks, 0, categories, 6, 0, 14, sc, daily, stats);
        assert(stats[4 * 14 + 1].sessions == 1);
        assert(stats[1 * 14 + 1].assisted == 1 && stats[2 * 14 + 1].assisted == 1);
    }

    // Duplicate clicks change nothing.
    {
        std::mt19937_64 rng(7);
        std::vector<BinEvent> ev; std::vector<Purchase> pur;
        for (int i = 0; i < 200; ++i)
            ev.push_back({uint32_t(rng() % (14 * DAY)), uint32_t(rng() % 4 == 0 ? rng() % 20 + 1 : 0)});
        pur.resize(20);
        for (auto& p : pur) p = {uint32_t(rng() % categories.size()), uint32_t(rng() % 1000 + 1)};
        std::vector<Click> clicks;
        std::deque<std::string> ids;
        for (int i = 0; i < 40; ++i) {
            ids.push_back("id" + std::string(3, char('a' + rng() % 26)));
            clicks.push_back({ids.back(), int64_t(rng() % (21 * DAY)) - WEEK, uint32_t(rng() % 5 + 1)});
        }
        run_case(ev, pur, clicks, 0, categories, 6, 0, 14, sc, daily, stats);
        auto d0 = daily; auto s0 = stats;
        auto twice = clicks;
        for (auto c : clicks) twice.push_back(c);
        run_case(ev, pur, twice, 0, categories, 6, 0, 14, sc, daily, stats);
        assert(d0 == daily);
        for (size_t i = 0; i < s0.size(); ++i)
            assert(s0[i].sessions == stats[i].sessions && s0[i].purchasing == stats[i].purchasing &&
                   s0[i].revenue == stats[i].revenue && s0[i].assisted == stats[i].assisted);
    }

    // Randomized differential: >=1000 histories (incl. >64 and 672+ sessions), shuffled rows.
    std::mt19937_64 rng(49325);
    int over64 = 0, whales = 0;
    for (int round = 0; round < 1100; ++round) {
        bool whale = round >= 1000;
        int64_t window_start = int64_t(rng() % 40000) * 3600000; // arbitrary, not always midnight
        int64_t first_day = day_of(window_start);
        size_t days = 17, campaigns = 6;
        std::vector<BinEvent> ev; std::vector<Purchase> pur;
        int64_t width = 14 * DAY;
        if (whale) {
            int64_t t = rng() % 1000;
            while (t < width) {
                ev.push_back({uint32_t(t), uint32_t(rng() % 5 == 0 ? ev.size() % 32 + 1 : 0)});
                if (ev.size() < 33) pur.push_back({uint32_t(rng() % categories.size()), uint32_t(rng() % 1000000 + 1)});
                t += GAP + 1; // isolated sessions, up to 672 per 14-day window
            }
            if (ev.size() > 600) ++whales;
        } else {
            int64_t t = rng() % width;
            for (int i = 0, n = 1 + int(rng() % 300); i < n; ++i) {
                ev.push_back({uint32_t(t), uint32_t(rng() % 4 == 0 ? ev.size() % 40 + 1 : 0)});
                if (ev.size() < 41) pur.push_back({uint32_t(rng() % categories.size()), uint32_t(rng() % 10000000 + 1)});
                t += int64_t(rng() % int(3 * GAP + 2 * DAY)) - GAP / 3; // merges and splits
                if (t < 0) t = 0;
                if (t >= width) break;
            }
        }
        for (auto& e : ev) if (e.purchase && e.purchase > pur.size()) e.purchase = 0;
        std::vector<Click> clicks;
        std::deque<std::string> ids;
        for (int i = 0, n = int(rng() % 45); i < n; ++i) {
            int64_t t = rng() % int(width + WEEK) - WEEK;
            std::string id(1 + rng() % 12, 'a');
            for (auto& ch : id) ch = char('!' + rng() % 90);
            uint32_t camp = uint32_t(1 + rng() % 5);
            if (!clicks.empty() && rng() % 5 == 0) { // same-time bytes ties
                t = clicks[rng() % clicks.size()].time;
                camp = 1 + uint32_t(rng() % 5);
            }
            ids.push_back(std::move(id));
            clicks.push_back({ids.back(), t, camp});
            if (rng() % 8 == 0) clicks.push_back(clicks.back()); // duplicate copies
        }
        if (ev.size() > 64) ++over64;
        // Shuffle rows so the reducer never sees sorted input.
        std::shuffle(ev.begin(), ev.end(), rng);
        std::shuffle(clicks.begin(), clicks.end(), rng);
        run_case(ev, pur, clicks, window_start, categories, campaigns, first_day, days, sc, daily, stats);
    }
    assert(over64 >= 100 && whales >= 90);

    // Scratch reuse across people must not leak state: rerun case 0 after everything.
    {
        std::vector<Purchase> pur;
        std::vector<BinEvent> ev{{0, 0}, {uint32_t(GAP), 0}};
        run_case(ev, pur, {}, 0, categories, 6, 0, 14, sc, daily, stats);
        assert(daily.size() == 1 && daily[0].sessions == 1 && daily[0].events == 2);
    }
    std::cout << "reduce tests passed (1100 randomized + boundary cases)\n";
}
