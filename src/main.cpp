#include <vector>
#include "core.hpp"
#include "parquet.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace ss;
constexpr size_t PARTS = 128;

struct Mapping {
    char* data = nullptr; size_t size = 0;
    explicit Mapping(const fs::path& path) {
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) throw std::runtime_error("cannot open " + path.string());
        struct stat st{};
        if (fstat(fd, &st) != 0 || st.st_size < 0) { close(fd); throw std::runtime_error("cannot stat " + path.string()); }
        size = st.st_size;
        if (size) {
            data = static_cast<char*>(mmap(nullptr,size,PROT_READ,MAP_PRIVATE,fd,0));
            if (data == MAP_FAILED) { data = nullptr; close(fd); throw std::runtime_error("cannot map " + path.string()); }
            madvise(data,size,MADV_SEQUENTIAL);
        }
        close(fd);
    }
    Mapping(Mapping&& b) noexcept : data(b.data),size(b.size) { b.data=nullptr; b.size=0; }
    Mapping(const Mapping&) = delete;
    ~Mapping() { if (data) munmap(data,size); }
};
struct Chunk { const char* begin; const char* end; };
std::vector<fs::path> files(const fs::path& directory) {
    std::vector<fs::path> result;
    for (const auto& p : fs::directory_iterator(directory))
        if (p.path().extension() == ".csv" && p.is_regular_file()) result.push_back(p.path());
    std::sort(result.begin(),result.end()); return result;
}
std::vector<Chunk> chunks(const std::vector<Mapping>& maps) {
    std::vector<Chunk> result;
    for (const auto& map : maps) {
        if (!map.size) continue;
        const char* end = map.data+map.size;
        const char* header = static_cast<const char*>(std::memchr(map.data,'\n',map.size));
        if (!header) continue;
        const char* begin=header+1;
        while (begin<end) {
            const char* cut=begin+std::min<size_t>(end-begin,4*1024*1024);
            if (cut<end) {
                auto nl=static_cast<const char*>(std::memchr(cut,'\n',end-cut));
                cut=nl ? nl+1 : end;
            }
            result.push_back({begin,cut}); begin=cut;
        }
    }
    return result;
}
template<class F> void lines(Chunk chunk,F f) {
    const char* p=chunk.begin;
    while (p<chunk.end) {
        auto nl=static_cast<const char*>(std::memchr(p,'\n',chunk.end-p));
        const char* end=nl ? nl : chunk.end;
        f(SV(p,end-p)); p=nl ? nl+1 : chunk.end;
    }
}
template<class F> void parallel(size_t jobs,size_t threads,F fn) {
    std::atomic<size_t> next{0}; std::exception_ptr failure; std::mutex mutex;
    std::vector<std::thread> pool;
    for (size_t t=0;t<std::min(jobs,threads);++t) pool.emplace_back([&,t] {
        try { for (;;) { size_t j=next.fetch_add(1,std::memory_order_relaxed); if (j>=jobs) break; fn(j,t); } }
        catch (...) { std::lock_guard lock(mutex); if (!failure) failure=std::current_exception(); next.store(jobs); }
    });
    for (auto& thread:pool) thread.join();
    if (failure) std::rethrow_exception(failure);
}
std::vector<Mapping> map_files(const std::vector<fs::path>& paths) {
    std::vector<Mapping> maps; maps.reserve(paths.size());
    for (const auto& p:paths) maps.emplace_back(p);
    return maps;
}

