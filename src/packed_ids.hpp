#pragma once

namespace v5 {
// Short keys encode their exact nonzero ASCII bytes; long/arbitrary bytes stay in exact fallback.
inline uint64_t opaque_key(SV id,char prefix) {
    if (id.size()<=9 && !id.empty() && id.front()==prefix) {
        uint64_t packed=0;
        if (id.size()>1) std::memcpy(&packed,id.data()+1,id.size()-1);
        constexpr uint64_t high=0x8080808080808080ULL;
        uint64_t active=id.size()==9 ? UINT64_MAX : (uint64_t(1)<<(8*(id.size()-1)))-1;
        uint64_t zeros=(packed-0x0101010101010101ULL)&~packed&high;
        if (!(packed&high) && !(zeros&active)) return packed;
    }
    return hash(id)|(uint64_t(1)<<63);
}
inline uint64_t alias_key(SV id) { return opaque_key(id,'u'); }
inline uint64_t event_key(SV id) { return opaque_key(id,'e'); }
struct PackedIds {
    struct Slot { uint64_t key=0; uint32_t value=0; };
    std::vector<Slot> slots;
    size_t used=0;
    void reserve(size_t count) {
        size_t size=16; while (size<count+count/2) size*=2;
        if (size<=slots.size()) return;
        auto old=std::move(slots); slots.resize(size);
        for (auto slot:old) if (slot.value) {
            size_t at=mix(slot.key)&(size-1);
            while(slots[at].value) at=(at+1)&(size-1);
            slots[at]=slot;
        }
    }
    template<class Equal> std::pair<uint32_t,bool> add(uint64_t key,uint32_t value,Equal equal) {
        if (slots.empty() || (used+1)*3>slots.size()*2) reserve(std::max<size_t>(16,slots.size()));
        size_t at=mix(key)&(slots.size()-1);
        while (slots[at].value) {
            if (slots[at].key==key && (!(key>>63) || equal(slots[at].value-1))) return {slots[at].value-1,false};
            at=(at+1)&(slots.size()-1);
        }
        slots[at]={key,value+1}; ++used; return {value,true};
    }
};
} // namespace v5
