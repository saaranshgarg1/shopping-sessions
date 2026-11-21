#pragma once
#include "core.hpp"
#include <cstdlib>
#include <immintrin.h>
#include <stdexcept>

namespace sprint {
using ss::SV;
struct Masks { uint64_t ends, newline, quotes; };
using Scan = Masks (*)(const char*, size_t);

inline Masks scan_sse2(const char* p,size_t size) {
    Masks masks{};
    size_t i=0;
    for (;i+16<=size;i+=16) {
        auto bytes=_mm_loadu_si128(reinterpret_cast<const __m128i*>(p+i));
        auto comma=_mm_cmpeq_epi8(bytes,_mm_set1_epi8(','));
        auto newline=_mm_cmpeq_epi8(bytes,_mm_set1_epi8('\n'));
        masks.ends |= uint64_t(unsigned(_mm_movemask_epi8(_mm_or_si128(comma,newline))))<<i;
        masks.newline |= uint64_t(unsigned(_mm_movemask_epi8(newline)))<<i;
        masks.quotes |= uint64_t(unsigned(_mm_movemask_epi8(_mm_cmpeq_epi8(bytes,_mm_set1_epi8('"')))))<<i;
    }
    for (;i<size;++i) {
        masks.ends |= uint64_t(p[i]==',' || p[i]=='\n')<<i;
        masks.newline |= uint64_t(p[i]=='\n')<<i;
        masks.quotes |= uint64_t(p[i]=='"')<<i;
    }
    return masks;
}
__attribute__((target("avx2"))) inline Masks scan_avx2(const char* p,size_t size) {
    // AVX2 has no byte-masked load; copy only the final short block.
    alignas(32) char tail[64]{};
    if (size<64) { std::memcpy(tail,p,size); p=tail; }
    Masks masks{};
    for (unsigned i=0;i<64;i+=32) {
        auto bytes=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p+i));
        auto comma=_mm256_cmpeq_epi8(bytes,_mm256_set1_epi8(','));
        auto newline=_mm256_cmpeq_epi8(bytes,_mm256_set1_epi8('\n'));
        masks.ends |= uint64_t(unsigned(_mm256_movemask_epi8(_mm256_or_si256(comma,newline))))<<i;
        masks.newline |= uint64_t(unsigned(_mm256_movemask_epi8(newline)))<<i;
        masks.quotes |= uint64_t(unsigned(_mm256_movemask_epi8(_mm256_cmpeq_epi8(bytes,_mm256_set1_epi8('"')))))<<i;
    }
    return masks;
}
__attribute__((target("avx512f,avx512bw"))) inline Masks scan_avx512(const char* p,size_t size) {
    uint64_t valid=size==64 ? UINT64_MAX : (uint64_t(1)<<size)-1;
    auto bytes=_mm512_maskz_loadu_epi8(valid,p);
    uint64_t newline=_mm512_cmpeq_epi8_mask(bytes,_mm512_set1_epi8('\n'));
    return {newline | _mm512_cmpeq_epi8_mask(bytes,_mm512_set1_epi8(',')), newline,
            _mm512_cmpeq_epi8_mask(bytes,_mm512_set1_epi8('"'))};
}
inline Scan choose_scan(bool avx2,bool avx512,SV mode) {
    if (mode!="auto" && mode!="sse2" && mode!="avx2" && mode!="avx512")
        throw std::runtime_error("SS_SIMD must be auto, sse2, avx2, or avx512");
    if (mode!="sse2" && mode!="avx2" && avx512) return scan_avx512;
    if (mode!="sse2" && avx2) return scan_avx2;
    return scan_sse2;
}
inline Scan runtime_scan() {
    const char* mode=std::getenv("SS_SIMD");
    // Compiler CPU checks include OS XSAVE state; unsupported ISA never executes.
    return choose_scan(__builtin_cpu_supports("avx2"),
        __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw"),mode ? mode : "auto");
}

template<size_t N> struct CsvReader {
    const char* p;
    const char* end;
    Scan scan;
    const char* block=nullptr;
    size_t size=0;
    Masks masks{};
    CsvReader(const char* begin,const char* finish,Scan scanner):p(begin),end(finish),scan(scanner) {}
    bool next(std::array<SV,N>& fields) {
        const char* row=p;
        const char* start=p;
        const char* first_quote=nullptr;
        const char* last_quote=nullptr;
        size_t count=0,quotes=0;
        bool valid=true;
        auto add_quotes=[&](uint64_t bits) {
            if (!bits) return;
            if (!quotes) first_quote=block+__builtin_ctzll(bits);
            last_quote=block+63-__builtin_clzll(bits);
            uint64_t rest=bits & (bits-1);
            quotes+=!rest ? 1 : !(rest & (rest-1)) ? 2 : 3;
        };
        auto field=[&](const char* stop) {
            SV value(start,stop-start);
            if (quotes) {
                if (quotes!=2 || first_quote!=start || last_quote!=stop-1) valid=false;
                else { value.remove_prefix(1); value.remove_suffix(1); }
            }
            if (count<N) fields[count]=value;
            ++count; start=stop; quotes=0;
        };
        auto finish=[&](const char* stop) {
            size_t length=stop-row;
            if (length>UINT32_MAX) throw std::runtime_error("CSV row exceeds 4 GiB");
            // shortcut: ordinary rows up to 256 bytes; irregular rows use the proven parser.
            if (!valid || count!=N || length>256) return ss::csv(SV(row,length),fields);
            return true;
        };
        for (;;) {
            if (!block || p==block+size) {
                if (p==end) { field(end); return finish(end); }
                block=p; size=std::min<size_t>(end-p,64); masks=scan(block,size);
            }
            while (masks.ends) {
                unsigned bit=__builtin_ctzll(masks.ends);
                uint64_t before=(uint64_t(1)<<bit)-1;
                add_quotes(masks.quotes & before);
                masks.quotes &= ~before;
                masks.ends &= masks.ends-1;
                const char* stop=block+bit;
                field(stop); p=stop+1;
                if (masks.newline & (uint64_t(1)<<bit)) return finish(stop);
                start=p;
            }
            add_quotes(masks.quotes); masks.quotes=0;
            p=block+size;
        }
    }
};

struct DateCache {
    struct Date { uint64_t head=0; uint16_t tail=0; int64_t midnight=0; };
    std::array<Date,256> dates{};
    bool parse(SV s,int64_t& out) {
        if (s.size()!=20 || s[19]!='Z' || s[4]!='-') return ss::timestamp(s,out);
        if (s[7]!='-' || s[10]!='T' || s[13]!=':' || s[16]!=':') return false;
        auto bytes=_mm_loadu_si128(reinterpret_cast<const __m128i*>(s.data()));
        auto valid=_mm_and_si128(_mm_cmpgt_epi8(bytes,_mm_set1_epi8('/')),
                                _mm_cmpgt_epi8(_mm_set1_epi8(':'),bytes));
        constexpr unsigned digits_mask=0xf | (3<<5) | (3<<8) | (3<<11) | (3<<14);
        if ((unsigned(_mm_movemask_epi8(valid))&digits_mask)!=digits_mask ||
            s[17]<'0' || s[17]>'9' || s[18]<'0' || s[18]>'9') return false;
        auto two=[&](size_t i) {
            uint16_t word; std::memcpy(&word,s.data()+i,2);
            return ((unsigned(word&0x0f0f)*2561)>>8)&255;
        };
        unsigned h=two(11),m=two(14),sec=two(17);
        if (h>23 || m>59 || sec>59) return false;
        int64_t tod=(h*3600+m*60+sec)*1000;
        uint64_t head; uint16_t tail;
        std::memcpy(&head,s.data(),8); std::memcpy(&tail,s.data()+8,2);
        auto& date=dates[ss::mix(head^tail)&255];
        if (date.head!=head || date.tail!=tail) {
            unsigned y=two(0)*100+two(2),month=two(5),day=two(8);
            constexpr unsigned months[]={31,28,31,30,31,30,31,31,30,31,30,31};
            if (!y || !month || month>12 || !day) return false;
            unsigned md=months[month-1]+(month==2 && y%4==0 && (y%100!=0 || y%400==0));
            if (day>md) return false;
            date={head,tail,ss::civil_days(y,month,day)*ss::DAY};
        }
        out=date.midnight+tod;
        return true;
    }
};
} // namespace sprint
