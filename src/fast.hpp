#pragma once
#include <immintrin.h>

namespace sprint {
struct Unsupported {};

// shortcut: nonzero ASCII suffixes up to eight bytes; other IDs use tagged exact string keys.
inline bool key(SV s, char prefix, uint64_t& value) {
    if (s.empty() || s.front()!=prefix || s.size()>9) return false;
    if (s.size()==9) {
        uint64_t bytes; std::memcpy(&bytes,s.data()+1,8);
        constexpr uint64_t high=0x8080808080808080ULL;
        if ((bytes&high) || ((bytes-0x0101010101010101ULL)&~bytes&high)) return false;
        value=__builtin_bswap64(bytes); return true;
    }
    value=0;
    for (size_t i=1;i<s.size();++i) {
        unsigned char c=s[i];
        if (!c || c>=128) return false;
        value |= uint64_t(c) << (64-8*i);
    }
    return true;
}
struct Keys {
    char prefix;
    std::mutex mutex;
    std::unordered_map<SV,uint64_t,Hasher> generic;
    std::vector<SV> values;
    explicit Keys(char p):prefix(p) {}
    uint64_t get(SV s) {
        uint64_t packed;
        if (key(s,prefix,packed)) return packed;
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

// One SIMD pass finds row boundaries, separators, and every quote; malformed rows are skipped.
template<size_t N> bool csv_next(const char*& p,const char* end,std::array<SV,N>& fields) {
    const char* row=p; const char* start=p; const char* first_quote=nullptr; const char* last_quote=nullptr;
    size_t count=0,quotes=0; bool valid=true;
    auto field=[&](const char* stop) {
        SV s(start,stop-start);
        if (quotes) {
            if (quotes!=2 || first_quote!=start || last_quote!=stop-1) valid=false;
            else { s.remove_prefix(1); s.remove_suffix(1); }
        }
        if (count<N) fields[count]=s;
        else valid=false;
        ++count; start=stop==end ? end : stop+1; quotes=0; first_quote=last_quote=nullptr;
    };
    const char* q=p;
    while (q<end) {
        unsigned mask=0; size_t step=1;
        if (end-q>=16) {
            __m128i bytes=_mm_loadu_si128(reinterpret_cast<const __m128i*>(q));
            auto sep=_mm_or_si128(_mm_cmpeq_epi8(bytes,_mm_set1_epi8(',')),
                                 _mm_cmpeq_epi8(bytes,_mm_set1_epi8('\n')));
            mask=unsigned(_mm_movemask_epi8(_mm_or_si128(sep,_mm_cmpeq_epi8(bytes,_mm_set1_epi8('"')))));
            step=16;
        } else if (*q==',' || *q=='\n' || *q=='"') mask=1;
        while (mask) {
            unsigned bit=__builtin_ctz(mask); mask &= mask-1;
            const char* at=q+bit;
            if (*at=='"') { if (!quotes) first_quote=at; last_quote=at; ++quotes; }
            else {
                field(at);
                if (*at=='\n') {
                    p=at+1;
                    if (uint64_t(at-row)>UINT32_MAX) throw std::runtime_error("CSV row exceeds 4 GiB");
                    return valid && count==N;
                }
            }
        }
        q+=step;
    }
    field(end); p=end;
    if (uint64_t(end-row)>UINT32_MAX) throw std::runtime_error("CSV row exceeds 4 GiB");
    return valid && count==N;
}
struct DateCache {
    struct Date { uint64_t head=0; uint16_t tail=0; int64_t midnight=0; };
    std::array<Date,256> dates{};
    bool parse(SV s,int64_t& out) {
        if ((s.size()!=20 && s.size()!=25) || s[4]!='-') return timestamp(s,out);
        int64_t offset=0;
        if (s.size()==20) { if (s[19]!='Z') return false; }
        else {
            if ((s[19]!='+' && s[19]!='-') || s[22]!=':') return false;
            for (int i:{20,21,23,24}) if (s[i]<'0' || s[i]>'9') return false;
            unsigned h=(s[20]-'0')*10+s[21]-'0',m=(s[23]-'0')*10+s[24]-'0';
            if (h>23 || m>59) return false;
            offset=int64_t(h*60+m)*60000*(s[19]=='+' ? 1 : -1);
        }
        if (s[7]!='-' || s[10]!='T' || s[13]!=':' || s[16]!=':') return false;
        __m128i bytes=_mm_loadu_si128(reinterpret_cast<const __m128i*>(s.data()));
        auto valid=_mm_and_si128(_mm_cmpgt_epi8(bytes,_mm_set1_epi8('/')),
                                _mm_cmpgt_epi8(_mm_set1_epi8(':'),bytes));
        constexpr unsigned digits_mask=0xf | (3<<5) | (3<<8) | (3<<11) | (3<<14);
        if ((unsigned(_mm_movemask_epi8(valid))&digits_mask)!=digits_mask ||
            s[17]<'0' || s[17]>'9' || s[18]<'0' || s[18]>'9') return false;
        auto two=[&](size_t i) { return unsigned(s[i]-'0')*10+unsigned(s[i+1]-'0'); };
        unsigned h=two(11),m=two(14),sec=two(17);
        if (h>23 || m>59 || sec>59) return false;
        int64_t tod=(h*3600+m*60+sec)*1000;
        uint64_t head; uint16_t tail;
        std::memcpy(&head,s.data(),8); std::memcpy(&tail,s.data()+8,2);
        auto& date=dates[mix(head^tail)&255];
        if (date.head!=head || date.tail!=tail) {
            if (!timestamp(s,out)) return false;
            date={head,tail,out-tod+offset};
        } else out=date.midnight+tod-offset;
        return true;
    }
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
struct RawClick { uint64_t id,user; int64_t ts; uint32_t campaign; };
struct Event { uint64_t user; int64_t ts; uint32_t category,price; };
struct Click { uint64_t user,id; int64_t ts; uint32_t campaign; };
struct PersonEvent { int64_t ts; uint32_t category,price; };
struct PersonClick { uint64_t id; int64_t ts; uint32_t campaign; };
static_assert(sizeof(Event)==24 && sizeof(PersonEvent)==16);
constexpr uint32_t INVALID=UINT32_MAX;

inline void sort_events(PersonEvent* rows,size_t count,std::vector<PersonEvent>& scratch) {
    if (count<2) return;
    if (count<1024) {
        std::sort(rows,rows+count,[](const auto& a,const auto& b) { return a.ts<b.ts; }); return;
    }
    uint64_t first=uint64_t(rows[0].ts)^(uint64_t(1)<<63),vary=0;
    for (size_t i=1;i<count;++i) vary|=(uint64_t(rows[i].ts)^(uint64_t(1)<<63))^first;
    if (!vary) return;
    scratch.resize(count); auto* src=rows; auto* dst=scratch.data();
    for (unsigned shift=0;shift<64;shift+=8) {
        if (!(vary&(uint64_t(255)<<shift))) continue;
        std::array<size_t,256> offsets{};
        for (size_t i=0;i<count;++i) ++offsets[((uint64_t(src[i].ts)^(uint64_t(1)<<63))>>shift)&255];
        size_t start=0;
        for (auto& value:offsets) { size_t n=value; value=start; start+=n; }
        for (size_t i=0;i<count;++i) dst[offsets[((uint64_t(src[i].ts)^(uint64_t(1)<<63))>>shift)&255]++]=src[i];
        std::swap(src,dst);
    }
    if (src!=rows) std::copy(src,src+count,rows);
}

template<bool EVENTS> void parse(const std::vector<Mapping>& maps,size_t threads,const People& people,
                                Keys& ids,Keys& users,const Lookup& products,const Lookup& campaigns,int64_t start,int64_t end,
                                std::array<Partition<RawEvent>,PARTS>& events,
                                std::array<Partition<RawClick>,PARTS>& clicks) {
    auto work=chunks(maps);
    parallel(work.size(),threads,[&](size_t j,size_t) {
        std::array<std::vector<RawEvent>,PARTS> ev;
        std::array<std::vector<RawClick>,PARTS> cl;
        size_t reserve=size_t(work[j].end-work[j].begin)/64/PARTS+8;
        for (size_t i=0;i<PARTS;++i) {
            if constexpr(EVENTS) ev[i].reserve(reserve);
            else cl[i].reserve(reserve);
        }
        DateCache cache;
        const char* p=work[j].begin;
        while (p<work[j].end) {
            std::array<SV,EVENTS?7:4> f;
            if (!csv_next(p,work[j].end,f) || f[0].empty()) continue;
            uint64_t id=ids.get(f[0]);
            size_t part=mix(id)&(PARTS-1);
            if constexpr(EVENTS) {
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
                ev[part].push_back({id,user,ts,ingest,category,valid ? uint32_t(price) : INVALID});
            } else {
                int64_t ts=0; uint64_t user=0; uint32_t campaign=INVALID;
                if (!f[1].empty() && cache.parse(f[3],ts) && ts>=start-WEEK && ts<end) {
                    campaign=campaigns.get(f[2],INVALID);
                    if (campaign!=INVALID) user=people.person(users.get(f[1]));
                }
                cl[part].push_back({id,user,ts,campaign});
            }
        }
        for (size_t i=0;i<PARTS;++i) {
            if constexpr(EVENTS) events[i].add(std::move(ev[i]));
            else clicks[i].add(std::move(cl[i]));
        }
    });
}
template<bool EVENTS> void dedup(std::array<Partition<RawEvent>,PARTS>& raw_events,
                                std::array<Partition<RawClick>,PARTS>& raw_clicks,size_t threads,
                                std::array<Partition<Event>,PARTS>& events,
                                std::array<Partition<Click>,PARTS>& clicks) {
    parallel(PARTS,threads,[&](size_t p,size_t) {
        auto& blocks=[&]() -> auto& { if constexpr(EVENTS) return raw_events[p].blocks; else return raw_clicks[p].blocks; }();
        size_t count=0; for (const auto& block:blocks) count+=block.size();
        if (!count) return;
        if (count>=UINT32_MAX) throw Unsupported{};
        using Row=std::conditional_t<EVENTS,RawEvent,RawClick>;
        size_t capacity=16; while (capacity<count+count/2) capacity*=2;
        std::vector<uint32_t> table(capacity);
        std::vector<const Row*> winners; winners.reserve(count);
        // Keep stable block pointers and four-byte slots: no raw copy or sparse output scan.
        for (const auto& block:blocks) for (const auto& row:block) {
            size_t at=mix(row.id)&(capacity-1);
            while (table[at] && winners[table[at]-1]->id!=row.id) at=(at+1)&(capacity-1);
            uint32_t v=table[at];
            if (!v) { winners.push_back(&row); table[at]=uint32_t(winners.size()); }
            else {
                const auto& old=*winners[v-1];
                if constexpr(EVENTS) {
                    if (row.ingest<old.ingest) winners[v-1]=&row;
                    else if (row.ingest==old.ingest && (row.user!=old.user || row.ts!=old.ts || row.category!=old.category || row.price!=old.price))
                        throw Unsupported{};
                } else if (row.user!=old.user || row.ts!=old.ts || row.campaign!=old.campaign) throw Unsupported{};
            }
        }
        std::array<std::vector<Event>,PARTS> ev;
        std::array<std::vector<Click>,PARTS> cl;
        for (const auto* winner:winners) {
            const auto& row=*winner;
            if constexpr(EVENTS) {
                if (row.price!=INVALID) ev[mix(row.user)&(PARTS-1)].push_back({row.user,row.ts,row.category,row.price});
            } else if (row.campaign!=INVALID) cl[mix(row.user)&(PARTS-1)].push_back({row.user,row.id,row.ts,row.campaign});
        }
        blocks.clear();
        for (size_t i=0;i<PARTS;++i) {
            if constexpr(EVENTS) events[i].add(std::move(ev[i]));
            else clicks[i].add(std::move(cl[i]));
        }
    });
}

Result aggregate(Partition<Event>& ep,Partition<Click>& cp,const Catalog& catalog,int64_t first_day,size_t days,
                 const Keys& user_ids,const Keys& click_ids,std::vector<std::string>& names) {
    size_t event_count=0; for (const auto& block:ep.blocks) event_count+=block.size();
    Flat users(event_count/8);
    std::vector<uint64_t> keys;
    std::vector<size_t> count,click_count;
    for (auto& block:ep.blocks) for (auto& event:block) {
        auto [v,added]=users.add(event.user,uint32_t(keys.size()+1));
        if (added) { keys.push_back(event.user); count.push_back(0); click_count.push_back(0); }
        event.user=v-1; ++count[v-1];
    }
    for (auto& block:cp.blocks) for (auto& click:block) {
        uint32_t v=users.find(click.user);
        click.user=v ? v-1 : UINT64_MAX;
        if (v) ++click_count[v-1];
    }
    std::vector<size_t> offsets(count.size()+1),click_offsets(count.size()+1);
    for (size_t i=0;i<count.size();++i) {
        offsets[i+1]=offsets[i]+count[i]; click_offsets[i+1]=click_offsets[i]+click_count[i];
        count[i]=offsets[i]; click_count[i]=click_offsets[i];
    }
    std::vector<PersonEvent> grouped(event_count);
    std::vector<PersonClick> grouped_clicks(click_offsets.back());
    for (const auto& block:ep.blocks) for (const auto& e:block) grouped[count[e.user]++]={e.ts,e.category,e.price};
    for (const auto& block:cp.blocks) for (const auto& c:block)
        if (c.user!=UINT64_MAX) grouped_clicks[click_count[c.user]++]={c.id,c.ts,c.campaign};
    ep.blocks.clear(); cp.blocks.clear();
    names.resize(keys.size());
    Result result; result.campaigns.resize(catalog.campaigns.size()*days);
    std::vector<int64_t> category_revenue(catalog.categories.size());
    std::vector<uint32_t> touched;
    std::vector<size_t> covered(catalog.campaigns.size());
    std::vector<Session> sessions;
    std::vector<PersonEvent> sort_scratch;
    for (size_t user=0;user<keys.size();++user) {
        size_t begin=offsets[user],finish=offsets[user+1],cb=click_offsets[user],ce=click_offsets[user+1];
        sort_events(grouped.data()+begin,finish-begin,sort_scratch);
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
            const char* p=chunk.begin;
            while (p<chunk.end) {
                std::array<SV,2> f;
                if (csv_next(p,chunk.end,f) && !f[0].empty() && !f[1].empty()) people.join(user_ids.get(f[0]),user_ids.get(f[1]));
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
            std::array<Partition<RawClick>,PARTS> raw_clicks;
            parse<true>(event_maps,threads,people,event_ids,user_ids,products,campaigns,start,end,raw_events,raw_clicks); timing("event parse/validate");
            dedup<true>(raw_events,raw_clicks,threads,events,clicks); timing("event dedup");
        }
        {
            std::array<Partition<RawEvent>,PARTS> raw_events;
            std::array<Partition<RawClick>,PARTS> raw_clicks;
            parse<false>(click_maps,threads,people,click_ids,user_ids,products,campaigns,start,end,raw_events,raw_clicks); timing("click parse/validate");
            dedup<false>(raw_events,raw_clicks,threads,events,clicks); timing("click dedup");
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