struct Identity {
    std::unordered_map<SV,uint32_t,Hasher> ids;
    std::vector<uint32_t> parent;
    std::vector<uint8_t> rank;
    std::vector<SV> names;
    FlatIds lookup;
    uint32_t add(SV id) {
        auto [it,inserted]=ids.try_emplace(id,parent.size());
        if (inserted) { parent.push_back(parent.size()); rank.push_back(0); names.push_back(id); }
        return it->second;
    }
    uint32_t root(uint32_t i) {
        uint32_t r=i; while (parent[r]!=r) r=parent[r];
        while (parent[i]!=i) { uint32_t next=parent[i]; parent[i]=r; i=next; }
        return r;
    }
    void join(SV a,SV b) {
        uint32_t x=root(add(a)),y=root(add(b)); if (x==y) return;
        if (rank[x]<rank[y]) std::swap(x,y);
        parent[y]=x; names[x]=std::min(names[x],names[y]);
        if (rank[x]==rank[y]) ++rank[x];
    }
    void finish() {
        lookup.reserve(ids.size());
        for (auto& [id,index]:ids) { index=root(index); lookup.insert(id,index); }
        ids.clear(); ids.rehash(0);
    }
    SV person(SV id) const { auto i=lookup.get(id); return i==UINT32_MAX ? id : names[i]; }
};
struct Catalog {
    FlatIds products,campaign_ids;
    std::vector<SV> categories{"unknown"}, campaigns{"organic"},channels{"organic"};
    void products_from(const Mapping& map) {
        std::unordered_map<SV,uint32_t,Hasher> cats{{"unknown",0}};
        const char* nl=map.size ? static_cast<const char*>(std::memchr(map.data,'\n',map.size)):nullptr;
        if (!nl) return;
        lines({nl+1,map.data+map.size},[&](SV row) {
            std::array<SV,2> f; if (!csv(row,f)) throw std::runtime_error("malformed products row");
            auto [it,added]=cats.try_emplace(f[1],categories.size()); if (added) categories.push_back(f[1]);
            products.insert(f[0],it->second);
        });
    }
    void campaigns_from(const Mapping& map) {
        const char* nl=map.size ? static_cast<const char*>(std::memchr(map.data,'\n',map.size)):nullptr;
        if (!nl) return;
        lines({nl+1,map.data+map.size},[&](SV row) {
            std::array<SV,2> f; if (!csv(row,f)) throw std::runtime_error("malformed campaigns row");
            campaign_ids.insert(f[0],campaigns.size()); campaigns.push_back(f[0]); channels.push_back(f[1]);
        });
    }
};
struct Raw {
    const char* line; int64_t ingest,ts; SV user;
    uint32_t length,fingerprint,category,price;
};
inline SV raw_id(const Raw& row) {
    const char* end=static_cast<const char*>(std::memchr(row.line,',',row.length));
    SV id(row.line,end-row.line);
    if (id.front()=='"') { id.remove_prefix(1); id.remove_suffix(1); }
    return id;
}
struct RawPart { std::mutex mutex; std::vector<std::vector<Raw>> blocks; };
template<size_t N> void ingest(const std::vector<Mapping>& maps,size_t threads,
                              std::array<RawPart,PARTS>& parts,const Identity& identity,
                              const Catalog& cat,int64_t start,int64_t end) {
    auto work=chunks(maps);
    parallel(work.size(),threads,[&](size_t j,size_t) {
        std::array<std::vector<Raw>,PARTS> local;
        lines(work[j],[&](SV row) {
            std::array<SV,N> f; if (!csv(row,f) || f[0].empty()) return;
            if (row.size()>UINT32_MAX) throw std::runtime_error("CSV row exceeds 4 GiB");
            Raw raw{}; raw.line=row.data(); raw.length=uint32_t(row.size());
            if constexpr(N==7) {
                if (!timestamp_fast(f[3],raw.ingest)) throw std::runtime_error("invalid ingest_ts");
                bool valid=!f[1].empty() && !f[5].empty() && timestamp_fast(f[2],raw.ts)
                           && raw.ts>=start && raw.ts<end;
                uint64_t price=0;
                if (f[4]=="purchase") valid=valid && digits(f[6],10000000,price) && price;
                else valid=valid && (f[4]=="view" || f[4]=="cart");
                if (valid && raw.ingest-raw.ts<=DAY) {
                    raw.user=identity.person(f[1]); raw.price=uint32_t(price);
                    if (price) raw.category=cat.products.get(f[5],0);
                }
            } else {
                auto campaign=cat.campaign_ids.get(f[2]);
                if (!f[1].empty() && campaign!=UINT32_MAX && timestamp_fast(f[3],raw.ts)
                    && raw.ts>=start-WEEK && raw.ts<end) {
                    raw.user=identity.person(f[1]); raw.category=campaign;
                }
            }
            // Invalid copies still compete in global dedup; an earliest invalid copy poisons the ID.
            uint64_t h=hash(f[0]); raw.fingerprint=uint32_t(h);
            local[(h>>32)&(PARTS-1)].push_back(raw);
        });
        for (size_t p=0;p<PARTS;++p) if (!local[p].empty()) {
            std::lock_guard lock(parts[p].mutex); parts[p].blocks.push_back(std::move(local[p]));
        }
    });
}
template<class T> struct Partition {
    std::mutex mutex; std::vector<std::vector<T>> blocks;
    void add(std::vector<T>&& block) { if (!block.empty()) { std::lock_guard lock(mutex); blocks.push_back(std::move(block)); } }
    std::vector<T> flatten() {
        size_t total=0; for (const auto& b:blocks) total+=b.size();
        std::vector<T> rows; rows.reserve(total);
        for (auto& b:blocks) { rows.insert(rows.end(),b.begin(),b.end()); std::vector<T>().swap(b); }
        blocks.clear(); return rows;
    }
};
template<bool EVENTS> void dedup(std::array<RawPart,PARTS>& raw,size_t threads,
                                std::array<Partition<Event>,PARTS>& events,
                                std::array<Partition<Click>,PARTS>& clicks) {
    parallel(PARTS,threads,[&](size_t p,size_t) {
        size_t count=0; for (const auto& b:raw[p].blocks) count+=b.size();
        if (!count) return;
        if (count>=UINT32_MAX) throw std::runtime_error("dedup partition exceeds 32-bit row count");
        std::vector<Raw> rows; rows.reserve(count);
        for (auto& b:raw[p].blocks) { rows.insert(rows.end(),b.begin(),b.end()); std::vector<Raw>().swap(b); }
        raw[p].blocks.clear();
        size_t slots=1; while (slots < count+count/2) slots*=2;
        std::vector<uint32_t> table(slots,UINT32_MAX);
        for (uint32_t i=0;i<rows.size();++i) {
            auto& row=rows[i]; size_t at=mix(row.fingerprint)&(slots-1);
            while (table[at]!=UINT32_MAX) {
                const auto& previous=rows[table[at]];
                if (row.fingerprint==previous.fingerprint && raw_id(row)==raw_id(previous)) break;
                at=(at+1)&(slots-1);
            }
            if (table[at]==UINT32_MAX || (EVENTS && row.ingest<rows[table[at]].ingest)) table[at]=i;
        }
        std::array<std::vector<Event>,PARTS> local_events;
        std::array<std::vector<Click>,PARTS> local_clicks;
        for (uint32_t index:table) {
            if (index==UINT32_MAX) continue;
            auto& row=rows[index]; if (row.user.empty()) continue;
            size_t part=hash(row.user)&(PARTS-1);
            if constexpr(EVENTS) local_events[part].push_back({row.user,row.ts,row.category,row.price});
            else local_clicks[part].push_back({row.user,raw_id(row),row.ts,row.category});
        }
        for (size_t i=0;i<PARTS;++i) {
            events[i].add(std::move(local_events[i])); clicks[i].add(std::move(local_clicks[i]));
        }
    });
}
struct UserRow { SV user,category; int32_t day; int64_t sessions,events,revenue; bool has_category; };
struct Result { std::vector<UserRow> users; std::vector<Stats> campaigns; };

