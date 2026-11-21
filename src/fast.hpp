#pragma once
#include "decode.hpp"

namespace sprint {
struct Unsupported {};

// shortcut: nonzero ASCII suffixes up to eight bytes; other IDs use tagged exact string keys.
inline uint64_t key(SV s, char prefix) {
    if (s.empty() || s.front()!=prefix || s.size()>9) throw Unsupported{};
    uint64_t value=0;
    for (size_t i=1;i<s.size();++i) {
        unsigned char c=s[i];
        if (!c || c>=128) throw Unsupported{};
        value |= uint64_t(c) << (64-8*i);
    }
    return value;
}
struct Keys {
    char prefix;
    std::mutex mutex;
    std::unordered_map<SV,uint64_t,Hasher> generic;
    std::vector<SV> values;
    explicit Keys(char p):prefix(p) {}
    uint64_t get(SV s) {
        try { return key(s,prefix); } catch (const Unsupported&) {}
        std::lock_guard lock(mutex);
        auto [it,added]=generic.try_emplace(s,(uint64_t(1)<<63)|values.size());
        if (added) values.push_back(s);
        return it->second;
    }
    std::string text(uint64_t k) const {
        if (k>>63) return std::string(values[k&~(uint64_t(1)<<63)]);
        std::string s(1,prefix);
        for (int shift=56;shift>=0;shift-=8) {
            unsigned char c=k>>shift;
            if (!c) break;
            s.push_back(char(c));
        }
        return s;
    }
    bool less(uint64_t a,uint64_t b) const {
        if (!(a>>63) && !(b>>63)) return a<b;
        return text(a)<text(b);
    }
};
struct Slot { uint64_t key=0; uint32_t value=0; };
struct Flat {
    std::vector<Slot> slots;
    size_t used=0;
    explicit Flat(size_t count=0) { reserve(count); }
    void reserve(size_t count) {
        size_t n=16;
        while (n<count+count/2) n*=2;
        if (n<=slots.size()) return;
        auto old=std::move(slots); slots.resize(n);
        for (size_t i=0;i<old.size();++i) if (old[i].value) {
            size_t at=mix(old[i].key)&(n-1);
            while (slots[at].value) at=(at+1)&(n-1);
            slots[at]=old[i];
        }
    }
    uint32_t find(uint64_t k) const {
        size_t at=mix(k)&(slots.size()-1);
        while (slots[at].value) {
            if (slots[at].key==k) return slots[at].value;
            at=(at+1)&(slots.size()-1);
        }
        return 0;
    }
    std::pair<uint32_t,bool> add(uint64_t k,uint32_t value) {
        if ((used+1)*3>slots.size()*2) reserve(slots.size());
        size_t at=mix(k)&(slots.size()-1);
        while (slots[at].value) {
            if (slots[at].key==k) return {slots[at].value,false};
            at=(at+1)&(slots.size()-1);
        }
        slots[at]={k,value}; ++used;
        return {value,true};
    }
};
struct People {
    const Keys& keys;
    explicit People(const Keys& k):keys(k) {}
    Flat ids;
    std::vector<uint32_t> parent;
    std::vector<uint8_t> rank;
    std::vector<uint64_t> minimum;
    uint32_t add(uint64_t k) {
        if (parent.size()>=UINT32_MAX) throw Unsupported{};
        auto [v,added]=ids.add(k,uint32_t(parent.size()+1));
        if (added) { parent.push_back(parent.size()); rank.push_back(0); minimum.push_back(k); }
        return v-1;
    }
    uint32_t root(uint32_t i) {
        uint32_t r=i;
        while (parent[r]!=r) r=parent[r];
        while (parent[i]!=i) { uint32_t next=parent[i]; parent[i]=r; i=next; }
        return r;
    }
    void join(uint64_t a,uint64_t b) {
        uint32_t x=root(add(a)),y=root(add(b));
        if (x==y) return;
        if (rank[x]<rank[y]) std::swap(x,y);
        parent[y]=x; if (keys.less(minimum[y],minimum[x])) minimum[x]=minimum[y];
        if (rank[x]==rank[y]) ++rank[x];
    }
    void finish() {
        for (auto& slot:ids.slots) if (slot.value) {
            slot.value=root(slot.value-1)+1;
        }
    }
    uint64_t person(uint64_t k) const { uint32_t v=ids.find(k); return v ? minimum[v-1] : k; }
};

inline bool numeric(SV s,SV prefix,uint32_t& value) {
    if (!s.starts_with(prefix)) return false;
    s.remove_prefix(prefix.size());
    if (s.empty() || s.size()>10 || (s.size()>1 && s.front()=='0')) return false;
    uint64_t v;
    if (!digits(s,UINT32_MAX,v)) return false;
    value=uint32_t(v); return true;
}
struct Lookup {
    const FlatIds& generic;
    SV prefix; std::vector<uint32_t> dense;
    Lookup(const FlatIds& source,SV p):generic(source),prefix(p) {
        uint32_t maximum=0; size_t count=0;
        generic.each([&](SV id,uint32_t) { ++count; uint32_t n; if (numeric(id,prefix,n)) maximum=std::max(maximum,n); });
        // shortcut: sparse numeric catalogs use exact generic lookup instead of allocating huge indexes.
        if (uint64_t(maximum)>count*4+4096 || maximum>16*1024*1024) return;
        dense.resize(size_t(maximum)+1,UINT32_MAX);
        generic.each([&](SV id,uint32_t index) { uint32_t n; if (numeric(id,prefix,n)) dense[n]=index; });
    }
    uint32_t get(SV id,uint32_t missing) const {
        uint32_t n;
        if (numeric(id,prefix,n)) return n<dense.size() && dense[n]!=UINT32_MAX ? dense[n] :
            dense.empty() ? fallback(id,missing) : missing;
        return fallback(id,missing);
    }
    uint32_t fallback(SV id,uint32_t missing) const {
        return generic.get(id,missing);
    }
};
struct RawEvent { uint64_t id,user; int64_t ts,ingest; uint32_t category,price; };
struct Event { uint64_t user; int64_t ts; uint32_t category,price; };
struct Click { uint64_t user,id; int64_t ts; uint32_t campaign; };
struct PersonEvent { int64_t ts; uint32_t category,price; };
struct PersonClick { uint64_t id; int64_t ts; uint32_t campaign; };
static_assert(sizeof(Event)==24 && sizeof(PersonEvent)==16);
constexpr uint32_t INVALID=UINT32_MAX;

struct DuplicateFilter {
    std::vector<uint64_t> seen,repeated;
    size_t mask;
    explicit DuplicateFilter(size_t bits) : seen(bits/64),repeated(bits/64),mask(bits-1) {
        if (bits<64 || (bits&(bits-1))) throw std::runtime_error("dedup filter needs power-of-two bits >= 64");
    }
    void add(uint64_t id) {
        size_t bit=mix(id)&mask; uint64_t flag=uint64_t(1)<<(bit&63);
        // Atomic insertion plus a completed scan marks every occurrence, including the first.
        if (std::atomic_ref<uint64_t>(seen[bit>>6]).fetch_or(flag,std::memory_order_relaxed)&flag)
            std::atomic_ref<uint64_t>(repeated[bit>>6]).fetch_or(flag,std::memory_order_relaxed);
    }
    bool candidate(uint64_t id) const {
        size_t bit=mix(id)&mask;
        return repeated[bit>>6]&(uint64_t(1)<<(bit&63));
    }
    void scan(const std::vector<Mapping>& maps,size_t threads,Keys& ids) {
        auto work=chunks(maps);
        parallel(work.size(),threads,[&](size_t j,size_t) {
            lines(work[j],[&](SV row) {
                auto comma=static_cast<const char*>(std::memchr(row.data(),',',row.size()));
                if (!comma) return;
                SV id(row.data(),comma-row.data());
                if (id.size()>=2 && id.front()=='"' && id.back()=='"') { id.remove_prefix(1); id.remove_suffix(1); }
                // Over-including malformed rows creates only false positives; never validate events here.
                if (!id.empty()) add(ids.get(id));
            });
        });
    }
};
inline size_t filter_bits(const std::vector<Mapping>& maps) {
    size_t bytes=0; for (const auto& map:maps) bytes+=map.size;
    size_t bits=64;
    // shortcut: cap each bitmap at 256 MiB; saturation only adds exact candidates, never loses duplicates.
    while (bits<(size_t(1)<<31) && bits<bytes/4) bits*=2;
    return bits;
}

template<bool EVENTS> void parse(const std::vector<Mapping>& maps,size_t threads,const People& people,
                                Keys& ids,Keys& users,const Lookup& products,const Lookup& campaigns,int64_t start,int64_t end,
                                std::array<Partition<RawEvent>,PARTS>& events,
                                std::array<Partition<Click>,PARTS>& clicks,
                                const DuplicateFilter* filter=nullptr,
                                std::array<Partition<Event>,PARTS>* direct=nullptr) {
    auto work=chunks(maps);
    parallel(work.size(),threads,[&](size_t j,size_t) {
        std::array<std::vector<RawEvent>,PARTS> ev;
        std::array<std::vector<Click>,PARTS> cl;
        std::array<std::vector<Event>,PARTS> unique;
        size_t reserve=size_t(work[j].end-work[j].begin)/64/PARTS+8;
        for (size_t i=0;i<PARTS;++i) {
            if constexpr(EVENTS) {
                if (filter) { ev[i].reserve(reserve/8+8); unique[i].reserve(reserve); }
                else ev[i].reserve(reserve);
            } else cl[i].reserve(reserve);
        }
        DateCache cache;
        CsvReader<EVENTS?7:4> reader(work[j].begin,work[j].end,runtime_scan());
        while (reader.p<reader.end) {
            std::array<SV,EVENTS?7:4> f;
            if (!reader.next(f) || f[0].empty()) continue;
            if constexpr(EVENTS) {
                uint64_t id=ids.get(f[0]);
                size_t part=mix(id)&(PARTS-1);
                int64_t ingest=0,ts=0;
                if (!cache.parse(f[3],ingest)) throw std::runtime_error("invalid ingest_ts");
                uint64_t price=0,user=0; uint32_t category=0;
                bool valid=!f[1].empty() && !f[5].empty() && cache.parse(f[2],ts) && ts>=start && ts<end;
                if (valid) {
                    if (f[4]=="purchase") valid=digits(f[6],10000000,price) && price;
                    else valid=f[4]=="view" || f[4]=="cart";
                }
                if (valid) valid=!(ingest>ts && uint64_t(ingest)-uint64_t(ts)>uint64_t(DAY));
                if (valid) {
                    user=people.person(users.get(f[1]));
                    if (price) category=products.get(f[5],0);
                }
                if (filter && !filter->candidate(id)) {
                    if (valid) unique[mix(user)&(PARTS-1)].push_back({user,ts,category,uint32_t(price)});
                } else ev[part].push_back({id,user,ts,ingest,category,valid ? uint32_t(price) : INVALID});
            } else {
                int64_t ts=0; uint64_t user=0; uint32_t campaign=INVALID;
                if (!f[1].empty() && cache.parse(f[3],ts) && ts>=start-WEEK && ts<end) {
                    campaign=campaigns.get(f[2],INVALID);
                    if (campaign!=INVALID) user=people.person(users.get(f[1]));
                }
                // Official copies have identical parsed values; winner and campaign membership are idempotent.
                if (campaign!=INVALID) cl[mix(user)&(PARTS-1)].push_back({user,ids.get(f[0]),ts,campaign});
            }
        }
        for (size_t i=0;i<PARTS;++i) {
            if constexpr(EVENTS) {
                events[i].add(std::move(ev[i]));
                if (direct) (*direct)[i].add(std::move(unique[i]));
            } else clicks[i].add(std::move(cl[i]));
        }
    });
}
inline void dedup(std::array<Partition<RawEvent>,PARTS>& raw_events,size_t threads,
                  std::array<Partition<Event>,PARTS>& events) {
    parallel(PARTS,threads,[&](size_t p,size_t) {
        auto rows=raw_events[p].flatten();
        if (rows.size()>=UINT32_MAX) throw Unsupported{};
        Flat table(rows.size());
        for (uint32_t i=0;i<rows.size();++i) {
            auto [v,added]=table.add(rows[i].id,i+1);
            if (!added) {
                auto& old=rows[v-1]; const auto& row=rows[i];
                if (row.ingest<old.ingest) old=row;
                else if (row.ingest==old.ingest && (row.user!=old.user || row.ts!=old.ts || row.category!=old.category || row.price!=old.price))
                    throw Unsupported{};
            }
        }
        std::array<std::vector<Event>,PARTS> ev;
        for (auto slot:table.slots) if (slot.value) {
            const auto& row=rows[slot.value-1];
            if (row.price!=INVALID) ev[mix(row.user)&(PARTS-1)].push_back({row.user,row.ts,row.category,row.price});
        }
        for (size_t i=0;i<PARTS;++i) events[i].add(std::move(ev[i]));
    });
}

Result aggregate(Partition<Event>& ep,Partition<Click>& cp,const Catalog& catalog,int64_t first_day,size_t days,
                 const Keys& user_ids,const Keys& click_ids,std::vector<std::string>& names) {
    auto events=ep.flatten(); auto clicks=cp.flatten();
    Flat users(events.size()/8);
    std::vector<uint64_t> keys;
    std::vector<size_t> count,click_count;
    for (auto& event:events) {
        auto [v,added]=users.add(event.user,uint32_t(keys.size()+1));
        if (added) { keys.push_back(event.user); count.push_back(0); click_count.push_back(0); }
        event.user=v-1; ++count[v-1];
    }
    for (auto& click:clicks) {
        uint32_t v=users.find(click.user);
        click.user=v ? v-1 : UINT64_MAX;
        if (v) ++click_count[v-1];
    }
    std::vector<size_t> offsets(count.size()+1),click_offsets(count.size()+1);
    for (size_t i=0;i<count.size();++i) {
        offsets[i+1]=offsets[i]+count[i]; click_offsets[i+1]=click_offsets[i]+click_count[i];
        count[i]=offsets[i]; click_count[i]=click_offsets[i];
    }
    std::vector<PersonEvent> grouped(events.size());
    std::vector<PersonClick> grouped_clicks(click_offsets.back());
    for (const auto& e:events) grouped[count[e.user]++]={e.ts,e.category,e.price};
    for (const auto& c:clicks) if (c.user!=UINT64_MAX) grouped_clicks[click_count[c.user]++]={c.id,c.ts,c.campaign};
    std::vector<Event>().swap(events); std::vector<Click>().swap(clicks);
    names.resize(keys.size());
    Result result; result.campaigns.resize(catalog.campaigns.size()*days);
    std::vector<int64_t> category_revenue(catalog.categories.size());
    std::vector<uint32_t> touched;
    std::vector<size_t> covered(catalog.campaigns.size());
    std::vector<Session> sessions;
    for (size_t user=0;user<keys.size();++user) {
        size_t begin=offsets[user],finish=offsets[user+1],cb=click_offsets[user],ce=click_offsets[user+1];
        std::sort(grouped.begin()+begin,grouped.begin()+finish,[](const auto& a,const auto& b) { return a.ts<b.ts; });
        std::sort(grouped_clicks.begin()+cb,grouped_clicks.begin()+ce,[&](const auto& a,const auto& b) {
            return a.ts==b.ts ? click_ids.less(b.id,a.id) : a.ts<b.ts;
        });
        names[user]=user_ids.text(keys[user]); SV person=names[user];
        sessions.clear(); touched.clear();
        int64_t previous=0; UserRow daily{};
        auto emit=[&] {
            if (!daily.events) return;
            int64_t best=-1; SV category;
            for (uint32_t c:touched) {
                int64_t value=category_revenue[c];
                if (value>best || (value==best && catalog.categories[c]<category)) { best=value; category=catalog.categories[c]; }
                category_revenue[c]=0;
            }
            daily.has_category=best>=0; daily.category=category;
            result.users.push_back(daily); touched.clear();
        };
        for (size_t i=begin;i<finish;++i) {
            const auto& e=grouped[i];
            if (i==begin || e.ts-previous>GAP) {
                int32_t day=day_of(e.ts);
                if (i==begin || daily.day!=day) { emit(); daily={person,{},day,0,0,0,false}; }
                ++daily.sessions; sessions.push_back({e.ts,0});
            }
            previous=e.ts; ++daily.events; daily.revenue+=e.price; sessions.back().revenue+=e.price;
            if (e.price) {
                if (!category_revenue[e.category]) touched.push_back(e.category);
                category_revenue[e.category]+=e.price;
            }
        }
        emit();
        attribute(sessions,grouped_clicks.empty() ? nullptr : grouped_clicks.data()+cb,ce-cb,first_day,days,result.campaigns,covered);
    }
    return result;
}

inline bool run(const fs::path& data,const fs::path& output,size_t threads,int64_t start,int64_t end,
                int64_t first_day,size_t days) {
    auto clock=std::chrono::steady_clock::now(),last=clock;
    auto timing=[&](const char* stage) { auto now=std::chrono::steady_clock::now();
        std::cerr<<"sprint "<<stage<<" "<<std::chrono::duration<double>(now-last).count()<<"s\n"; last=now; };
    try {
        auto link_maps=map_files(files(data/"links"));
        Keys user_ids('u'),event_ids('e'),click_ids('k');
        People people(user_ids);
        for (auto chunk:chunks(link_maps)) {
            CsvReader<2> reader(chunk.begin,chunk.end,runtime_scan());
            while (reader.p<reader.end) {
                std::array<SV,2> f;
                if (reader.next(f) && !f[0].empty() && !f[1].empty()) people.join(user_ids.get(f[0]),user_ids.get(f[1]));
            }
        }
        people.finish();
        Mapping product_map(data/"products.csv"),campaign_map(data/"campaigns.csv");
        Catalog catalog; catalog.products_from(product_map); catalog.campaigns_from(campaign_map);
        Lookup products(catalog.products,"p"),campaigns(catalog.campaign_ids,"c_");
        timing("identity/catalog");
        auto event_maps=map_files(files(data/"events")),click_maps=map_files(files(data/"clicks"));
        std::array<Partition<Event>,PARTS> events;
        std::array<Partition<Click>,PARTS> clicks;
        {
            std::array<Partition<RawEvent>,PARTS> raw_events;
            if (setting("SS_DEDUP","1")=="1") {
                DuplicateFilter filter(filter_bits(event_maps));
                filter.scan(event_maps,threads,event_ids); timing("event ID prefilter");
                parse<true>(event_maps,threads,people,event_ids,user_ids,products,campaigns,start,end,raw_events,clicks,&filter,&events);
                timing("event parse/validate/direct");
            } else {
                parse<true>(event_maps,threads,people,event_ids,user_ids,products,campaigns,start,end,raw_events,clicks);
                timing("event parse/validate");
            }
            size_t candidates=0; for (const auto& part:raw_events) for (const auto& block:part.blocks) candidates+=block.size();
            std::cerr<<"dedup exact candidate rows "<<candidates<<'\n';
            dedup(raw_events,threads,events); timing("event dedup");
        }
        {
            std::array<Partition<RawEvent>,PARTS> raw_events;
            parse<false>(click_maps,threads,people,click_ids,user_ids,products,campaigns,start,end,raw_events,clicks); timing("click parse/validate/direct");
        }
        // All unsupported-shape checks finish before any output file is opened.
        std::vector<Stats> campaign_stats(catalog.campaigns.size()*days);
        std::mutex mutex;
        parallel(PARTS,threads,[&](size_t p,size_t) {
            std::vector<std::string> names;
            auto result=aggregate(events[p],clicks[p],catalog,first_day,days,user_ids,click_ids,names);
            if (!result.users.empty()) write_users(output/"user_daily"/("part-"+std::to_string(p)+".parquet"),result.users);
            std::lock_guard lock(mutex);
            for (size_t i=0;i<campaign_stats.size();++i) campaign_stats[i]+=result.campaigns[i];
        });
        if (fs::is_empty(output/"user_daily")) write_users(output/"user_daily"/"part-0.parquet",{});
        write_campaigns(output/"campaign_daily"/"part-0.parquet",campaign_stats,catalog,first_day,days);
        timing("person blocks/session/attribution/write");
        std::cerr<<"sprint total "<<std::chrono::duration<double>(std::chrono::steady_clock::now()-clock).count()<<"s\n";
        return true;
    } catch (const Unsupported&) {
        std::cerr<<"sprint unsupported shape; restarting baseline\n";
        return false;
    }
}
} // namespace sprint
