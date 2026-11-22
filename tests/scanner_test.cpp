#include "scanner.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <random>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace old {
__attribute__((target("avx2"))) uint64_t mask32(const char* p) {
    __m256i b=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
    return uint32_t(_mm256_movemask_epi8(_mm256_or_si256(_mm256_cmpeq_epi8(b,_mm256_set1_epi8(',')),
        _mm256_or_si256(_mm256_cmpeq_epi8(b,_mm256_set1_epi8('\n')),_mm256_cmpeq_epi8(b,_mm256_set1_epi8('"'))))));
}
__attribute__((target("avx512f,avx512bw"))) uint64_t mask64(const char* p) {
    __m512i b=_mm512_loadu_si512(p);
    return _mm512_cmpeq_epi8_mask(b,_mm512_set1_epi8(',')) |
        _mm512_cmpeq_epi8_mask(b,_mm512_set1_epi8('\n')) |
        _mm512_cmpeq_epi8_mask(b,_mm512_set1_epi8('"'));
}
class Scanner {
    const char *cursor,*end,*row,*block=nullptr;
    uint64_t mask=0;
    size_t width;
    uint64_t (*kernel)(const char*);
    const char* delimiter() {
        while (!mask) {
            if (cursor==end) return end;
            block=cursor;
            if (size_t(end-cursor)>=width) { mask=kernel(cursor);cursor+=width; }
            else {
                for (size_t i=0,n=end-cursor;i<n;++i)
                    if (cursor[i]==',' || cursor[i]=='\n' || cursor[i]=='"') mask|=uint64_t(1)<<i;
                cursor=end;
            }
        }
        unsigned bit=std::countr_zero(mask);mask&=mask-1;return block+bit;
    }
public:
    Scanner(const char* begin,const char* stop,size_t w):cursor(begin),end(stop),row(begin),width(w),kernel(w==64 ? mask64 : mask32) {}
    bool more() const { return row<end; }
    template<size_t N> bool next(std::array<ss::SV,N>& fields) {
        const char* start=row;
        size_t count=0,quotes=0;
        const char* first=nullptr;const char* last=nullptr;
        bool valid=true;
        for (;;) {
            const char* at=delimiter();
            if (at!=end && *at=='"') { if (!quotes)first=at;last=at;++quotes;continue; }
            ss::SV field(start,at-start);
            if (quotes) {
                if (quotes!=2 || first!=start || last!=at-1) valid=false;
                else {field.remove_prefix(1);field.remove_suffix(1);}
            }
            if (count<N) fields[count]=field;else valid=false;
            ++count;
            if (at==end || *at=='\n') {row=at==end ? end : at+1;return valid && count==N;}
            start=at+1;quotes=0;first=last=nullptr;
        }
    }
};
}

template<int WIDTH,size_t N> void check(const std::string& input) {
    v6::Scanner<WIDTH> scanner(input.data(),input.data()+input.size());
    const char* p=input.data();
    const char* end=p+input.size();
    while (p<end) {
        const char* nl=static_cast<const char*>(std::memchr(p,'\n',end-p));
        const char* stop=nl ? nl : end;
        std::array<ss::SV,N> expected{},actual{};
        bool valid=ss::csv(ss::SV(p,stop-p),expected);
        assert(scanner.more());
        assert(scanner.next(actual)==valid);
        if (valid) assert(actual==expected);
        p=nl ? nl+1 : end;
    }
    assert(!scanner.more());
}

