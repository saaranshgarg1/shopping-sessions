#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace ss {
using SV = std::string_view;
constexpr int64_t DAY = 86400000, WEEK = 7 * DAY, GAP = 1800000;

inline uint64_t mix(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
inline uint64_t hash(SV s) {
    uint64_t h = s.size() * 0x9e3779b97f4a7c15ULL;
    while (s.size() >= 8) {
        uint64_t word; std::memcpy(&word, s.data(), 8);
        h = mix(h ^ word); s.remove_prefix(8);
    }
    uint64_t last = 0; if (!s.empty()) std::memcpy(&last, s.data(), s.size());
    return mix(h ^ last);
}
struct Hasher { size_t operator()(SV s) const { return hash(s); } };

// Exact bytes resolve every fingerprint collision; immutable lookups are thread-safe.
class FlatIds {
    struct Entry { SV key; uint32_t value = 0, fingerprint = 0; };
    std::vector<Entry> table;
    size_t used = 0;
    void grow() {
        auto old = std::move(table);
        table.resize(std::max<size_t>(16,old.size()*2)); used = 0;
        for (const auto& e:old) if (e.fingerprint) insert(e.key,e.value);
    }
public:
    void reserve(size_t n) {
        size_t slots = 16;
        while (slots < n+n/2) slots *= 2;
        if (slots > table.size()) {
            auto old = std::move(table); table.resize(slots); used = 0;
            for (const auto& e:old) if (e.fingerprint) insert(e.key,e.value);
        }
    }
    uint32_t get(SV key,uint32_t fallback = UINT32_MAX) const {
        if (table.empty()) return fallback;
        uint64_t h = hash(key); uint32_t fp = uint32_t(h >> 32) | 1;
        size_t at = h & (table.size()-1);
        for (;;) {
            const auto& e = table[at];
            if (!e.fingerprint) return fallback;
            if (e.fingerprint == fp && e.key == key) return e.value;
            at = (at+1) & (table.size()-1);
        }
    }
    template<class F> void each(F fn) const { for (const auto& e:table) if (e.fingerprint) fn(e.key,e.value); }
    // Returns the stored value and whether this exact key was newly inserted.
    std::pair<uint32_t,bool> insert(SV key,uint32_t value) {
        if (table.empty() || (used+1)*3 > table.size()*2) grow();
        uint64_t h = hash(key); uint32_t fp = uint32_t(h >> 32) | 1;
        size_t at = h & (table.size()-1);
        for (;;) {
            auto& e = table[at];
            if (!e.fingerprint) { e = {key,value,fp}; ++used; return {value,true}; }
            if (e.fingerprint == fp && e.key == key) return {e.value,false};
            at = (at+1) & (table.size()-1);
        }
    }
};

template<size_t N> bool csv(SV line, std::array<SV, N>& fields) {
    size_t start = 0;
    for (size_t i = 0; i < N; ++i) {
        const void* comma = std::memchr(line.data() + start, ',', line.size() - start);
        size_t end = comma ? static_cast<const char*>(comma) - line.data() : line.size();
        if ((i + 1 < N) != (comma != nullptr)) return false;
        SV field = line.substr(start, end - start);
        if (!field.empty() && field.front() == '"') {
            if (field.size() < 2 || field.back() != '"') return false;
            field.remove_prefix(1); field.remove_suffix(1);
        }
        if (field.find('"') != SV::npos) return false;
        fields[i] = field; start = end + 1;
    }
    return true;
}
inline bool digits(SV s, uint64_t max, uint64_t& value) {
    if (s.empty()) return false;
    value = 0;
    for (unsigned char c : s) {
        if (c < '0' || c > '9') return false;
        unsigned d = c - '0';
        if (value > max / 10 || (value == max / 10 && d > max % 10)) return false;
        value = value * 10 + d;
    }
    return true;
}
// Proleptic Gregorian calendar, with floor division for dates before 1970.
inline int64_t civil_days(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yo = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    return int64_t(era) * 146097 + yo * 365 + yo / 4 - yo / 100 + doy - 719468;
}
inline int64_t day_of(int64_t ms) {
    return ms / DAY - (ms < 0 && ms % DAY != 0);
}
inline bool timestamp(SV s, int64_t& out) {
    if (s.size() != 20 && s.size() != 25) {
        uint64_t value;
        if (!digits(s, uint64_t(std::numeric_limits<int64_t>::max()), value)) return false;
        out = static_cast<int64_t>(value); return true;
    }
    // A 20- or 25-digit epoch is valid too; leading zeros can put it in-window.
    if (s[4] != '-') {
        uint64_t value;
        if (!digits(s, uint64_t(std::numeric_limits<int64_t>::max()), value)) return false;
        out = static_cast<int64_t>(value); return true;
    }
    if (s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':') return false;
    constexpr int positions[] = {0,1,2,3,5,6,8,9,11,12,14,15,17,18};
    for (int p : positions) if (s[p] < '0' || s[p] > '9') return false;
    auto two = [&](int p) { return (s[p]-'0')*10 + s[p+1]-'0'; };
    int y = two(0)*100 + two(2), m = two(5), d = two(8);
    int hour = two(11), minute = two(14), second = two(17), offset = 0;
    constexpr int months[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (y < 1 || m < 1 || m > 12 || d < 1 || hour > 23 || minute > 59 || second > 59) return false;
    int md = months[m-1] + (m == 2 && y%4 == 0 && (y%100 != 0 || y%400 == 0));
    if (d > md) return false;
    if (s.size() == 20) { if (s[19] != 'Z') return false; }
    else {
        if ((s[19] != '+' && s[19] != '-') || s[22] != ':') return false;
        for (int p : {20,21,23,24}) if (s[p] < '0' || s[p] > '9') return false;
        int oh = two(20), om = two(23);
        if (oh > 23 || om > 59) return false;
        offset = (oh * 60 + om) * 60 * (s[19] == '+' ? 1 : -1);
    }
    out = (civil_days(y,m,d)*86400 + hour*3600 + minute*60 + second - offset)*1000;
    return true;
}
inline bool timestamp_fast(SV s,int64_t& out) {
    if (s.size()==13) {
        uint64_t value=0;
        for (unsigned char c:s) { unsigned d=c-'0'; if (d>9) return false; value=value*10+d; }
        out=int64_t(value); return true;
    }
    if ((s.size()!=20 && s.size()!=25) || s[4]!='-') return timestamp(s,out);
    struct Date { uint64_t prefix=0; uint16_t suffix=0; int64_t midnight=0; };
    static thread_local std::array<Date,64> dates;
    uint64_t prefix; uint16_t suffix;
    std::memcpy(&prefix,s.data(),8); std::memcpy(&suffix,s.data()+8,2);
    auto& date=dates[(suffix^(prefix>>40))&63];
    if (date.prefix!=prefix || date.suffix!=suffix) {
        if (!timestamp(s,out)) return false;
        int y=(s[0]-'0')*1000+(s[1]-'0')*100+(s[2]-'0')*10+s[3]-'0';
        int m=(s[5]-'0')*10+s[6]-'0',d=(s[8]-'0')*10+s[9]-'0';
        date={prefix,suffix,civil_days(y,m,d)*DAY};
        return true;
    }
    if (s[10]!='T' || s[13]!=':' || s[16]!=':') return false;
    for (int p:{11,12,14,15,17,18}) if (unsigned(s[p]-'0')>9) return false;
    auto two=[&](int p) { return (s[p]-'0')*10+s[p+1]-'0'; };
    int h=two(11),m=two(14),sec=two(17),offset=0;
    if (h>23 || m>59 || sec>59) return false;
    if (s.size()==20) { if (s[19]!='Z') return false; }
    else {
        if ((s[19]!='+' && s[19]!='-') || s[22]!=':') return false;
        for (int p:{20,21,23,24}) if (unsigned(s[p]-'0')>9) return false;
        int oh=two(20),om=two(23); if (oh>23 || om>59) return false;
        offset=(oh*3600+om*60)*(s[19]=='+' ? 1 : -1);
    }
    out=date.midnight+(h*3600+m*60+sec-offset)*1000LL; return true;
}
struct Event { SV user; int64_t ts; uint32_t category, price; };
struct Click { SV user, id; int64_t ts; uint32_t campaign; };
struct Session { int64_t start, revenue; };
struct Stats {
    int64_t sessions = 0, purchasing = 0, revenue = 0, assisted = 0;
    Stats& operator+=(const Stats& b) {
        sessions += b.sessions; purchasing += b.purchasing;
        revenue += b.revenue; assisted += b.assisted; return *this;
    }
};

// Per-campaign interval union over session indices: no session-by-click join.
template<class ClickRow>
inline void attribute(const std::vector<Session>& sessions, const ClickRow* clicks, size_t count,
                      int64_t first_day, size_t days, std::vector<Stats>& stats,
                      std::vector<size_t>& covered) {
    size_t latest = 0;
    for (const auto& session : sessions) {
        while (latest < count && clicks[latest].ts <= session.start) ++latest;
        uint32_t campaign = 0;
        if (latest && session.start - clicks[latest-1].ts <= WEEK) campaign = clicks[latest-1].campaign;
        auto& a = stats[campaign * days + day_of(session.start) - first_day];
        ++a.sessions; a.purchasing += session.revenue > 0; a.revenue += session.revenue;
    }
    if (!count) return;
    // Reset only campaigns this person used, not the entire campaign catalog.
    for (size_t i = 0; i < count; ++i) covered[clicks[i].campaign] = 0;
    std::vector<size_t> day_end(days, 0);
    for (size_t i = 0; i < sessions.size(); ++i) day_end[day_of(sessions[i].start)-first_day] = i+1;
    size_t l = 0, r = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto& click = clicks[i];
        while (l < sessions.size() && sessions[l].start < click.ts) ++l;
        r = std::max(r,l);
        // The window's finite timestamps make addition safe after validation.
        while (r < sessions.size() && sessions[r].start - click.ts <= WEEK) ++r;
        size_t from = std::max(l, covered[click.campaign]);
        covered[click.campaign] = std::max(covered[click.campaign],r);
        while (from < r) {
            auto day = day_of(sessions[from].start) - first_day;
            size_t end = std::min(r,day_end[day]);
            stats[click.campaign * days + day].assisted += end - from;
            from = end;
        }
    }
}
} // namespace ss