struct CompactEvent { uint32_t ts,user,category,price; };
struct CompactClick { uint32_t user,campaign; int64_t ts; SV id; };

void sort_events(std::vector<CompactEvent>& rows) {
    auto key=[](const CompactEvent& r) { return uint64_t(r.user)<<32 | r.ts; };
    if (rows.size()<4096) {
        std::sort(rows.begin(),rows.end(),[&](const auto& a,const auto& b) { return key(a)<key(b); });
        return;
    }
    std::vector<CompactEvent> temp(rows.size());
    constexpr size_t B=2048;
    std::array<size_t,B> counts;
    for (unsigned shift=0;shift<64;shift+=11) {
        counts.fill(0);
        for (const auto& r:rows) ++counts[(key(r)>>shift)&(B-1)];
        size_t total=0;
        for (auto& c:counts) { size_t n=c; c=total; total+=n; }
        for (const auto& r:rows) temp[counts[(key(r)>>shift)&(B-1)]++]=r;
        rows.swap(temp);
    }
}
void sort_events(std::vector<Event>& rows) {
    std::sort(rows.begin(),rows.end(),[](const Event& a,const Event& b) {
        return a.user==b.user ? a.ts<b.ts : a.user<b.user;
    });
}

template<class E,class C,class Name>
Result aggregate_rows(std::vector<E> events,std::vector<C> clicks,const Catalog& catalog,
                      int64_t first_day,size_t days,int64_t offset,Name name) {
    sort_events(events);
    std::sort(clicks.begin(),clicks.end(),[](const C& a,const C& b) {
        if (a.user!=b.user) return a.user<b.user;
        if (a.ts!=b.ts) return a.ts<b.ts;
        return a.id>b.id; // Smallest ID is last at a tied timestamp.
    });
    Result result; result.campaigns.resize(catalog.campaigns.size()*days);
    std::vector<int64_t> category_revenue(catalog.categories.size());
    std::vector<uint32_t> touched;
    std::vector<size_t> covered(catalog.campaigns.size());
    std::vector<Session> sessions;
    size_t ci=0;
    for (size_t begin=0;begin<events.size();) {
        size_t finish=begin+1; auto user=events[begin].user;
        while (finish<events.size() && events[finish].user==user) ++finish;
        sessions.clear(); touched.clear();
        int64_t previous=0; UserRow daily{};
        auto emit=[&] {
            if (!daily.events) return;
            int64_t best=-1; SV name;
            for (uint32_t category:touched) {
                int64_t value=category_revenue[category];
                if (value>best || (value==best && catalog.categories[category]<name)) {
                    best=value; name=catalog.categories[category];
                }
                category_revenue[category]=0;
            }
            daily.has_category=best>=0; daily.category=name;
            result.users.push_back(daily); touched.clear();
        };
        for (size_t i=begin;i<finish;++i) {
            const auto& event=events[i]; int64_t ts=offset+event.ts;
            if (i==begin || ts-previous>GAP) {
                int32_t day=day_of(ts);
                if (i==begin || daily.day!=day) {
                    emit(); daily={name(user),{},day,0,0,0,false};
                }
                ++daily.sessions; sessions.push_back({ts,0});
            }
            previous=ts; ++daily.events;
            daily.revenue+=event.price; sessions.back().revenue+=event.price;
            if (event.price) {
                if (!category_revenue[event.category]) touched.push_back(event.category);
                category_revenue[event.category]+=event.price;
            }
        }
        emit();
        while (ci<clicks.size() && clicks[ci].user<user) ++ci;
        size_t cb=ci; while (ci<clicks.size() && clicks[ci].user==user) ++ci;
        attribute(sessions,clicks.empty() ? nullptr : clicks.data()+cb,ci-cb,first_day,days,result.campaigns,covered);
        begin=finish;
    }
    return result;
}

