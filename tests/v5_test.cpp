#define main shopping_cli_main
#include "../src/main.cpp"
#undef main
#include <cassert>
#include <deque>
#include <random>

int main() {
    std::mt19937_64 random(490235);
    std::deque<std::string> ids;
    std::unordered_map<std::string,uint32_t> expected;
    std::vector<SV> exact_names;
    v5::PackedIds packed;
    for (size_t i=0;i<10000;++i) {
        std::string id="u";
        for (size_t n=random()%45;n;--n) id.push_back(char(random()%256));
        ids.push_back(id);
        auto [it,inserted]=expected.emplace(id,uint32_t(expected.size()));
        if (inserted) exact_names.push_back(ids.back());
        uint64_t key=v5::alias_key(ids.back());
        auto [value,added]=packed.add(key,it->second,[&](uint32_t previous) { return exact_names[previous]==id; });
        assert(value==it->second && added==inserted);
    }
    std::deque<std::string> event_ids;
    std::unordered_map<std::string,uint32_t> event_expected;
    std::vector<SV> event_names;
    v5::PackedIds event_keys;
    auto check_event_key=[&](std::string id) {
        event_ids.push_back(std::move(id));
        const auto& exact=event_ids.back();
        bool short_key=!exact.empty() && exact.front()=='e' && exact.size()<=9;
        uint64_t wanted=0;
        for (size_t i=1;i<exact.size();++i) {
            unsigned char byte=exact[i];
            if (!byte || byte>=128) short_key=false;
            if (i<=8) wanted|=uint64_t(byte)<<(8*(i-1));
        }
        uint64_t key=v5::event_key(exact);
        assert(bool(key>>63)==!short_key);
        assert(key==(short_key ? wanted : hash(exact)|(uint64_t(1)<<63)));
        auto [it,inserted]=event_expected.emplace(exact,uint32_t(event_expected.size()));
        if (inserted) event_names.push_back(exact);
        auto [value,added]=event_keys.add(key,it->second,[&](uint32_t previous) { return event_names[previous]==exact; });
        assert(value==it->second && added==inserted);
    };
    for (SV id:{"e","e0","e00","e000","e00000000","e12345678","e123456789","E1","u1",""})
        check_event_key(std::string(id));
    check_event_key(std::string("e\0",2));
    check_event_key(std::string("e\0x",3));
    check_event_key(std::string("e\x80",2));
    check_event_key(std::string("e\xff",2));
    for (size_t i=0;i<10000;++i) {
        std::string id=i%7 ? "e" : "E";
        size_t length=i%3 ? random()%9 : random()%45;
        for (size_t n=0;n<length;++n) id.push_back(char(i%3 ? 1+random()%127 : random()%256));
        check_event_key(std::move(id));
    }
    v5::PackedIds collided;
    std::array<SV,3> collision_names{"long-a","long-b","long-c"};
    for (uint32_t i=0;i<3;++i) {
        auto [value,added]=collided.add(uint64_t(1)<<63,i,[&](uint32_t previous) { return collision_names[previous]==collision_names[i]; });
        assert(value==i && added);
    }
    auto [same,added]=collided.add(uint64_t(1)<<63,3,[&](uint32_t previous) { return collision_names[previous]=="long-b"; });
    assert(same==1 && !added);
    for (size_t width:{32,64}) {
        if (width==64 && !__builtin_cpu_supports("avx512bw")) continue;
        for (size_t trial=0;trial<1000;++trial) {
            std::string input;
            std::vector<std::string> rows;
            for (size_t r=0;r<30;++r) {
                std::string row;
                for (size_t n=random()%100;n;--n) {
                    constexpr char chars[]="abc,\"\r \t09";
                    row+=chars[random()%(sizeof(chars)-1)];
                }
                rows.push_back(row); input+=row;
                if (r!=29 || trial%2) input+='\n';
            }
            v5::Scanner scanner({input.data(),input.data()+input.size()},width);
            for (const auto& row:rows) {
                if (!scanner.more()) { assert(row.empty() && &row==&rows.back()); break; }
                std::array<SV,4> slow{},fast{};
                bool valid=csv(SV(row),slow);
                assert(scanner.next(fast)==valid);
                if (valid) assert(slow==fast);
            }
            assert(!scanner.more());
        }
        // Bounded loads at a protected mapping end.
        size_t page=size_t(sysconf(_SC_PAGESIZE));
        char* memory=static_cast<char*>(mmap(nullptr,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        assert(memory!=MAP_FAILED);
        assert(mprotect(memory+page,page,PROT_NONE)==0);
        for (size_t padding=0;padding<128;++padding) {
            std::string row(padding,'x'); row+=",\"\",c,4";
            char* begin=memory+page-row.size(); std::memcpy(begin,row.data(),row.size());
            v5::Scanner scanner({begin,memory+page},width);
            std::array<SV,4> fields;
            assert(scanner.next(fields));
            assert(fields[0].size()==padding && fields[1].empty() && fields[3]=="4");
        }
        munmap(memory,page*2);
    }
    v5::Pool pool(4);
    std::atomic<size_t> jobs=0;
    for (size_t count:{0,1,1000,2,10000}) pool.run(count,[&](size_t,size_t) { ++jobs; });
    assert(jobs==11003);
    bool caught=false;
    try { pool.run(100,[](size_t i,size_t) { if (i==0) throw std::runtime_error("expected"); }); }
    catch (const std::runtime_error&) { caught=true; }
    assert(caught);
    pool.run(1,[&](size_t,size_t) { ++jobs; }); assert(jobs==11004);

    // Force long IDs with a shared prefix into one fingerprint bucket.
    std::vector<std::string> texts{
        "e-common-prefix-00000000000000000000000000000000,u1,1790812800000,2026-10-01T00:00:02Z,view,p,ignored\n",
        "e-common-prefix-00000000000000000000000000000000,u2,bad,2026-10-01T00:00:01Z,view,p,ignored\n",
        "e-common-prefix-00000000000000000000000000000001,u3,1790812800000,2026-10-01T00:00:01Z,view,p,ignored\n",
        "e-common-prefix-00000000000000000000000000000000,u4,1790812800000,2026-10-01T00:00:03Z,view,p,ignored\n"
    };
    Catalog catalog; sprint::Lookup products(catalog.products,"p"),campaigns(catalog.campaign_ids,"c_");
    std::vector<v5::Source> sources;
    for (size_t i=0;i<texts.size();++i) {
        v5::Source s{}; s.kind=v5::EVENTS; s.chunk={texts[i].data(),texts[i].data()+texts[i].size()};
        v5::scan(s,uint32_t(i),products,campaigns,1790812800000,1790812800000+DAY,32);
        assert(s.events.size()==1);
        for (auto& shard:s.descriptors) shard.clear();
        s.descriptors[0].push_back({42,0}); sources.push_back(std::move(s));
    }
    auto stats=v5::select(sources,0);
    assert(stats.duplicates==2 && stats.descriptors==4);
    assert(sources[0].events[0].time==UINT32_MAX && sources[1].events[0].time==UINT32_MAX);
    assert(sources[2].events[0].time==0 && sources[3].events[0].time==UINT32_MAX);
    // Packed zero keys and leading-zero suffixes stay distinct through exact selection.
    std::vector<std::string> short_texts{
        "e,u1,1790812800000,2026-10-01T00:00:02Z,view,p,ignored\n",
        "e0,u2,1790812800000,2026-10-01T00:00:01Z,view,p,ignored\n",
        "e00,u3,1790812800000,2026-10-01T00:00:01Z,view,p,ignored\n",
        "e,u4,bad,2026-10-01T00:00:01Z,view,p,ignored\n"
    };
    std::vector<v5::Source> short_sources;
    for (uint32_t i=0;i<short_texts.size();++i) {
        v5::Source s{}; s.kind=v5::EVENTS; s.chunk={short_texts[i].data(),short_texts[i].data()+short_texts[i].size()};
        v5::scan(s,i,products,campaigns,1790812800000,1790812800000+DAY,32);
        assert(s.events.size()==1);
        uint64_t key=v5::event_key(v5::event_id(s,s.events[0]));
        for (auto& shard:s.descriptors) shard.clear();
        s.descriptors[0].push_back({uint32_t(mix(key)),0}); short_sources.push_back(std::move(s));
    }
    auto short_stats=v5::select(short_sources,0);
    assert(short_stats.descriptors==4 && short_stats.duplicates==1);
    assert(short_sources[0].events[0].time==UINT32_MAX && short_sources[3].events[0].time==UINT32_MAX);
    assert(short_sources[1].events[0].time==0 && short_sources[2].events[0].time==0);
    // Distinct mixed keys sharing a table slot survive linear probing.
    std::vector<std::string> distinct{
        "x,u1,1790812800000,2026-10-01T00:00:01Z,view,p,ignored\n",
        "y,u2,1790812800000,2026-10-01T00:00:01Z,view,p,ignored\n",
        "z,u3,1790812800000,2026-10-01T00:00:01Z,view,p,ignored\n"
    };
    std::vector<v5::Source> false_hits;
    for (uint32_t i=0;i<distinct.size();++i) {
        v5::Source s{}; s.kind=v5::EVENTS; s.chunk={distinct[i].data(),distinct[i].data()+distinct[i].size()};
        v5::scan(s,i,products,campaigns,1790812800000,1790812800000+DAY,32);
        for (auto& shard:s.descriptors) shard.clear();
        uint32_t h=i==0 ? 42 : i==1 ? 42+32*128 : 42+2*128;
        s.descriptors[0].push_back({h,0}); false_hits.push_back(std::move(s));
    }
    auto filtered=v5::select(false_hits,0);
    assert(filtered.descriptors==3 && filtered.duplicates==0);
    for (const auto& s:false_hits) assert(s.events[0].time==0);
    // Exercise a large shard and the prefetch tail with an invalid earliest winner.
    std::string large_input;
    for (size_t i=0;i<40000;++i)
        large_input+="e"+std::to_string(i)+",u,1790812800000,2026-10-01T00:00:02Z,view,p,x\n";
    large_input+="e123,other,bad,2026-10-01T00:00:01Z,view,p,x\n";
    std::vector<v5::Source> large(1);
    large[0].kind=v5::EVENTS; large[0].chunk={large_input.data(),large_input.data()+large_input.size()};
    v5::scan(large[0],0,products,campaigns,1790812800000,1790812800000+DAY,32);
    assert(large[0].events.size()==40001);
    for (auto& shard:large[0].descriptors) shard.clear();
    for (uint32_t i=0;i<large[0].events.size();++i)
        large[0].descriptors[0].push_back({uint32_t(mix(v5::event_key(v5::event_id(large[0],large[0].events[i])))),i});
    auto large_stats=v5::select(large,0);
    assert(large_stats.descriptors==40001 && large_stats.duplicates==1);
    assert(large[0].events[123].time==UINT32_MAX && large[0].events.back().time==UINT32_MAX);
    size_t valid_large=0; for (const auto& event:large[0].events) valid_large+=event.time!=UINT32_MAX;
    assert(valid_large==39999);
    std::string links="u3,uZ\nuZ,A-link-only\nuZ,u3\nA-link-only,A-link-only\nempty,\nuClick,uClickAlias\nuClickAlias,B-click-only\n";
    v5::Source link_source{}; link_source.kind=v5::LINKS; link_source.chunk={links.data(),links.data()+links.size()};
    v5::scan(link_source,4,products,campaigns,1790812800000,1790812800000+DAY,32);
    assert(link_source.links.size()==6); sources.push_back(std::move(link_source));
    std::string click_text="k,uClick,c_0,1790812800000\n";
    v5::Source click_source{}; click_source.kind=v5::CLICKS;
    click_source.chunk={click_text.data(),click_text.data()+click_text.size()};
    click_source.clicks.push_back({0,1,2,6,0,1}); sources.push_back(std::move(click_source));
    v5::People people; people.build(sources,pool);
    const auto& row=sources[2].events[0];
    assert(v5::view(sources[2],row.user_offset,row.user_length)=="u3");
    const auto& click_row=sources[5].clicks[0];
    assert(v5::view(sources[5],click_row.user_offset,click_row.user_length)=="uClick");
    size_t events=0,clicks=0;
    uint32_t event_person=UINT32_MAX,click_person=UINT32_MAX;
    for (const auto& shard:people.shards) {
        for (const auto& event:shard.events) {
            ++events;
            assert(event.time==0 && event.purchase==0);
            event_person=people.parent[shard.base+event.user];
            assert(people.names[event_person]=="A-link-only");
        }
        for (const auto& click:shard.clicks) {
            ++clicks;
            assert(click.ref.source==5 && click.ref.row==0);
            click_person=people.parent[shard.base+click.user];
            assert(people.names[click_person]=="B-click-only");
        }
    }
    assert(events==1 && clicks==1 && event_person!=click_person);
    size_t owners=0,roots=0;
    for (size_t part=0;part<PARTS;++part) for (size_t index=0;index<people.owners[part].size();++index) {
        uint32_t person=people.owners[part][index]; ++owners;
        assert(people.parent[person]==person && (mix(person)&(PARTS-1))==part);
        assert(people.dense[person]==index);
    }
    for (uint32_t person=0;person<people.parent.size();++person) roots+=people.parent[person]==person;
    assert(owners==roots);
    assert(people.owners[mix(event_person)&(PARTS-1)][people.dense[event_person]]==event_person);
    assert(people.owners[mix(click_person)&(PARTS-1)][people.dense[click_person]]==click_person);
    std::cout<<"v5 tests passed: 60000 differential CSV rows, guard pages, pool, full-hash collisions, invalid earliest, linked IDs, dense owners\n";
}
