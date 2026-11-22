#pragma once
#include "core.hpp"
#include <bit>
#include <immintrin.h>

namespace v6 {
namespace detail {
struct Masks { uint64_t separators, quotes; };
__attribute__((always_inline, target("avx2"))) inline Masks masks32(const char* p) {
    const __m256i bytes=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
    const __m256i separators=_mm256_or_si256(_mm256_cmpeq_epi8(bytes,_mm256_set1_epi8(',')),
                                             _mm256_cmpeq_epi8(bytes,_mm256_set1_epi8('\n')));
    return {uint32_t(_mm256_movemask_epi8(separators)),
            uint32_t(_mm256_movemask_epi8(_mm256_cmpeq_epi8(bytes,_mm256_set1_epi8('"'))))};
}
__attribute__((always_inline, target("avx512f,avx512bw"))) inline Masks masks64(const char* p) {
    const __m512i bytes=_mm512_loadu_si512(p);
    return {_mm512_cmpeq_epi8_mask(bytes,_mm512_set1_epi8(',')) |
            _mm512_cmpeq_epi8_mask(bytes,_mm512_set1_epi8('\n')),
            _mm512_cmpeq_epi8_mask(bytes,_mm512_set1_epi8('"'))};
}
}

template<int WIDTH> class Scanner;

// The target-specific row loop keeps vector loads inlined; quotes are counted by mask, not visited byte by byte.
#define V6_SCANNER(WIDTH, ISA, MASKS) \
template<> class Scanner<WIDTH> { \
    const char* cursor; \
    const char* end; \
    const char* row; \
    const char* block=nullptr; \
    uint64_t separators=0, quotes=0; \
public: \
    Scanner(const char* begin,const char* stop):cursor(begin),end(stop),row(begin) {} \
    bool more() const { return row<end; } \
    template<size_t N> __attribute__((target(ISA))) bool next(std::array<ss::SV,N>& fields) { \
        const char* start=row; \
        const char* first_quote=nullptr; \
        const char* last_quote=nullptr; \
        size_t count=0, quote_count=0; \
        bool valid=true; \
        auto take_quotes=[&](uint64_t taken) { \
            if (!taken) return; \
            if (!quote_count) first_quote=block+std::countr_zero(taken); \
            last_quote=block+63-std::countl_zero(taken); \
            quote_count+=std::popcount(taken); \
        }; \
        for (;;) { \
            while (!separators && cursor<end) { \
                take_quotes(quotes); \
                block=cursor; \
                if (size_t(end-cursor)>=WIDTH) { \
                    const auto masks=detail::MASKS(cursor); \
                    separators=masks.separators; quotes=masks.quotes; cursor+=WIDTH; \
                } else { \
                    quotes=0; \
                    for (size_t i=0,n=end-cursor;i<n;++i) { \
                        if (cursor[i]==',' || cursor[i]=='\n') separators|=uint64_t(1)<<i; \
                        else if (cursor[i]=='"') quotes|=uint64_t(1)<<i; \
                    } \
                    cursor=end; \
                } \
            } \
            const char* at=end; \
            if (separators) { \
                const unsigned bit=std::countr_zero(separators); \
                separators&=separators-1; \
                const uint64_t before=(uint64_t(1)<<bit)-1; \
                take_quotes(quotes&before); quotes&=~before; \
                at=block+bit; \
            } else { take_quotes(quotes); quotes=0; } \
            ss::SV field(start,size_t(at-start)); \
            if (quote_count) { \
                if (quote_count!=2 || first_quote!=start || last_quote!=at-1) valid=false; \
                else { field.remove_prefix(1); field.remove_suffix(1); } \
            } \
            if (count<N) fields[count]=field; else valid=false; \
            ++count; \
            if (at==end || *at=='\n') { row=at==end ? end : at+1; return valid && count==N; } \
            start=at+1; quote_count=0; first_quote=last_quote=nullptr; \
        } \
    } \
    template<size_t N,class F> __attribute__((target(ISA), flatten)) void run(F&& fn) { \
        while (more()) { std::array<ss::SV,N> fields; bool valid=next(fields); fn(valid,fields); } \
    } \
};

V6_SCANNER(32, "avx2", masks32)
V6_SCANNER(64, "avx512f,avx512bw", masks64)
#undef V6_SCANNER
} // namespace v6
