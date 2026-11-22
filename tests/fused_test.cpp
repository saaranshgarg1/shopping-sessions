#define main shopping_cli_main
#include "../src/main.cpp"
#undef main
#include <cassert>
#include <deque>
#include <random>

int main() {
    if (!__builtin_cpu_supports("sse4.2")) return 0;
    v6::ExactNames map;
    std::deque<std::string> names;
    std::mt19937_64 rng(819325);
    for (size_t n=0;n<10000;++n) {
        names.push_back(std::to_string(n)+std::string(n%43,char('a'+n%26)));
        SV key=names.back(); uint64_t hash=v6::key_hash(key);
        auto index=map.add(key,hash);
        assert(map.find(key,hash)==index && map.add(key,hash)==index);
    }
    // Fingerprint equality never merges different full bytes or lengths.
    v6::ExactNames collision;
    assert(collision.add("u",42)==0 && collision.add(SV("u\0",2),42)==1);
    assert(collision.add("abcdefgh-long-1",42)==2 && collision.add("abcdefgh-long-2",42)==3);
    assert(collision.find("abcdefgh-long-2",42)==3 && collision.find("missing",42)==UINT32_MAX);
    v5::Pool pool(4);
    std::vector<std::vector<v6::Link>> links{{{"long-alias","u"},{"u","A-link-only"}},{{"u","long-alias"}}};
    v6::Linked linked; linked.build(links,pool);
    auto a=linked.find("long-alias",v6::key_hash("long-alias"));
    auto b=linked.find("u",v6::key_hash("u"));
    assert(a==b && linked.names[a]=="A-link-only");
    v6::UserCache cache(3);
    assert(cache.get("u",linked)==a);
    auto one=cache.get("unlinked",linked); assert(one>>63 && cache.get("unlinked",linked)==one);

    std::vector<std::string> lines{
        "same,u,1,2026-10-01T00:00:02Z,view,p,x",
        "same,v,bad,2026-10-01T00:00:01Z,view,p,x",
        "different,u,1,2026-10-01T00:00:01Z,view,p,x",
        "same,z,1,2026-10-01T00:00:03Z,view,p,x"
    };
    std::vector<v6::Source> sources(lines.size());
    for (uint32_t i=0;i<lines.size();++i) {
        auto& s=sources[i]; s.chunk={lines[i].data(),lines[i].data()+lines[i].size()};
        auto comma=lines[i].find(','); auto ingest=lines[i].find("2026");
        s.rows.push_back({0,uint32_t(comma),uint32_t(ingest),0,0,i==1?UINT32_MAX:0,0});
        s.descriptors[0].push_back({42,{i,0}});
    }
    v6::select(sources,0);
    assert(sources[0].rows[0].time==UINT32_MAX && sources[1].rows[0].time==UINT32_MAX);
    assert(sources[2].rows[0].time==0 && sources[3].rows[0].time==UINT32_MAX);
    std::cout<<"fused tests passed: exact packed/long IDs, linked names, cache, forced prefilter collisions\n";
}
