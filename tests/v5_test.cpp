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

    // Force distinct byte IDs to share a full 64-bit hash.
    std::vector<std::string> texts{
        "same,u1,1790812800000,2026-10-01T00:00:02Z,view,p,ignored\n",
        "same,u2,bad,2026-10-01T00:00:01Z,view,p,ignored\n",
        "other,u3,1790812800000,2026-10-01T00:00:01Z,view,p,ignored\n",
        "same,u4,1790812800000,2026-10-01T00:00:03Z,view,p,ignored\n"
    };
    Catalog catalog; sprint::Lookup products(catalog.products,"p"),campaigns(catalog.campaign_ids,"c_");
    std::vector<v5::Source> sources;
    for (size_t i=0;i<texts.size();++i) {
        v5::Source s{}; s.kind=v5::EVENTS; s.chunk={texts[i].data(),texts[i].data()+texts[i].size()};
        v5::scan(s,uint32_t(i),products,campaigns,1790812800000,1790812800000+DAY,32);
        assert(s.events.size()==1);
        for (auto& shard:s.descriptors) shard.clear();
        s.descriptors[0].push_back({42,{uint32_t(i),0}}); sources.push_back(std::move(s));
    }
    auto stats=v5::select(sources,0);
    assert(stats.duplicates==2 && stats.descriptors==4);
    assert(sources[0].events[0].time==UINT32_MAX && sources[1].events[0].time==UINT32_MAX);
    assert(sources[2].events[0].time==0 && sources[3].events[0].time==UINT32_MAX);
    // False repeat-bit hits stay distinct; an unflagged singleton also survives.
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
        uint64_t h=i==0 ? 42 : i==1 ? 42+32*128 : 42+2*128;
        s.descriptors[0].push_back({h,{i,0}}); false_hits.push_back(std::move(s));
    }
    auto filtered=v5::select(false_hits,0);
    assert(filtered.descriptors==3 && filtered.duplicates==0);
    for (const auto& s:false_hits) assert(s.events[0].time==0);
    std::string links="u3,uZ\nuZ,A-link-only\nuZ,u3\nA-link-only,A-link-only\nempty,\n";
    v5::Source link_source{}; link_source.kind=v5::LINKS; link_source.chunk={links.data(),links.data()+links.size()};
    v5::scan(link_source,4,products,campaigns,1790812800000,1790812800000+DAY,32);
    assert(link_source.links.size()==4); sources.push_back(std::move(link_source));
    v5::People people; people.build(sources,pool);
    auto& row=sources[2].events[0];
    auto person=people.person(row.user_offset,row.user_length);
    assert(people.names[person]=="A-link-only");
    std::cout<<"v5 tests passed: 60000 differential CSV rows, guard pages, pool, full-hash collisions, invalid earliest, linked IDs\n";
}