Result aggregate(Partition<Event>& ep,Partition<Click>& cp,const Catalog& catalog,
                 int64_t first_day,size_t days,int64_t start,int64_t end) {
    auto events=ep.flatten(); auto clicks=cp.flatten();
    // shortcut: 32-bit relative event time; use the full-width path for wider windows.
    if (end-start>UINT32_MAX || events.size()+clicks.size()>=UINT32_MAX)
        return aggregate_rows(std::move(events),std::move(clicks),catalog,first_day,days,0,[](SV id) { return id; });
    FlatIds people; std::vector<SV> names;
    people.reserve(std::min<size_t>(events.size()+clicks.size(),65536));
    auto intern=[&](SV id) {
        auto [index,added]=people.insert(id,names.size());
        if (added) names.push_back(id);
        return index;
    };
    std::vector<CompactEvent> compact_events; compact_events.reserve(events.size());
    for (const auto& r:events) compact_events.push_back({uint32_t(r.ts-start),intern(r.user),r.category,r.price});
    std::vector<Event>().swap(events);
    std::vector<CompactClick> compact_clicks; compact_clicks.reserve(clicks.size());
    for (const auto& r:clicks) compact_clicks.push_back({intern(r.user),r.campaign,r.ts,r.id});
    std::vector<Click>().swap(clicks);
    return aggregate_rows(std::move(compact_events),std::move(compact_clicks),catalog,first_day,days,start,
                          [&](uint32_t id) { return names[id]; });
}

