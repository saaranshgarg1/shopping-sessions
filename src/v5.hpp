#pragma once
#include "reduce.hpp"
#include "scanner.hpp"
#include "packed_ids.hpp"
#include <bit>
#include <condition_variable>
#include <functional>
#include <sched.h>

namespace v5 {
// Keep one pool alive across scans, shard ownership and person reduction.
class Pool {
    std::vector<std::thread> workers;
    std::mutex mutex;
    std::condition_variable ready, done;
    std::function<void(size_t,size_t)> task;
    std::atomic<size_t> next{0};
    size_t count=0, generation=0, remaining=0;
    bool stopping=false;
    std::exception_ptr failure;
public:
    explicit Pool(size_t threads) {
        for (size_t t=0;t<threads;++t) workers.emplace_back([&,t] {
            size_t seen=0;
            for (;;) {
                std::unique_lock lock(mutex);
                ready.wait(lock,[&] { return stopping || generation!=seen; });
                if (stopping) return;
                seen=generation; lock.unlock();
                try {
                    for (;;) {
                        size_t j=next.fetch_add(1,std::memory_order_relaxed);
                        if (j>=count) break;
                        task(j,t);
                    }
                } catch (...) {
                    std::lock_guard guard(mutex);
                    if (!failure) failure=std::current_exception();
                    next.store(count,std::memory_order_relaxed);
                }
                lock.lock();
                if (!--remaining) done.notify_one();
            }
        });
    }
    Pool(const Pool&)=delete;
    ~Pool() {
        { std::lock_guard lock(mutex); stopping=true; }
        ready.notify_all();
        for (auto& worker:workers) worker.join();
    }
    template<class F> void run(size_t jobs,F fn) {
        if (!jobs) return;
        std::unique_lock lock(mutex);
        task=std::move(fn); count=jobs; next=0; failure=nullptr;
        remaining=workers.size(); ++generation;
        ready.notify_all(); done.wait(lock,[&] { return !remaining; });
        task={}; if (failure) std::rethrow_exception(failure);
    }
};

__attribute__((target("avx2"))) inline uint64_t mask256(const char* p) {
    __m256i b=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
    auto m=_mm256_or_si256(_mm256_cmpeq_epi8(b,_mm256_set1_epi8(',')),
        _mm256_or_si256(_mm256_cmpeq_epi8(b,_mm256_set1_epi8('\n')),_mm256_cmpeq_epi8(b,_mm256_set1_epi8('"'))));
    return uint32_t(_mm256_movemask_epi8(m));
}
__attribute__((target("avx512f,avx512bw"))) inline uint64_t mask512(const char* p) {
    __m512i b=_mm512_loadu_si512(p);
    return _mm512_cmpeq_epi8_mask(b,_mm512_set1_epi8(',')) |
           _mm512_cmpeq_epi8_mask(b,_mm512_set1_epi8('\n')) |
           _mm512_cmpeq_epi8_mask(b,_mm512_set1_epi8('"'));
}
class Scanner {
    const char* cursor;
    const char* end;
    const char* block=nullptr;
    uint64_t mask=0;
    size_t width;
    uint64_t (*kernel)(const char*);
    const char* delimiter() {
        while (!mask) {
            if (cursor==end) return end;
            block=cursor;
            if (size_t(end-cursor)>=width) { mask=kernel(cursor); cursor+=width; }
            else {
                for (size_t i=0,n=end-cursor;i<n;++i)
                    if (cursor[i]==',' || cursor[i]=='\n' || cursor[i]=='"') mask|=uint64_t(1)<<i;
                cursor=end;
            }
        }
        unsigned bit=std::countr_zero(mask); mask&=mask-1;
        return block+bit;
    }
    const char* row;
public:
    Scanner(Chunk chunk,size_t vector_width):cursor(chunk.begin),end(chunk.end),width(vector_width),
        kernel(vector_width==64 ? mask512 : mask256),row(chunk.begin) {}
    bool more() const { return row<end; }
    template<size_t N> bool next(std::array<SV,N>& fields) {
        const char* start=row;
        size_t count=0,quotes=0;
        const char* first=nullptr; const char* last=nullptr;
        bool valid=true;
        for (;;) {
            const char* at=delimiter();
            if (at!=end && *at=='"') { if (!quotes) first=at; last=at; ++quotes; continue; }
            SV f(start,at-start);
            if (quotes) {
                if (quotes!=2 || first!=start || last!=at-1) valid=false;
                else { f.remove_prefix(1); f.remove_suffix(1); }
            }
            if (count<N) fields[count]=f; else valid=false;
            ++count;
            if (at==end || *at=='\n') { row=at==end ? end : at+1; return valid && count==N; }
            start=at+1; quotes=0; first=last=nullptr;
        }
    }
};
inline size_t vector_width() {
    auto mode=setting("SS_SIMD","auto");
    if (mode!="auto" && mode!="avx2" && mode!="avx512") throw std::runtime_error("SS_SIMD must be auto, avx2 or avx512");
    bool wide=__builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("avx512f");
    if (mode=="avx512" && !wide) throw std::runtime_error("AVX-512BW unavailable on this CPU");
    if (!__builtin_cpu_supports("avx2")) return 0;
    return mode!="avx2" && wide ? 64 : 32;
}

struct Ref { uint32_t source,row; };
struct Descriptor {
    static constexpr uint32_t SOURCE_MASK=UINT32_MAX>>1;
    uint64_t hash;
    Ref ref;
    uint32_t source() const { return ref.source&SOURCE_MASK; }
    bool needs_byte_comparison() const { return ref.source>SOURCE_MASK; }
};
struct AliasDescriptor { uint64_t key; uint32_t row, event_time; };
static_assert(sizeof(Descriptor)==16 && sizeof(Ref)==8);
struct EventRow {
    uint32_t id_offset,id_length,ingest_offset,user_offset,user_length,time,purchase;
};
struct ClickRow { uint32_t id_offset,id_length,user_offset,user_length; int32_t time; uint32_t campaign; };
struct LinkRow { uint32_t a_offset,a_length,b_offset,b_length; };
enum Kind { EVENTS, CLICKS, LINKS };
struct Source {
    Chunk chunk;
    Kind kind;
    std::vector<EventRow> events;
    std::vector<ClickRow> clicks;
    std::vector<LinkRow> links;
    std::vector<Purchase> purchases;
    std::array<std::vector<Descriptor>,PARTS> descriptors;
    std::array<std::vector<AliasDescriptor>,PARTS> aliases;
    uint32_t purchase_base=0;
    uint64_t rows=0;
};
inline SV view(const Source& source,uint32_t offset,uint32_t length) { return {source.chunk.begin+offset,length}; }
inline uint32_t offset(const Source& source,SV field) {
    uint64_t n=field.data()-source.chunk.begin;
    if (n>UINT32_MAX || field.size()>UINT32_MAX) throw std::runtime_error("CSV chunk exceeds 32-bit field offsets");
    return uint32_t(n);
}
inline SV event_id(const Source& source,const EventRow& row) { return view(source,row.id_offset,row.id_length); }
struct IngestCache {
    struct Date { uint64_t head=0; uint16_t tail=0; int64_t midnight=0; };
    std::array<Date,64> dates{};
    bool late(SV ingest,int64_t time,sprint::DateCache& cache) {
        if (ingest.size()!=20) throw std::runtime_error("invalid ingest_ts");
        uint64_t head; uint16_t tail;
        std::memcpy(&head,ingest.data(),8); std::memcpy(&tail,ingest.data()+8,2);
        auto& date=dates[sprint::day_slot(tail)];
        int64_t parsed=0;
        if (date.head!=head || date.tail!=tail) {
            if (!cache.parse(ingest,parsed)) throw std::runtime_error("invalid ingest_ts");
            date={head,tail,day_of(parsed)*DAY};
        }
        if (time>=date.midnight) return false;
        if (time<date.midnight-DAY) return true;
        auto two=[&](size_t i) { return (ingest[i]-'0')*10+ingest[i+1]-'0'; };
        int64_t tod=int64_t(two(11)*3600+two(14)*60+two(17))*1000;
        return date.midnight-DAY+tod>time;
    }
};
template<int WIDTH> inline void scan_width(Source& source,uint32_t index,const sprint::Lookup& products,const sprint::Lookup& campaigns,
                 int64_t start,int64_t end) {
    v6::Scanner<WIDTH> scanner(source.chunk.begin,source.chunk.end);
    sprint::DateCache cache;
    IngestCache ingest_cache;
    if (source.kind==EVENTS) {
        size_t estimate=size_t(source.chunk.end-source.chunk.begin)/80;
        source.events.reserve(estimate);
        source.purchases.reserve(estimate/24+8);
        for (auto& bucket:source.descriptors) bucket.reserve(estimate/PARTS+8);
        scanner.template run<7>([&](bool parsed,const std::array<SV,7>& f) {
            ++source.rows;
            if (!parsed || f[0].empty()) return;
            if (source.events.size()>=UINT32_MAX) throw std::runtime_error("too many rows in CSV chunk");
            EventRow row{offset(source,f[0]),uint32_t(f[0].size()),offset(source,f[3]),
                offset(source,f[1]),uint32_t(f[1].size()),UINT32_MAX,0};
            int64_t time=0; uint64_t price=0;
            bool valid=!f[1].empty() && !f[5].empty() && cache.parse(f[2],time) && time>=start && time<end;
            if (valid) {
                if (f[4]=="purchase") valid=digits(f[6],10000000,price) && price;
                else valid=f[4]=="view" || f[4]=="cart";
            }
            if (valid && !ingest_cache.late(f[3],time,cache)) {
                row.time=uint32_t(time-start);
                if (price) {
                    if (source.purchases.size()>=UINT32_MAX-1) throw std::runtime_error("too many purchase payloads in chunk");
                    source.purchases.push_back({products.get(f[5],0),uint32_t(price)});
                    row.purchase=uint32_t(source.purchases.size());
                }
            }
            uint64_t key=event_key(f[0]);
            uint64_t mixed=mix(key);
            uint32_t tagged_source=index | (uint32_t(key>>63)<<31);
            source.descriptors[(mixed>>57)&(PARTS-1)].push_back({mixed,{tagged_source,uint32_t(source.events.size())}});
            source.events.push_back(row);
        });
    } else if (source.kind==CLICKS) {
        source.clicks.reserve((source.chunk.end-source.chunk.begin)/64);
        scanner.template run<4>([&](bool parsed,const std::array<SV,4>& f) {
            ++source.rows;
            if (!parsed || f[0].empty() || f[1].empty()) return;
            int64_t time; uint32_t campaign=campaigns.get(f[2],UINT32_MAX);
            if (campaign==UINT32_MAX || !cache.parse(f[3],time) || time<start-WEEK || time>=end) return;
            if (source.clicks.size()>=UINT32_MAX) throw std::runtime_error("too many clicks in chunk");
            source.clicks.push_back({offset(source,f[0]),uint32_t(f[0].size()),offset(source,f[1]),uint32_t(f[1].size()),
                int32_t(time-start),campaign});
        });
    } else {
        scanner.template run<2>([&](bool parsed,const std::array<SV,2>& f) {
            ++source.rows;
            if (!parsed || f[0].empty() || f[1].empty()) return;
            source.links.push_back({offset(source,f[0]),uint32_t(f[0].size()),offset(source,f[1]),uint32_t(f[1].size())});
        });
    }
}
inline void scan(Source& source,uint32_t index,const sprint::Lookup& products,const sprint::Lookup& campaigns,
                 int64_t start,int64_t end,size_t width) {
    if (width==64) scan_width<64>(source,index,products,campaigns,start,end);
    else scan_width<32>(source,index,products,campaigns,start,end);
}
struct SelectionStats { uint64_t descriptors=0,comparisons=0,duplicates=0; size_t largest=0; };
inline SelectionStats select(std::vector<Source>& sources,size_t shard) {
    size_t count=0;
    for (const auto& source:sources) count+=source.descriptors[shard].size();
    SelectionStats stats{count,0,0,count};
    if (!count) return stats;
    size_t capacity=16;
    while (capacity<count+count/2) capacity*=2;
    const size_t mask=capacity-1;
    struct Slot { uint64_t key=0; Ref winner{}; };
    std::vector<Slot> table(capacity);
    for (auto& block_source:sources) {
        auto& block=block_source.descriptors[shard];
        for (size_t i=0;i<block.size();++i) {
            constexpr size_t lookahead=12;
            if (i+lookahead<block.size())
                __builtin_prefetch(&table[block[i+lookahead].hash&mask],1,3);
            const Descriptor& candidate=block[i];
            size_t at=candidate.hash&mask;
            while (table[at].winner.row) {
                if (candidate.hash==table[at].key) {
                    const Ref winner=table[at].winner;
                    auto& source=sources[candidate.source()];
                    auto& row=source.events[candidate.ref.row];
                    auto& old_source=sources[winner.source];
                    auto& old=old_source.events[winner.row-1];
                    ++stats.comparisons;
                    if (!candidate.needs_byte_comparison() || event_id(source,row)==event_id(old_source,old)) {
                        ++stats.duplicates;
                        if (std::memcmp(source.chunk.begin+row.ingest_offset,old_source.chunk.begin+old.ingest_offset,20)<0) {
                            old.time=UINT32_MAX;
                            table[at].winner={candidate.source(),candidate.ref.row+1};
                        } else row.time=UINT32_MAX;
                        break;
                    }
                }
                at=(at+1)&mask;
            }
            if (!table[at].winner.row)
                table[at]={candidate.hash,{candidate.source(),candidate.ref.row+1}};
        }
        std::vector<Descriptor>().swap(block);
    }
    return stats;
}
inline SV alias(const Source& s,uint32_t row) {
    if (s.kind==EVENTS) { const auto& r=s.events[row]; return view(s,r.user_offset,r.user_length); }
    if (s.kind==CLICKS) { const auto& r=s.clicks[row]; return view(s,r.user_offset,r.user_length); }
    const auto& r=s.links[row/2];
    return row%2 ? view(s,r.b_offset,r.b_length) : view(s,r.a_offset,r.a_length);
}
inline void handle(Source& s,uint32_t row,uint32_t local,uint32_t shard) {
    if (s.kind==EVENTS) { auto& r=s.events[row]; r.user_offset=local; r.user_length=shard; }
    else if (s.kind==CLICKS) { auto& r=s.clicks[row]; r.user_offset=local; r.user_length=shard; }
    else {
        auto& r=s.links[row/2];
        if (row%2) { r.b_offset=local; r.b_length=shard; }
        else { r.a_offset=local; r.a_length=shard; }
    }
}
struct RoutedEvent { uint32_t user,time,purchase; };
struct RoutedClick { uint32_t user; Ref ref; };
struct Groups {
    std::vector<size_t> event_offsets,click_offsets,event_positions,click_positions;
    std::vector<BinEvent> events;
    std::vector<Click> clicks;
};
struct IdShard {
    PackedIds packed; std::vector<SV> names; uint32_t base=0;
    std::vector<RoutedEvent> events;
    std::vector<RoutedClick> clicks;
    std::vector<uint32_t> event_counts,click_counts;
    std::vector<BinEvent*> event_positions;
    std::vector<Click*> click_positions;
};
struct People {
    std::array<IdShard,PARTS> shards;
    std::vector<uint32_t> parent;
    std::vector<uint8_t> rank;
    std::vector<SV> names;
    std::vector<uint32_t> dense;
    std::vector<size_t> event_counts,click_counts;
    std::array<std::vector<uint32_t>,PARTS> owners;
    uint32_t root(uint32_t i) {
        uint32_t r=i; while (parent[r]!=r) r=parent[r];
        while (parent[i]!=i) { uint32_t next=parent[i]; parent[i]=r; i=next; }
        return r;
    }
    uint32_t index(uint32_t local,uint32_t shard) const { return shards[shard].base+local; }
    void build(std::vector<Source>& sources,Pool& pool) {
        pool.run(sources.size(),[&](size_t j,size_t) {
            auto& s=sources[j];
            for (auto& bucket:s.aliases) bucket.reserve((s.events.size()+s.clicks.size()+s.links.size()*2)/PARTS+8);
            auto add=[&](uint32_t row,uint32_t event_time=0) {
                uint64_t key=alias_key(alias(s,row));
                s.aliases[(mix(key)>>57)&(PARTS-1)].push_back({key,row,event_time});
            };
            if (s.kind==EVENTS) { for (uint32_t i=0;i<s.events.size();++i) if (s.events[i].time!=UINT32_MAX) {
                const auto& event=s.events[i];
                add(i,event.time | (event.purchase ? 0x80000000u : 0u));
            } }
            else if (s.kind==CLICKS) { for (uint32_t i=0;i<s.clicks.size();++i) add(i); }
            else {
                if (s.links.size()>=UINT32_MAX/2) throw std::runtime_error("too many links in chunk");
                for (uint32_t i=0;i<s.links.size()*2;++i) add(i);
            }
        });
        pool.run(PARTS,[&](size_t p,size_t) {
            auto& shard=shards[p];
            size_t count=0,event_count=0,click_count=0;
            for (const auto& source:sources) {
                size_t n=source.aliases[p].size(); count+=n;
                if (source.kind==EVENTS) event_count+=n;
                else if (source.kind==CLICKS) click_count+=n;
            }
            shard.events.reserve(event_count); shard.clicks.reserve(click_count);
            shard.packed.reserve(std::min<size_t>(count/24+64,65536));
            for (auto& s:sources) {
                auto& refs=s.aliases[p];
                const uint32_t source_index=uint32_t(&s-sources.data());
                for (const auto& descriptor:refs) {
                    if (shard.names.size()>=UINT32_MAX-1) throw std::runtime_error("alias shard exceeds 32-bit handles");
                    auto [v,added]=shard.packed.add(descriptor.key,uint32_t(shard.names.size()),
                        [&](uint32_t previous) { return shard.names[previous]==alias(s,descriptor.row); });
                    if (added) { shard.names.push_back(alias(s,descriptor.row)); shard.event_counts.push_back(0); shard.click_counts.push_back(0); }
                    if (s.kind==EVENTS) {
                        ++shard.event_counts[v];
                        const uint32_t purchase=(descriptor.event_time>>31) ? s.purchase_base+s.events[descriptor.row].purchase : 0;
                        shard.events.push_back({v,descriptor.event_time&0x7fffffffu,purchase});
                    } else if (s.kind==CLICKS) {
                        ++shard.click_counts[v];
                        shard.clicks.push_back({v,{source_index,descriptor.row}});
                    } else handle(s,descriptor.row,v,uint32_t(p));
                }
                std::vector<AliasDescriptor>().swap(refs);
            }
        });
        size_t total=0;
        for (auto& shard:shards) {
            shard.base=uint32_t(total); total+=shard.names.size();
            if (total>=UINT32_MAX) throw std::runtime_error("too many distinct aliases");
        }
        parent.resize(total); rank.resize(total); names.resize(total); event_counts.resize(total); click_counts.resize(total);
        pool.run(PARTS,[&](size_t p,size_t) {
            const auto& s=shards[p];
            for (size_t i=0;i<s.names.size();++i) {
                parent[s.base+i]=uint32_t(s.base+i); names[s.base+i]=s.names[i];
                event_counts[s.base+i]=s.event_counts[i]; click_counts[s.base+i]=s.click_counts[i];
            }
        });
        for (const auto& source:sources) for (const auto& link:source.links) {
            uint32_t x=root(index(link.a_offset,link.a_length)),y=root(index(link.b_offset,link.b_length));
            if (x==y) continue;
            if (rank[x]<rank[y]) std::swap(x,y);
            parent[y]=x; names[x]=std::min(names[x],names[y]);
            event_counts[x]+=event_counts[y]; event_counts[y]=0;
            click_counts[x]+=click_counts[y]; click_counts[y]=0;
            if (rank[x]==rank[y]) ++rank[x];
        }
        for (uint32_t i=0;i<total;++i) parent[i]=root(i);
        dense.resize(total);
        for (uint32_t i=0;i<total;++i) if (parent[i]==i) {
            size_t part=mix(i)&(PARTS-1); dense[i]=uint32_t(owners[part].size()); owners[part].push_back(i);
        }
        std::vector<uint8_t>().swap(rank);
        for (auto& shard:shards) { shard.packed=PackedIds{}; std::vector<SV>().swap(shard.names); }
    }
    uint32_t person(uint32_t local,uint32_t shard) const { return parent[index(local,shard)]; }
};
inline void aggregate(size_t part,Groups& group,const People& people,
                      const std::vector<Purchase>& purchases,const Catalog& catalog,int64_t start,int64_t end,
                      int64_t first_day,size_t days,Scratch& scratch,std::vector<Stats>& stats,const fs::path& output) {
    const auto& keys=people.owners[part];
    const auto& offsets=group.event_offsets; const auto& coffsets=group.click_offsets;
    const auto& events=group.events; const auto& clicks=group.clicks;
    std::vector<UserRow> output_rows;
    std::vector<Daily> daily;
    for (size_t user=0;user<keys.size();++user) {
        if (offsets[user]==offsets[user+1]) continue;
        daily.clear();
        size_t cb=coffsets[user],ce=coffsets[user+1];
        reduce(events.data()+offsets[user],offsets[user+1]-offsets[user],purchases,
               clicks.empty() ? nullptr : clicks.data()+cb,ce-cb,start,size_t((end-start+GAP-1)/GAP),
               catalog.categories,catalog.campaigns.size(),first_day,days,daily,stats,scratch);
        for (const auto& d:daily) output_rows.push_back({people.names[keys[user]],d.has_category ? catalog.categories[d.category] : SV{},
            d.day,d.sessions,d.events,d.revenue,d.has_category});
    }
    if (!output_rows.empty()) write_users(output/"user_daily"/("part-"+std::to_string(part)+".parquet"),output_rows);
}
inline bool run(const fs::path& data,const fs::path& output,size_t threads,int64_t start,int64_t end,int64_t first_day,size_t days) {
    // shortcut: signed relative milliseconds cover 14 days plus lookback; wider windows use oracle.
    size_t width=vector_width();
    if (!width || end-start>INT32_MAX) return false;
#if SS_PROFILE
    auto clock=std::chrono::steady_clock::now(),last=clock;
    auto timing=[&](const char* name) {
        auto now=std::chrono::steady_clock::now();
        std::cerr<<"v5 "<<name<<" "<<std::chrono::duration<double>(now-last).count()<<"s cumulative "
                 <<std::chrono::duration<double>(now-clock).count()<<"s\n"; last=now;
    };
#else
    auto timing=[](const char*) {};
#endif
    Pool pool(threads);
    Mapping product_map(data/"products.csv"),campaign_map(data/"campaigns.csv");
    Catalog catalog; catalog.products_from(product_map); catalog.campaigns_from(campaign_map);
    sprint::Lookup products(catalog.products,"p"),campaigns(catalog.campaign_ids,"c_");
    auto event_maps=map_files(files(data/"events")),click_maps=map_files(files(data/"clicks")),link_maps=map_files(files(data/"links"));
    std::vector<Source> sources;
    auto add=[&](const std::vector<Mapping>& maps,Kind kind) {
        for (auto chunk:chunks(maps)) { Source source{}; source.chunk=chunk; source.kind=kind; sources.push_back(std::move(source)); }
    };
    add(event_maps,EVENTS); add(click_maps,CLICKS); add(link_maps,LINKS);
    if (sources.size()>Descriptor::SOURCE_MASK) throw std::runtime_error("too many CSV chunks");
    timing("map/catalog");
    pool.run(sources.size(),[&](size_t j,size_t) { scan(sources[j],uint32_t(j),products,campaigns,start,end,width); });
    timing("scan/validate");
    std::array<SelectionStats,PARTS> selection;
    pool.run(PARTS,[&](size_t p,size_t) { selection[p]=select(sources,p); });
    timing("exact-event-selection");
    size_t purchase_count=0;
    for (auto& s:sources) { s.purchase_base=uint32_t(purchase_count); purchase_count+=s.purchases.size(); }
    if (purchase_count>=UINT32_MAX) throw std::runtime_error("too many purchase payloads");
    std::vector<Purchase> purchases(purchase_count);
    People people; people.build(sources,pool);
    timing("alias/union");
    pool.run(sources.size(),[&](size_t j,size_t) {
        auto& s=sources[j];
        std::copy(s.purchases.begin(),s.purchases.end(),purchases.begin()+s.purchase_base);
        std::vector<Purchase>().swap(s.purchases);
        std::vector<EventRow>().swap(s.events); std::vector<LinkRow>().swap(s.links);
    });
    std::array<Groups,PARTS> groups;
    pool.run(PARTS,[&](size_t part,size_t) {
        auto& group=groups[part]; const auto& keys=people.owners[part];
        group.event_offsets.resize(keys.size()+1); group.click_offsets.resize(keys.size()+1);
        for (size_t i=0;i<keys.size();++i) {
            uint32_t root=keys[i];
            group.event_offsets[i+1]=group.event_offsets[i]+people.event_counts[root];
            group.click_offsets[i+1]=group.click_offsets[i]+(people.event_counts[root] ? people.click_counts[root] : 0);
        }
        group.events.resize(group.event_offsets.back()); group.clicks.resize(group.click_offsets.back());
        group.event_positions=group.event_offsets; group.click_positions=group.click_offsets;
    });
    // Reserve disjoint alias ranges once per distinct ID, not once per event.
    for (auto& shard:people.shards) {
        shard.event_positions.resize(shard.event_counts.size()); shard.click_positions.resize(shard.click_counts.size());
        for (uint32_t alias=0;alias<shard.event_counts.size();++alias) {
            uint32_t root=people.parent[shard.base+alias]; auto& group=groups[mix(root)&(PARTS-1)];
            uint32_t dense=people.dense[root];
            if (shard.event_counts[alias]) {
                shard.event_positions[alias]=group.events.data()+group.event_positions[dense];
                group.event_positions[dense]+=shard.event_counts[alias];
            }
            if (people.event_counts[root] && shard.click_counts[alias]) {
                shard.click_positions[alias]=group.clicks.data()+group.click_positions[dense];
                group.click_positions[dense]+=shard.click_counts[alias];
            }
        }
    }
    pool.run(PARTS,[&](size_t p,size_t) {
        auto& shard=people.shards[p];
        for (const auto& event:shard.events) {
            *shard.event_positions[event.user]++={event.time,event.purchase};
        }
        for (const auto& click:shard.clicks) {
            auto& target=shard.click_positions[click.user];
            if (!target) continue;
            const auto& source=sources[click.ref.source]; const auto& row=source.clicks[click.ref.row];
            *target++={view(source,row.id_offset,row.id_length),row.time,row.campaign};
        }
        std::vector<RoutedEvent>().swap(shard.events); std::vector<RoutedClick>().swap(shard.clicks);
        std::vector<uint32_t>().swap(shard.event_counts); std::vector<uint32_t>().swap(shard.click_counts);
        std::vector<BinEvent*>().swap(shard.event_positions); std::vector<Click*>().swap(shard.click_positions);
    });
    timing("person-route");
    std::vector<std::vector<Stats>> counters(threads,std::vector<Stats>(catalog.campaigns.size()*days));
    std::vector<Scratch> scratch(threads);
    pool.run(PARTS,[&](size_t p,size_t worker) {
        aggregate(p,groups[p],people,purchases,catalog,start,end,first_day,days,scratch[worker],counters[worker],output);
    });
    std::vector<Stats> stats(catalog.campaigns.size()*days);
    for (const auto& worker:counters) for (size_t i=0;i<stats.size();++i) stats[i]+=worker[i];
    if (fs::is_empty(output/"user_daily")) write_users(output/"user_daily"/"part-0.parquet",{});
    write_campaigns(output/"campaign_daily"/"part-0.parquet",stats,catalog,first_day,days);
    timing("bins/coverage/parquet-close");
#if SS_PROFILE
    SelectionStats sum;
    for (auto s:selection) { sum.descriptors+=s.descriptors; sum.comparisons+=s.comparisons; sum.duplicates+=s.duplicates; sum.largest=std::max(sum.largest,s.largest); }
    uint64_t bytes=0,rows=0;
    for (const auto* maps:{&event_maps,&click_maps,&link_maps}) for (const auto& m:*maps) bytes+=m.size;
    for (const auto& s:sources) rows+=s.rows;
    std::cerr<<"v5 counts bytes="<<bytes<<" physical_rows="<<rows<<" descriptors="<<sum.descriptors
             <<" descriptor_bytes="<<sum.descriptors*sizeof(Descriptor)<<" exact_comparisons="<<sum.comparisons
             <<" duplicates="<<sum.duplicates<<" largest_shard="<<sum.largest<<" aliases="<<people.names.size()
             <<" purchases="<<purchase_count<<" simd="<<width*8<<"\n";
#endif
    return true;
}
} // namespace v5
