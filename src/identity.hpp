#pragma once
#include <immintrin.h>

namespace v6 {
// CRC is only a fingerprint: length and all bytes still decide identity.
__attribute__((target("sse4.2"))) inline uint64_t key_hash(SV s) {
    uint64_t a=0x9e3779b9U ^ s.size(),b=0x85ebca6bU;
    const char* p=s.data(); size_t n=s.size();
    while (n>=16) {
        uint64_t x,y; std::memcpy(&x,p,8); std::memcpy(&y,p+8,8);
        a=_mm_crc32_u64(a,x); b=_mm_crc32_u64(b,y); p+=16; n-=16;
    }
    if (n>=8) { uint64_t x; std::memcpy(&x,p,8); a=_mm_crc32_u64(a,x); p+=8; n-=8; }
    uint64_t tail=0; if (n) std::memcpy(&tail,p,n);
    b=_mm_crc32_u64(b,tail);
    return mix((a<<32)|uint32_t(b));
}
struct ExactNames {
    struct Slot { const char* data=nullptr; uint64_t packed=0; uint32_t fingerprint=0,index=0,length=0,value=0; };
    std::vector<Slot> slots;
    std::vector<SV> keys;
    std::vector<uint32_t> values;
    void reserve(size_t count) {
        size_t capacity=16; while (capacity<count+count/2) capacity*=2;
        if (capacity<=slots.size()) return;
        slots.assign(capacity,{});
        for (uint32_t i=0;i<keys.size();++i) place(i,key_hash(keys[i]));
    }
    static uint64_t pack(SV key) {
        uint64_t value=0;
        if (key.size()<=8 && !key.empty()) std::memcpy(&value,key.data(),key.size());
        return value;
    }
    bool equal(const Slot& slot,SV key,uint64_t packed) const {
        return slot.length==key.size() &&
               (key.size()<=8 ? slot.packed==packed : SV(slot.data,slot.length)==key);
    }
    void place(uint32_t i,uint64_t hash) {
        size_t at=uint32_t(hash)&(slots.size()-1);
        while (slots[at].index) at=(at+1)&(slots.size()-1);
        slots[at]={keys[i].data(),pack(keys[i]),uint32_t(hash>>32),i+1,uint32_t(keys[i].size()),values[i]};
    }
    uint32_t find(SV key,uint64_t hash) const {
        if (slots.empty()) return UINT32_MAX;
        size_t at=uint32_t(hash)&(slots.size()-1); uint32_t fp=hash>>32; uint64_t packed=pack(key);
        while (slots[at].index) {
            auto slot=slots[at];
            if (slot.fingerprint==fp && equal(slot,key,packed)) return slot.value;
            at=(at+1)&(slots.size()-1);
        }
        return UINT32_MAX;
    }
    uint32_t add(SV key,uint64_t hash) {
        if (slots.empty() || (keys.size()+1)*3>slots.size()*2) reserve(std::max<size_t>(16,slots.size()));
        size_t at=uint32_t(hash)&(slots.size()-1); uint32_t fp=hash>>32; uint64_t packed=pack(key);
        while (slots[at].index) {
            auto slot=slots[at];
            if (slot.fingerprint==fp && equal(slot,key,packed)) return slot.index-1;
            at=(at+1)&(slots.size()-1);
        }
        if (keys.size()>=UINT32_MAX-1) throw std::runtime_error("too many exact names");
        uint32_t index=keys.size(); keys.push_back(key); values.push_back(index);
        slots[at]={key.data(),packed,fp,index+1,uint32_t(key.size()),index}; return index;
    }
};
struct Link { SV a,b; uint32_t x=0,y=0; };
struct Linked {
    static constexpr size_t SHARDS=128;
    struct Shard { ExactNames map; uint32_t base=0; };
    std::array<Shard,SHARDS> shards;
    std::vector<uint32_t> parent;
    std::vector<uint8_t> rank,parts;
    std::vector<SV> names;
    uint32_t root(uint32_t i) {
        uint32_t r=i; while (parent[r]!=r) r=parent[r];
        while (parent[i]!=i) { auto next=parent[i]; parent[i]=r; i=next; }
        return r;
    }
    template<class Pool> void build(std::vector<std::vector<Link>>& chunks,Pool& pool) {
        struct Endpoint { uint32_t chunk,row; bool second; SV key; uint64_t hash; };
        std::vector<std::array<std::vector<Endpoint>,SHARDS>> buckets(chunks.size());
        pool.run(chunks.size(),[&](size_t c,size_t) {
            for (uint32_t r=0;r<chunks[c].size();++r) {
                auto& link=chunks[c][r];
                for (bool second:{false,true}) {
                    SV id=second ? link.b : link.a; uint64_t h=key_hash(id);
                    buckets[c][(h>>32)&(SHARDS-1)].push_back({uint32_t(c),r,second,id,h});
                }
            }
        });
        pool.run(SHARDS,[&](size_t p,size_t) {
            auto& map=shards[p].map; size_t count=0;
            for (const auto& chunk:buckets) count+=chunk[p].size();
            map.reserve(count/2+16);
            for (const auto& chunk:buckets) for (const auto& e:chunk[p]) {
                uint32_t id=map.add(e.key,e.hash); auto& link=chunks[e.chunk][e.row];
                if (e.second) link.y=id; else link.x=id;
            }
        });
        size_t count=0;
        for (auto& s:shards) { s.base=uint32_t(count); count+=s.map.keys.size(); }
        if (count>=UINT32_MAX) throw std::runtime_error("too many linked IDs");
        parent.resize(count); rank.resize(count); names.resize(count); parts.resize(count);
        pool.run(SHARDS,[&](size_t p,size_t) {
            auto& s=shards[p];
            for (size_t i=0;i<s.map.keys.size();++i) { parent[s.base+i]=s.base+i; names[s.base+i]=s.map.keys[i]; }
        });
        for (const auto& chunk:chunks) for (const auto& link:chunk) {
            uint32_t x=root(shards[(key_hash(link.a)>>32)&(SHARDS-1)].base+link.x);
            uint32_t y=root(shards[(key_hash(link.b)>>32)&(SHARDS-1)].base+link.y);
            if (x==y) continue;
            if (rank[x]<rank[y]) std::swap(x,y);
            parent[y]=x; names[x]=std::min(names[x],names[y]); if (rank[x]==rank[y]) ++rank[x];
        }
        for (uint32_t i=0;i<count;++i) parent[i]=root(i);
        pool.run(SHARDS,[&](size_t p,size_t) {
            auto& s=shards[p];
            for (size_t i=0;i<s.map.values.size();++i) s.map.values[i]=parent[s.base+i];
            for (auto& slot:s.map.slots) if (slot.index) slot.value=s.map.values[slot.index-1];
        });
        pool.run(count,[&](size_t i,size_t) { parts[i]=uint8_t(key_hash(names[parent[i]])&(SHARDS-1)); });
        std::vector<uint8_t>().swap(rank);
    }
    uint32_t find(SV key,uint64_t hash) const { return shards[(hash>>32)&(SHARDS-1)].map.find(key,hash); }
};
} // namespace v6
