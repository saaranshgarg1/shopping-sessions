#include "decode.hpp"
#include <cassert>
#include <iostream>
#include <random>
#include <sys/mman.h>
#include <unistd.h>
using namespace sprint;
std::mt19937_64 rng(937813);
size_t scanned=0;
Masks counted_scan(const char* p,size_t n) { scanned+=n; return scan_sse2(p,n); }

template<size_t N> void csv_checks(Scan scan) {
    for (unsigned round=0;round<1000;++round) {
        std::string text;
        std::vector<std::string> rows;
        for (unsigned j=0;j<1+rng()%20;++j) {
            std::string row;
            size_t count=rng()%5 ? N : rng()%(N+3)+1;
            for (size_t f=0;f<count;++f) {
                if (f) row+=',';
                bool quoted=rng()%2;
                if (quoted) row+='"';
                for (size_t k=0,n=rng()%8 ? rng()%30 : 500;k<n;++k) {
                    char c=char(rng()%256);
                    if (c==',' || c=='\n' || c=='"') c='x';
                    row+=c;
                }
                if (quoted) row+='"';
            }
            if (rng()%4==0) row.insert(rng()%(row.size()+1),1,'"');
            if (rng()%4==0) row+='\r';
            rows.push_back(row); text+=row;
            if (j || rng()%2) text+='\n';
            else break;
        }
        CsvReader<N> reader(text.data(),text.data()+text.size(),scan);
        for (const auto& row:rows) {
            assert(reader.p<reader.end);
            std::array<SV,N> expected,actual;
            bool valid=ss::csv(SV(row),expected);
            assert(reader.next(actual)==valid);
            if (valid) assert(expected==actual);
        }
        assert(reader.p==reader.end);
    }
    for (SV row:{"", "a", "a,b,c", "\"a,b\",c", "\"a\"\"b\",c", "a\"b,c", "\"\",x", "x,y\r", "\"x\",\"y\"\r"}) {
        std::string text(row); text+='\n';
        CsvReader<N> reader(text.data(),text.data()+text.size(),scan);
        std::array<SV,N> expected,actual;
        bool valid=ss::csv(row,expected);
        assert(reader.next(actual)==valid);
        if (valid) assert(expected==actual);
        assert(reader.p==reader.end);
    }
}
void date_checks() {
    DateCache cache;
    auto compare=[&](SV value) {
        int64_t expected=0,actual=0;
        bool valid=ss::timestamp(value,expected);
        assert(cache.parse(value,actual)==valid);
        if (valid) assert(expected==actual);
    };
    for (unsigned i=0;i<10000;++i) {
        char text[40];
        unsigned y=rng()%10000,m=rng()%15,d=rng()%34,h=rng()%26,minute=rng()%63,s=rng()%63;
        std::snprintf(text,sizeof(text),"%04u-%02u-%02uT%02u:%02u:%02uZ",y,m,d,h,minute,s);
        compare(text); compare(text);
        std::string value=text;
        value[rng()%20]=char(rng()%256); compare(value);
        value=text; value.resize(19); value+="+05:30"; compare(value);
        compare(std::to_string(rng()>>1));
    }
    for (SV value:{"2028-02-29T00:00:00Z", "1900-02-29T00:00:00Z", "0000-01-01T00:00:00Z",
                  "2026-01-01T23:59:59Z", "2026-01-01T24:00:00Z", "2026-01-01T00:00:60Z",
                  "2026-01-01T00:00:00-23:59", "2026-01-01T00:00:00+24:00", "2026-01-01T00:00:00Z\r",
                  "00000001791071890000", "0000000000001791071890000", "9223372036854775807", "9223372036854775808"})
        compare(value);
    for (unsigned position=0;position<20;++position) for (unsigned byte=0;byte<256;++byte) {
        std::string value="2028-02-29T23:59:59Z";
        value[position]=char(byte); compare(value);
    }
}
int main() {
    assert(choose_scan(false,false,"auto")==scan_sse2);
    assert(choose_scan(false,false,"avx512")==scan_sse2);
    assert(choose_scan(true,false,"avx512")==scan_avx2);
    assert(choose_scan(true,true,"auto")==scan_avx512);
    assert(choose_scan(true,true,"sse2")==scan_sse2);
    assert(choose_scan(true,true,"avx2")==scan_avx2);
    bool rejected=false; try { choose_scan(true,true,"bad"); } catch (const std::runtime_error&) { rejected=true; }
    assert(rejected);
    std::vector<Scan> scans{scan_sse2};
    if (__builtin_cpu_supports("avx2")) scans.push_back(scan_avx2);
    if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw")) scans.push_back(scan_avx512);
    size_t page=sysconf(_SC_PAGESIZE);
    char* memory=static_cast<char*>(mmap(nullptr,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    assert(memory!=MAP_FAILED && mprotect(memory+page,page,PROT_READ|PROT_WRITE)==0);
    for (auto scan:scans) {
        for (size_t n=0;n<=64;++n) for (unsigned round=0;round<100;++round) {
            char* p=memory+2*page-n;
            Masks expected{};
            for (size_t i=0;i<n;++i) {
                p[i]=rng()%2 ? char(rng()%256) : ",\n\""[rng()%3];
                expected.ends |= uint64_t(p[i]==',' || p[i]=='\n')<<i;
                expected.newline |= uint64_t(p[i]=='\n')<<i;
                expected.quotes |= uint64_t(p[i]=='\"')<<i;
            }
            auto actual=scan(p,n);
            assert(actual.ends==expected.ends && actual.newline==expected.newline && actual.quotes==expected.quotes);
        }
        csv_checks<2>(scan); csv_checks<4>(scan); csv_checks<7>(scan);
    }
    munmap(memory,page*3);
    std::string text="a,b\nx,y\n"; scanned=0;
    CsvReader<2> reader(text.data(),text.data()+text.size(),counted_scan);
    std::array<SV,2> fields;
    assert(reader.next(fields) && reader.next(fields) && scanned==text.size());
    date_checks();
    std::cout<<"decode tests passed: "<<scans.size()<<" CPU-gated paths, guard-page tails, randomized CSV/date differential\n";
}