template<int WIDTH> void tests() {
    std::mt19937_64 rng(350732);
    for (size_t trial=0;trial<2200;++trial) {
        std::string input;
        for (size_t row=0;row<30;++row) {
            for (size_t length=rng()%170;length;--length) {
                constexpr char chars[]="abc,\"\r \t012\x80\xff";
                input+=chars[rng()%(sizeof(chars)-1)];
            }
            if (row!=29 || trial%2) input+='\n';
        }
        check<WIDTH,7>(input);check<WIDTH,4>(input);check<WIDTH,2>(input);
    }
    for (size_t padding=0;padding<200;++padding) {
        std::string first(padding,'x');
        for (std::string input:{first+",\"\",c,4\n\"a\",b,c,d\n",
                               first+",\"\"\"\",c,4\n",first+",x\"x,c,4",first+",\"x\",c,4\r\n",
                               first+",x\",c,4\n",first+",x,c,\"ignored\"price\n",
                               "\""+first+"\",b,c,d",first+",\"\",c,\"\""}) check<WIDTH,4>(input);
    }
    size_t page=size_t(sysconf(_SC_PAGESIZE));
    char* memory=static_cast<char*>(mmap(nullptr,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    assert(memory!=MAP_FAILED);assert(mprotect(memory+page,page,PROT_NONE)==0);
    for (size_t padding=0;padding<256;++padding) {
        std::string row(padding,'x');row+=",\"\",c,4";
        char* begin=memory+page-row.size();std::memcpy(begin,row.data(),row.size());
        v6::Scanner<WIDTH> scanner(begin,memory+page);std::array<ss::SV,4> fields;
        assert(scanner.next(fields));assert(fields[0].size()==padding && fields[1].empty() && fields[3]=="4");
        assert(!scanner.more());
    }
    munmap(memory,page*2);
}

struct Mapping {
    const char* data=nullptr;size_t size=0;
    explicit Mapping(const std::filesystem::path& path) {
        int fd=open(path.c_str(),O_RDONLY);assert(fd>=0);struct stat s{};assert(!fstat(fd,&s));size=size_t(s.st_size);
        if (size) {data=static_cast<const char*>(mmap(nullptr,size,PROT_READ,MAP_PRIVATE,fd,0));assert(data!=MAP_FAILED);}
        close(fd);
    }
    Mapping(Mapping&& other) noexcept:data(other.data),size(other.size) {other.data=nullptr;}
    Mapping(const Mapping&)=delete;
    ~Mapping() {if(data)munmap(const_cast<char*>(data),size);}
};
struct Totals {uint64_t valid=0,rows=0,field_bytes=0;bool operator==(const Totals&) const=default;};
template<class Scanner> Totals count(Scanner& scanner) {
    Totals result;
    while (scanner.more()) {
        std::array<ss::SV,7> fields;++result.rows;
        if (scanner.next(fields)) {++result.valid;for(auto f:fields)result.field_bytes+=f.size();}
    }
    return result;
}
template<int WIDTH> Totals count_run(v6::Scanner<WIDTH>& scanner) {
    Totals result;
    scanner.template run<7>([&](bool valid,const std::array<ss::SV,7>& fields) {
        ++result.rows;if(valid){++result.valid;for(auto f:fields)result.field_bytes+=f.size();}
    });
    return result;
}

template<int WIDTH> void benchmark(const std::vector<Mapping>& maps) {
    Totals previous{};
    for (size_t iteration=0;iteration<3;++iteration) {
        for (int version=0;version<2;++version) {
            auto start=std::chrono::steady_clock::now();Totals result{};size_t bytes=0;
            for (const auto& map:maps) {
                auto nl=static_cast<const char*>(std::memchr(map.data,'\n',map.size));
                if (!nl)continue;
                Totals part;
                if (version==0) {old::Scanner scanner(nl+1,map.data+map.size,WIDTH);part=count(scanner);}
                else {v6::Scanner<WIDTH> scanner(nl+1,map.data+map.size);part=count_run(scanner);}
                result.valid+=part.valid;result.rows+=part.rows;result.field_bytes+=part.field_bytes;bytes+=map.size;
            }
            double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
            if (version==1)assert(result==previous);else previous=result;
            std::cout<<(version ? "new" : "old")<<" width="<<WIDTH<<" seconds="<<seconds<<" bytes="<<bytes
                     <<" rows="<<result.rows<<" valid="<<result.valid<<" field_bytes="<<result.field_bytes<<'\n';
        }
    }
}
int main(int argc,char** argv) {
    assert(__builtin_cpu_supports("avx2"));
    tests<32>();
    if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw")) tests<64>();
    std::cout<<"scanner tests passed (66000 random rows x 3 schemas, boundaries, protected tails per ISA)\n";
    if (argc==2) {
        std::vector<Mapping> maps;
        for (const auto& entry:std::filesystem::directory_iterator(argv[1]))
            if(entry.path().extension()==".csv")maps.emplace_back(entry.path());
        benchmark<32>(maps);
        if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw")) benchmark<64>(maps);
    }
}
