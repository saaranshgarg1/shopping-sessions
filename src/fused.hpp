#pragma once
#include "identity.hpp"
#include "scanner.hpp"

namespace v6 {
struct UserCache {
    struct Entry { SV key; uint64_t hash=0,id=0,packed=0; };
    std::array<Entry,2048> cache{};
    ExactNames singletons;
    uint32_t worker;
    explicit UserCache(uint32_t t):worker(t) {}
    uint64_t get(SV key,const Linked& linked) {
        uint64_t h=key_hash(key); auto& slot=cache[uint32_t(h)&(cache.size()-1)];
        uint64_t packed=ExactNames::pack(key);
        if (slot.hash==h && slot.key.size()==key.size() && (key.size()<=8 ? slot.packed==packed : slot.key==key)) return slot.id;
        uint32_t found=linked.find(key,h);
        uint64_t id=found!=UINT32_MAX ? found : (uint64_t(1)<<63)|(uint64_t(worker)<<32)|singletons.add(key,h);
        slot={key,h,id,packed}; return id;
    }
};
struct RawEvent { uint64_t user; uint32_t time,purchase; };
struct RawClick { uint64_t user; SV id; int32_t time; uint32_t campaign; };
struct Source {
    Chunk chunk;
    std::vector<v5::EventRow> rows;
    std::vector<RawEvent> events;
    std::vector<RawClick> clicks;
    std::vector<v5::Purchase> purchases;
    std::array<std::vector<v5::Descriptor>,PARTS> descriptors;
    uint32_t purchase_base=0;
    uint64_t physical_rows=0;
};
inline SV id(const Source& source,const v5::EventRow& row) { return {source.chunk.begin+row.id_offset,row.id_length}; }
inline void select(std::vector<Source>& sources,size_t shard) {
    size_t count=0; for (const auto& source:sources) count+=source.descriptors[shard].size();
    if (!count) return;
    // A completed repeat prefilter only bypasses singleton hash buckets, never collisions.
    size_t bits=16; while (bits<count*8) bits*=2;
    std::vector<uint64_t> seen((bits+63)/64),repeat(seen.size());
    for (const auto& source:sources) for (const auto& d:source.descriptors[shard]) {
        size_t at=(d.hash>>7)&(bits-1); uint64_t bit=uint64_t(1)<<(at&63);
        repeat[at>>6]|=seen[at>>6]&bit; seen[at>>6]|=bit;
    }
    std::vector<v5::Descriptor> candidates;
    candidates.reserve(count/4);
    for (auto& source:sources) {
        for (const auto& d:source.descriptors[shard]) {
            size_t at=(d.hash>>7)&(bits-1);
            if (repeat[at>>6]&(uint64_t(1)<<(at&63))) candidates.push_back(d);
        }
        std::vector<v5::Descriptor>().swap(source.descriptors[shard]);
    }
    count=candidates.size();
    if (count>=UINT32_MAX) throw std::runtime_error("event shard exceeds 32-bit index");
    size_t capacity=16; while (capacity<count+count/2) capacity*=2;
    std::vector<uint32_t> table(capacity);
    for (size_t i=0;i<count;++i) {
        const auto& d=candidates[i]; auto& source=sources[d.ref.source]; auto& row=source.rows[d.ref.row];
        size_t at=mix(d.hash)&(capacity-1);
        while (table[at]) {
            const auto& old_d=candidates[table[at]-1]; auto& old_source=sources[old_d.ref.source];
            auto& old=old_source.rows[old_d.ref.row];
            if (d.hash==old_d.hash && id(source,row)==id(old_source,old)) {
                if (std::memcmp(source.chunk.begin+row.ingest_offset,old_source.chunk.begin+old.ingest_offset,20)<0) {
                    old.time=UINT32_MAX; table[at]=uint32_t(i+1);
                } else row.time=UINT32_MAX;
                break;
            }
            at=(at+1)&(capacity-1);
        }
        if (!table[at]) table[at]=uint32_t(i+1);
    }
}
template<int WIDTH> inline void scan(Source& source,uint32_t index,bool events,UserCache& users,const Linked& linked,
                 const sprint::Lookup& products,const sprint::Lookup& campaigns,int64_t start,int64_t end) {
    Scanner<WIDTH> scanner(source.chunk.begin,source.chunk.end); sprint::DateCache cache; v5::IngestCache ingest;
    if (events) {
        size_t estimate=size_t(source.chunk.end-source.chunk.begin)/80;
        source.rows.reserve(estimate); source.events.reserve(estimate);
        for (auto& bucket:source.descriptors) bucket.reserve(estimate/PARTS+16);
        scanner.template run<7>([&](bool parsed,const std::array<SV,7>& f) {
            ++source.physical_rows;
            if (!parsed || f[0].empty()) return;
            auto off=[&](SV s) { auto v=s.data()-source.chunk.begin; if (v>UINT32_MAX || s.size()>UINT32_MAX) throw std::runtime_error("CSV chunk too large"); return uint32_t(v); };
            v5::EventRow row{off(f[0]),uint32_t(f[0].size()),off(f[3]),0,0,UINT32_MAX,0};
            int64_t time; uint64_t price=0;
            bool valid=!f[1].empty() && !f[5].empty() && cache.parse(f[2],time) && time>=start && time<end;
            if (valid) {
                if (f[4]=="purchase") valid=digits(f[6],10000000,price) && price;
                else valid=f[4]=="view" || f[4]=="cart";
            }
            RawEvent payload{};
            if (valid && !ingest.late(f[3],time,cache)) {
                payload.user=users.get(f[1],linked); row.time=payload.time=uint32_t(time-start);
                if (price) {
                    if (source.purchases.size()>=UINT32_MAX-1) throw std::runtime_error("too many purchases");
                    source.purchases.push_back({products.get(f[5],0),uint32_t(price)});
                    payload.purchase=uint32_t(source.purchases.size());
                }
            }
            if (source.rows.size()>=UINT32_MAX) throw std::runtime_error("too many event rows");
            uint64_t h=key_hash(f[0]);
            source.descriptors[h&(PARTS-1)].push_back({h,{index,uint32_t(source.rows.size())}});
            source.rows.push_back(row); source.events.push_back(payload);
        });
    } else {
        source.clicks.reserve(size_t(source.chunk.end-source.chunk.begin)/64);
        scanner.template run<4>([&](bool parsed,const std::array<SV,4>& f) {
            ++source.physical_rows;
            if (!parsed || f[0].empty() || f[1].empty()) return;
            int64_t time; uint32_t campaign=campaigns.get(f[2],UINT32_MAX);
            if (campaign==UINT32_MAX || !cache.parse(f[3],time) || time<start-WEEK || time>=end) return;
            source.clicks.push_back({users.get(f[1],linked),f[0],int32_t(time-start),campaign});
        });
    }
}
struct Route {
    std::array<std::vector<v5::RoutedEvent>,PARTS> events;
    std::array<std::vector<v5::Click>,PARTS> clicks;
    std::array<std::vector<uint32_t>,PARTS> click_users;
};
inline void aggregate(size_t part,std::vector<Route>& routes,const std::vector<SV>& names,
                      const std::vector<v5::Purchase>& purchases,const Catalog& catalog,int64_t start,int64_t end,
                      int64_t first_day,size_t days,v5::Scratch& scratch,std::vector<Stats>& stats,const fs::path& output) {
    sprint::Flat ids; std::vector<uint32_t> keys;
    std::vector<size_t> count,click_count;
    for (auto& route:routes) for (auto& e:route.events[part]) {
        auto [v,added]=ids.add(e.user,uint32_t(keys.size()+1));
        if (added) { keys.push_back(e.user); count.push_back(0); click_count.push_back(0); }
        e.user=v-1; ++count[v-1];
    }
    if (keys.empty()) return;
    for (auto& route:routes) for (auto& user:route.click_users[part]) {
        uint32_t v=ids.find(user); user=v ? v-1 : UINT32_MAX; if (v) ++click_count[v-1];
    }
    std::vector<size_t> offsets(keys.size()+1),coffsets(keys.size()+1);
    for (size_t i=0;i<keys.size();++i) { offsets[i+1]=offsets[i]+count[i]; coffsets[i+1]=coffsets[i]+click_count[i]; count[i]=offsets[i]; click_count[i]=coffsets[i]; }
    std::vector<v5::BinEvent> events(offsets.back()); std::vector<v5::Click> clicks(coffsets.back());
    for (auto& route:routes) {
        for (auto e:route.events[part]) events[count[e.user]++]={e.time,e.purchase};
        for (size_t i=0;i<route.clicks[part].size();++i) if (route.click_users[part][i]!=UINT32_MAX)
            clicks[click_count[route.click_users[part][i]]++]=route.clicks[part][i];
        std::vector<v5::RoutedEvent>().swap(route.events[part]); std::vector<v5::Click>().swap(route.clicks[part]); std::vector<uint32_t>().swap(route.click_users[part]);
    }
    std::vector<UserRow> rows; std::vector<v5::Daily> daily;
    for (size_t user=0;user<keys.size();++user) {
        daily.clear(); size_t cb=coffsets[user],ce=coffsets[user+1];
        v5::reduce(events.data()+offsets[user],offsets[user+1]-offsets[user],purchases,clicks.empty()?nullptr:clicks.data()+cb,ce-cb,
                   start,size_t((end-start+GAP-1)/GAP),catalog.categories,catalog.campaigns.size(),first_day,days,daily,stats,scratch);
        for (auto d:daily) rows.push_back({names[keys[user]],d.has_category?catalog.categories[d.category]:SV{},d.day,d.sessions,d.events,d.revenue,d.has_category});
    }
    if (!rows.empty()) write_users(output/"user_daily"/("part-"+std::to_string(part)+".parquet"),rows);
}
inline bool run(const fs::path& data,const fs::path& output,size_t threads,int64_t start,int64_t end,int64_t first_day,size_t days) {
    size_t width=v5::vector_width(); if (!width || !__builtin_cpu_supports("sse4.2") || end-start>INT32_MAX) return false;
#if SS_PROFILE
    auto begin=std::chrono::steady_clock::now(),last=begin;
    auto timing=[&](const char* label) { auto now=std::chrono::steady_clock::now(); std::cerr<<"v6 "<<label<<" "<<std::chrono::duration<double>(now-last).count()<<"s total "<<std::chrono::duration<double>(now-begin).count()<<"s\n"; last=now; };
#else
    auto timing=[](const char*) {};
#endif
    v5::Pool pool(threads);
    Mapping pm(data/"products.csv"),cm(data/"campaigns.csv"); Catalog catalog; catalog.products_from(pm);catalog.campaigns_from(cm);
    sprint::Lookup products(catalog.products,"p"),campaigns(catalog.campaign_ids,"c_");
    auto lm=map_files(files(data/"links")),em=map_files(files(data/"events")),clm=map_files(files(data/"clicks"));
    auto link_chunks=chunks(lm); std::vector<std::vector<Link>> links(link_chunks.size());
    pool.run(link_chunks.size(),[&](size_t j,size_t) {
        auto parse=[&](auto& scanner) { scanner.template run<2>([&](bool valid,const std::array<SV,2>& f) {
            if(valid&&!f[0].empty()&&!f[1].empty()) links[j].push_back({f[0],f[1]});
        }); };
        if(width==64) { Scanner<64> scanner(link_chunks[j].begin,link_chunks[j].end); parse(scanner); }
        else { Scanner<32> scanner(link_chunks[j].begin,link_chunks[j].end); parse(scanner); }
    });
    Linked linked; linked.build(links,pool); timing("links/catalog");
    std::vector<UserCache> users; users.reserve(threads); for(uint32_t i=0;i<threads;++i) users.emplace_back(i);
    auto ec=chunks(em),cc=chunks(clm); std::vector<Source> sources(ec.size()+cc.size());
    for(size_t j=0;j<ec.size();++j)sources[j].chunk=ec[j]; for(size_t j=0;j<cc.size();++j)sources[ec.size()+j].chunk=cc[j];
    pool.run(sources.size(),[&](size_t j,size_t t) {
        if(width==64) scan<64>(sources[j],uint32_t(j),j<ec.size(),users[t],linked,products,campaigns,start,end);
        else scan<32>(sources[j],uint32_t(j),j<ec.size(),users[t],linked,products,campaigns,start,end);
    });
    timing("fused-scan/identity");
    pool.run(PARTS,[&](size_t p,size_t) { select(sources,p); }); timing("exact-selection");
    std::array<ExactNames,PARTS> singleton_shards;
    struct Singleton { uint32_t worker,row; SV name; uint64_t hash; };
    std::array<std::vector<Singleton>,PARTS> singleton_refs;
    std::vector<std::vector<uint32_t>> remap(threads);
    for(size_t t=0;t<threads;++t) { remap[t].resize(users[t].singletons.keys.size()); for(uint32_t i=0;i<users[t].singletons.keys.size();++i) { SV n=users[t].singletons.keys[i];uint64_t h=key_hash(n);singleton_refs[h&(PARTS-1)].push_back({uint32_t(t),i,n,h}); } }
    pool.run(PARTS,[&](size_t p,size_t) { auto& map=singleton_shards[p];map.reserve(singleton_refs[p].size());for(const auto& r:singleton_refs[p])remap[r.worker][r.row]=map.add(r.name,r.hash); });
    std::vector<SV> names=linked.names; std::array<uint32_t,PARTS> bases;
    for(size_t p=0;p<PARTS;++p) { bases[p]=uint32_t(names.size()); names.insert(names.end(),singleton_shards[p].keys.begin(),singleton_shards[p].keys.end()); }
    if(names.size()>=UINT32_MAX)throw std::runtime_error("too many people");
    pool.run(PARTS,[&](size_t p,size_t) { for(const auto& r:singleton_refs[p])remap[r.worker][r.row]+=bases[p]; });
    auto resolve=[&](uint64_t user) { return user>>63 ? remap[(user>>32)&0x7fffffff][uint32_t(user)] : uint32_t(user); };
    size_t pc=0;for(auto& s:sources) {s.purchase_base=uint32_t(pc);pc+=s.purchases.size();} if(pc>=UINT32_MAX)throw std::runtime_error("too many purchases");
    std::vector<v5::Purchase> purchases(pc);std::vector<Route> routes(sources.size());
    pool.run(sources.size(),[&](size_t j,size_t) {auto& s=sources[j];auto& r=routes[j];std::copy(s.purchases.begin(),s.purchases.end(),purchases.begin()+s.purchase_base);
        for(size_t i=0;i<s.events.size();++i)if(s.rows[i].time!=UINT32_MAX){auto e=s.events[i];uint32_t user=resolve(e.user);size_t p=user<linked.names.size()?linked.parts[user]:(key_hash(names[user])&(PARTS-1));r.events[p].push_back({user,e.time,e.purchase?s.purchase_base+e.purchase:0});}
        for(auto c:s.clicks){uint32_t user=resolve(c.user);size_t p=user<linked.names.size()?linked.parts[user]:(key_hash(names[user])&(PARTS-1));r.clicks[p].push_back({c.id,c.time,c.campaign});r.click_users[p].push_back(user);}
        std::vector<RawEvent>().swap(s.events);std::vector<RawClick>().swap(s.clicks);std::vector<v5::EventRow>().swap(s.rows);std::vector<v5::Purchase>().swap(s.purchases);
    });timing("singleton-merge/route");
    std::vector<std::vector<Stats>> stats(threads,std::vector<Stats>(catalog.campaigns.size()*days));std::vector<v5::Scratch> scratch(threads);
    pool.run(PARTS,[&](size_t p,size_t t){aggregate(p,routes,names,purchases,catalog,start,end,first_day,days,scratch[t],stats[t],output);});
    std::vector<Stats> total(catalog.campaigns.size()*days);for(const auto& st:stats)for(size_t i=0;i<total.size();++i)total[i]+=st[i];
    if(fs::is_empty(output/"user_daily"))write_users(output/"user_daily"/"part-0.parquet",{});write_campaigns(output/"campaign_daily"/"part-0.parquet",total,catalog,first_day,days);timing("bins/coverage/write");
    return true;
}
} // namespace v6
