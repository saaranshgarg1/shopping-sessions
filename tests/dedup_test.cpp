#define main shopping_pipeline_main
#include "../src/main.cpp"
#undef main
#include <cassert>
#include <random>

int main() {
    using namespace sprint;
    // End every field at a guard page: bounded ID loads must not touch the next byte.
    size_t page=sysconf(_SC_PAGESIZE);
    char* guarded=static_cast<char*>(mmap(nullptr,page*2,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    assert(guarded!=MAP_FAILED && mprotect(guarded,page,PROT_READ|PROT_WRITE)==0);
    std::mt19937_64 random(211120267);
    for (char prefix:{'u','e','k'}) for (size_t n=0;n<=8;++n) for (unsigned round=0;round<2000;++round) {
        char* p=guarded+page-n-1; p[0]=prefix;
        bool valid=true; uint64_t expected=0;
        for (size_t i=1;i<=n;++i) {
            unsigned char c=round%2 ? 1+random()%127 : random()%256;
            p[i]=char(c); valid&=c && c<128;
            expected|=uint64_t(c)<<(64-8*i);
        }
        bool accepted=true; uint64_t actual=0;
        try { actual=key(SV(p,n+1),prefix); } catch (const Unsupported&) { accepted=false; }
        assert(accepted==valid && (!valid || actual==expected));
    }
    munmap(guarded,page*2);
    Keys exact_keys('k');
    std::vector<std::string> byte_ids{"k", "kA", "kAA", "kZ", "ka", "k12345678",
                                      "other", "k123456789", "k\xc3\xa9", std::string("kA\0B",4)};
    for (const auto& a:byte_ids) for (const auto& b:byte_ids)
        assert(exact_keys.less(exact_keys.get(a),exact_keys.get(b))==(a<b));
    DuplicateFilter filter(64);
    const uint64_t repeated_id=key("eRepeat",'e');
    parallel(128,8,[&](size_t,size_t) { filter.add(repeated_id); });
    assert(filter.candidate(repeated_id));
    uint64_t first=1,collision=2;
    while ((mix(first)&63)!=(mix(collision)&63)) ++collision;
    DuplicateFilter collisions(64);
    collisions.add(first);
    assert(!collisions.candidate(first));
    collisions.add(collision);
    assert(collisions.candidate(first) && collisions.candidate(collision));
    Keys ids('e');
    uint64_t generic=ids.get("generic-long-event-id"),nul=ids.get(SV("e\0",2));
    assert(generic!=nul && generic!=key("e",'e'));
    DuplicateFilter exact(64);
    exact.add(generic); exact.add(generic); exact.add(nul); exact.add(nul);
    assert(exact.candidate(generic) && exact.candidate(nul));
    std::array<Partition<RawEvent>,PARTS> raw;
    std::array<Partition<sprint::Event>,PARTS> events;
    std::array<Partition<sprint::Click>,PARTS> clicks;
    // Later valid copy and earlier invalid copy name different people and arrive in reverse order.
    raw[0].add({{repeated_id,2,1000,"2028-03-01T00:00:00Z",0,500},
                {repeated_id,0,0,"2028-02-29T23:59:59Z",0,INVALID}});
    sprint::dedup(raw,8,events);
    for (auto& part:events) assert(part.flatten().empty());
    // Equal-ingest conflicting copies must still invoke the existing arbitrary-shape fallback.
    raw[0].add({{repeated_id,1,1000,"2028-02-29T23:59:59Z",0,100},
                {repeated_id,2,1000,"2028-02-29T23:59:59Z",0,100}});
    bool unsupported=false;
    try { sprint::dedup(raw,8,events); }
    catch (const Unsupported&) { unsupported=true; }
    assert(unsupported);
    // Thousands of cross-file quoted/unquoted copies still count each campaign/session once.
    char temporary[]="/tmp/shopping-click-copies.XXXXXX";
    assert(mkdtemp(temporary)); fs::path folder(temporary);
    Keys user_keys('u'),click_keys('k'),event_keys('e'); People people(user_keys);
    people.join(user_keys.get("uA"),user_keys.get("uB")); people.finish();
    FlatIds product_ids,campaign_ids; campaign_ids.insert("c_1",1); campaign_ids.insert("c_2",2);
    Lookup product_lookup(product_ids,"p"),campaign_lookup(campaign_ids,"c_");
    for (unsigned file=0;file<12;++file) {
        std::ofstream out(folder/(std::to_string(file)+".csv"));
        out<<"click_id,user_id,campaign_id,ts\n";
        for (unsigned copy=0;copy<300;++copy) {
            out<<(copy%2 ? "\"kA\",\"uB\",\"c_2\",\"2028-02-29T00:00:00Z\"\n" :
                              "kA,uB,c_2,2028-02-29T00:00:00Z\n");
            out<<"kZ,uA,c_1,2028-02-29T00:00:00Z\n";
        }
        out<<"kBad,uA,c_missing,2028-02-29T00:00:00Z\n";
    }
    auto maps=map_files(files(folder)); int64_t midnight=0;
    assert(timestamp("2028-02-29T00:00:00Z",midnight));
    parse<false>(maps,8,people,click_keys,user_keys,product_lookup,campaign_lookup,
                 midnight,midnight+DAY,raw,clicks);
    size_t parsed=0;
    for (auto& part:clicks) {
        auto rows=part.flatten(); parsed+=rows.size();
        if (rows.empty()) continue;
        assert(rows.size()==7200);
        for (const auto& row:rows) assert(row.user==user_keys.get("uA"));
        std::sort(rows.begin(),rows.end(),[&](const auto& a,const auto& b) {
            return a.ts==b.ts ? click_keys.less(b.id,a.id) : a.ts<b.ts;
        });
        std::vector<Stats> stats(3); std::vector<size_t> covered(3);
        attribute(std::vector<Session>{{midnight,100},{midnight+GAP+1,0}},rows.data(),rows.size(),
                  day_of(midnight),1,stats,covered);
        assert(stats[1].sessions==0 && stats[2].sessions==2 && stats[2].revenue==100);
        assert(stats[1].assisted==2 && stats[2].assisted==2);
    }
    assert(parsed==7200); fs::remove_all(folder);
    FlatIds catalog; catalog.insert("p1",7); catalog.insert("p01",9); catalog.insert("p500000000",11);
    Lookup lookup(catalog,"p");
    assert(lookup.get("p1",0)==7 && lookup.get("p01",0)==9);
    assert(lookup.get("p500000000",0)==11 && lookup.get("p2",0)==0);
    // Generic fallback must select physical first copy, not worker completion order.
    std::array<RawPart,PARTS> base_raw;
    std::array<Partition<ss::Event>,PARTS> base_events;
    std::array<Partition<ss::Click>,PARTS> base_clicks;
    std::string first_row="same,uA,1,2,view,p0,0",second_row="same,uB,1,2,view,p0,0";
    Raw a{},b{}; a.line=first_row.data(); b.line=second_row.data();
    a.length=first_row.size(); b.length=second_row.size(); a.fingerprint=b.fingerprint=hash("same");
    a.user="uA"; b.user="uB"; a.ts=b.ts=1; a.ingest=b.ingest=2;
    base_raw[0].blocks.emplace_back(1,std::vector<Raw>{b});
    base_raw[0].blocks.emplace_back(0,std::vector<Raw>{a});
    ::dedup<true>(base_raw,2,base_events,base_clicks);
    auto winners=base_events[hash("uA")&(PARTS-1)].flatten();
    assert(winners.size()==1 && winners[0].user=="uA");
    DateCache cache;
    auto check_late=[&](SV event,SV arrival) {
        int64_t ts=0,ingest=0;
        assert(timestamp(event,ts) && timestamp(arrival,ingest));
        bool expected=!(ingest>ts && uint64_t(ingest)-uint64_t(ts)>uint64_t(DAY));
        assert(on_time(cache,event,arrival,ts)==expected);
    };
    check_late("2028-02-29T00:00:00Z","2028-02-29T23:59:59Z");
    check_late("2028-02-29T23:59:59Z","2028-02-29T00:00:00Z");
    check_late("2028-02-29T23:59:59Z","2028-03-01T23:59:59Z");
    check_late("2028-02-29T23:59:59Z","2028-03-02T00:00:00Z");
    check_late("2028-02-29T00:00:00+01:00","2028-02-29T23:30:00Z");
    check_late("1835395200000","2028-02-29T23:59:59Z");
    // Separate allocations: comparing pointers would pick the wrong calendar winner.
    std::vector<std::string> arrivals{"2028-03-01T00:00:00Z","2028-02-29T23:59:59Z",
                                      "2029-01-01T00:00:00Z","2028-12-31T23:59:59Z"};
    for (const auto& a:arrivals) for (const auto& b:arrivals) {
        int64_t x=0,y=0; assert(timestamp(a,x) && timestamp(b,y));
        assert((std::memcmp(a.data(),b.data(),20)<0)==(x<y));
    }
    for (SV value:{"2028-02-29T23:59:59Z","2027-02-29T23:59:59Z","9223372036854775807"}) {
        int64_t fast=0,slow=0; bool ok=timestamp(value,slow);
        assert(cache.parse(value,fast)==ok && (!ok || fast==slow));
    }
    std::cout<<"dedup bitmap race/collision/exact-key/invalid-winner/tie/catalog/fallback self-check PASS\n";
}