std::string setting(const char* name,const char* fallback) { auto v=std::getenv(name); return v ? v : fallback; }
int64_t manifest_value(const fs::path& path,SV key) {
    std::ifstream stream(path); if (!stream) throw std::runtime_error("cannot read manifest");
    std::string text((std::istreambuf_iterator<char>(stream)),{});
    size_t p=text.find(std::string("\"")+std::string(key)+"\"");
    if (p==std::string::npos || (p=text.find(':',p))==std::string::npos || (p=text.find('"',p))==std::string::npos)
        throw std::runtime_error("missing manifest key");
    size_t end=text.find('"',p+1); int64_t time;
    if (end==std::string::npos || !timestamp(SV(text).substr(p+1,end-p-1),time)) throw std::runtime_error("invalid manifest timestamp");
    return time;
}

// Output uses one independent part per hash partition, keeping writer memory bounded.
void write_users(const fs::path& path,const std::vector<UserRow>& rows) {
    using C=pq::Column;
    std::vector<C> cols{{"user_id",C::STRING},{"day",C::DATE},{"sessions",C::INT64},
                        {"events",C::INT64},{"revenue_cents",C::INT64},{"top_category",C::STRING}};
    for (const auto& r:rows) {
        cols[0].strings.emplace_back(r.user); cols[1].integers.push_back(r.day);
        cols[2].integers.push_back(r.sessions); cols[3].integers.push_back(r.events); cols[4].integers.push_back(r.revenue);
        cols[5].strings.emplace_back(r.category); cols[5].valid.push_back(r.has_category);
    }
    pq::write(path.string(),cols);
}
void write_campaigns(const fs::path& path,const std::vector<Stats>& stats,const Catalog& cat,int64_t first_day,size_t days) {
    using C=pq::Column;
    std::vector<C> cols{{"campaign_id",C::STRING},{"channel",C::STRING},{"day",C::DATE},
                        {"sessions",C::INT64},{"purchasing_sessions",C::INT64},{"revenue_cents",C::INT64},{"assisted_sessions",C::INT64}};
    for (size_t c=0;c<cat.campaigns.size();++c) for (size_t d=0;d<days;++d) {
        const auto& r=stats[c*days+d]; if (!r.sessions && !r.assisted) continue;
        cols[0].strings.emplace_back(cat.campaigns[c]); cols[1].strings.emplace_back(cat.channels[c]);
        cols[2].integers.push_back(first_day+d); cols[3].integers.push_back(r.sessions);
        cols[4].integers.push_back(r.purchasing); cols[5].integers.push_back(r.revenue); cols[6].integers.push_back(r.assisted);
    }
    pq::write(path.string(),cols);
}
int main(int argc,char** argv) {
    try {
        if (argc!=1 && argc!=3) throw std::runtime_error("usage: shopping-sessions [DATA_DIR OUTPUT_DIR]");
        fs::path data=argc==3 ? argv[1] : setting("DATA_DIR","/data");
        fs::path output=argc==3 ? argv[2] : setting("OUTPUT_DIR","/output");
        uint64_t ncpu=0; if (!digits(setting("NCPU","8"),128,ncpu) || !ncpu) throw std::runtime_error("NCPU must be 1..128");
        size_t threads=ncpu;
        auto clock=std::chrono::steady_clock::now(),last=clock;
        auto timing=[&](const char* stage) { auto now=std::chrono::steady_clock::now();
            std::cerr<<stage<<" "<<std::chrono::duration<double>(now-last).count()<<"s\n"; last=now; };
        int64_t start=manifest_value(data/"manifest.json","window_start"),end=manifest_value(data/"manifest.json","window_end");
        if (end<=start) throw std::runtime_error("empty or reversed manifest window");
        int64_t first_day=day_of(start); size_t days=day_of(end-1)-first_day+1;
        if (days>36600) throw std::runtime_error("manifest window exceeds 100 years");
        for (const char* table:{"user_daily","campaign_daily"}) {
            auto path=output/table;
            if (fs::exists(path) && (!fs::is_directory(path) || !fs::is_empty(path)))
                throw std::runtime_error("refusing to overwrite nonempty output " + path.string());
            fs::create_directories(path);
        }
        auto link_maps=map_files(files(data/"links"));
        Identity identity;
        for (auto chunk:chunks(link_maps)) lines(chunk,[&](SV row) {
            std::array<SV,2> f; if (csv(row,f) && !f[0].empty() && !f[1].empty()) identity.join(f[0],f[1]);
        });
        identity.finish();
        Mapping product_map(data/"products.csv"),campaign_map(data/"campaigns.csv");
        Catalog catalog; catalog.products_from(product_map); catalog.campaigns_from(campaign_map);
        timing("identity/catalog");
        auto event_maps=map_files(files(data/"events")); auto click_maps=map_files(files(data/"clicks"));
        std::array<Partition<Event>,PARTS> events;
        std::array<Partition<Click>,PARTS> clicks;
        {
            std::array<RawPart,PARTS> raw;
            ingest<7>(event_maps,threads,raw,identity,catalog,start,end); timing("event parse");
            dedup<true>(raw,threads,events,clicks); timing("event dedup/validate");
        }
        {
            std::array<RawPart,PARTS> raw;
            ingest<4>(click_maps,threads,raw,identity,catalog,start,end); timing("click parse");
            dedup<false>(raw,threads,events,clicks); timing("click dedup/validate");
        }
        std::vector<Stats> campaign_stats(catalog.campaigns.size()*days);
        std::mutex campaign_mutex;
        parallel(PARTS,threads,[&](size_t part,size_t) {
            auto result=aggregate(events[part],clicks[part],catalog,first_day,days,start,end);
            if (!result.users.empty()) write_users(output/"user_daily"/("part-"+std::to_string(part)+".parquet"),result.users);
            std::lock_guard lock(campaign_mutex);
            for (size_t i=0;i<campaign_stats.size();++i) campaign_stats[i]+=result.campaigns[i];
        });
        if (fs::is_empty(output/"user_daily")) write_users(output/"user_daily"/"part-0.parquet",{});
        write_campaigns(output/"campaign_daily"/"part-0.parquet",campaign_stats,catalog,first_day,days);
        timing("sort/session/attribution/write");
        std::cerr<<"total "<<std::chrono::duration<double>(std::chrono::steady_clock::now()-clock).count()<<"s\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<"shopping-sessions: "<<error.what()<<'\n'; return 1; }
}
