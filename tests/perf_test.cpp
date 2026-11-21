#define main shopping_pipeline_main
#include "../src/main.cpp"
#undef main
#include <cassert>
#include <random>
#include <tuple>

int main() {
    std::vector<std::string> keys{"", "u", "U", " u", "u ", "ü", std::string("u\0x",3)};
    keys.reserve(100007);
    for (int i=0;i<100000;++i) keys.push_back("arbitrary-long-user-"+std::to_string(i));
    FlatIds ids;
    for (uint32_t i=0;i<keys.size();++i) {
        auto inserted=ids.insert(keys[i],i);
        assert(inserted.second && inserted.first==i);
    }
    ids.reserve(300000);
    for (uint32_t i=0;i<keys.size();++i) {
        assert(ids.get(keys[i])==i);
        auto duplicate=ids.insert(keys[i],999999);
        assert(!duplicate.second && duplicate.first==i);
    }
    assert(ids.get("missing")==UINT32_MAX);
    std::mt19937_64 rng(67819);
    std::vector<std::string> timestamps{
        "2028-02-29T00:00:00Z", "2028-02-29T23:59:59+23:59", "2028-02-29T00:00:00-00:00",
        "2027-02-29T00:00:00Z", "2028-02-30T00:00:00Z", "0001-01-01T00:00:00Z",
        "9999-12-31T23:59:59-23:59", "00000001791071890000", std::string(100,'0')+"1",
        "86400000", "1791071890000", "2028-02-29T00:00:00Z\r"
    };
    auto check_time=[](SV s) {
        int64_t slow=0,fast=0; bool a=timestamp(s,slow),b=timestamp_fast(s,fast);
        assert(a==b && (!a || slow==fast));
    };
    for (const auto& s:timestamps) for (int repeat=0;repeat<5;++repeat) check_time(s);
    for (int i=0;i<100000;++i) {
        auto s=timestamps[rng()%timestamps.size()];
        if (i%2) s[rng()%s.size()]=char(rng()%256);
        check_time(s);
    }
    std::vector<CompactEvent> rows;
    for (int i=0;i<30000;++i) rows.push_back({uint32_t(rng()),uint32_t(rng()%500),uint32_t(rng()%20),uint32_t(rng())});
    auto wanted=rows;
    auto tuple=[](const auto& r) { return std::tuple(r.user,r.ts,r.category,r.price); };
    std::sort(wanted.begin(),wanted.end(),[&](auto a,auto b) { return tuple(a)<tuple(b); });
    sort_events(rows);
    for (size_t i=1;i<rows.size();++i)
        assert(rows[i-1].user<rows[i].user || (rows[i-1].user==rows[i].user && rows[i-1].ts<=rows[i].ts));
    std::sort(rows.begin(),rows.end(),[&](auto a,auto b) { return tuple(a)<tuple(b); });
    for (size_t i=0;i<rows.size();++i) assert(tuple(rows[i])==tuple(wanted[i]));

    Catalog catalog;
    catalog.categories.push_back("category");
    catalog.campaigns.push_back("campaign"); catalog.channels.push_back("channel");
    auto check_aggregate=[&](int64_t span) {
        Partition<Event> ep; Partition<Click> cp;
        std::vector<Event> e{{"u",0,1,100},{"u",GAP,0,0},{"u",DAY+1,1,500},
                             {" U",2,0,0},{"u ",span-1,1,300}};
        std::vector<Click> c{{"u","z",-WEEK,1},{"u","a",-WEEK,1},{"u ","long-id",span-1,1}};
        ep.add(std::vector<Event>(e)); cp.add(std::vector<Click>(c));
        size_t days=day_of(span-1)+1;
        auto got=aggregate(ep,cp,catalog,0,days,0,span);
        auto expected=aggregate_rows(std::move(e),std::move(c),catalog,0,days,0,[](SV id) {return id;});
        auto order=[](const UserRow& r) {return std::tuple(r.user,r.day,r.sessions,r.events,r.revenue,r.category,r.has_category);};
        auto sort=[&](auto& v) {std::sort(v.begin(),v.end(),[&](auto a,auto b) {return order(a)<order(b);});};
        sort(got.users); sort(expected.users);
        assert(got.users.size()==expected.users.size() && got.campaigns.size()==expected.campaigns.size());
        for (size_t i=0;i<got.users.size();++i) assert(order(got.users[i])==order(expected.users[i]));
        for (size_t i=0;i<got.campaigns.size();++i) {
            auto a=got.campaigns[i],b=expected.campaigns[i];
            assert(a.sessions==b.sessions && a.purchasing==b.purchasing && a.revenue==b.revenue && a.assisted==b.assisted);
        }
    };
    check_aggregate(14*DAY);
    check_aggregate(60*DAY); // Full-width fallback, not a truncated relative timestamp.
    std::cout<<"performance tests passed: exact IDs, 100000 timestamp mutations, radix, compact/wide aggregate\n";
}
