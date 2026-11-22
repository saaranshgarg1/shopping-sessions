#define main shopping_cli_main
#include "../src/main.cpp"
#undef main
#include <cassert>
#include <iostream>
#include <random>
#include <set>

int main() {
    std::mt19937_64 rng(811219);
    sprint::Keys users('u');
    std::set<std::string> ids{"u", "u0", "u00", "u12345678", "u123456789", "é", std::string("u\0x",3)};
    for (size_t n=0;n<5000;++n) {
        std::string s(n%3 ? "u" : "other");
        for (size_t i=0,k=rng()%12;i<k;++i) s.push_back(char(rng()%256));
        ids.insert(std::move(s));
    }
    std::set<uint64_t> encoded;
    for (const auto& id:ids) {
        auto key=users.get(id);
        assert(encoded.insert(key).second);
        assert(users.text(key)==id);
        assert(users.get(id)==key);
    }
    for (size_t n=0;n<20000;++n) {
        auto a=ids.begin(),b=ids.begin();
        std::advance(a,rng()%ids.size()); std::advance(b,rng()%ids.size());
        assert(users.less(users.get(*a),users.get(*b))==(*a<*b));
    }
    sprint::DateCache cache;
    for (SV value:{"2028-02-29T23:59:59Z", "2028-02-29T23:59:59+23:59",
                  "2028-02-29T00:00:00-23:59", "2028-02-29T23:59:59-00:00",
                  "2028-02-29T23:59:59+00:00", "2028-02-29T23:59:59Z",
                  "2027-02-29T00:00:00Z", "00000001791071890000", "9223372036854775807"}) {
        int64_t fast=0,slow=0;
        bool ok=timestamp(value,slow);
        assert(cache.parse(value,fast)==ok && (!ok || fast==slow));
    }
    auto check_time=[&](SV value) {
        int64_t fast=0,slow=0; bool ok=timestamp(value,slow);
        assert(cache.parse(value,fast)==ok && (!ok || fast==slow));
    };
    for (SV value:{"0000000000000","1791071890000","9999999999999"}) check_time(value);
    for (size_t position=0;position<13;++position) for (unsigned byte=0;byte<256;++byte) {
        std::string value="1791071890000"; value[position]=char(byte); check_time(value);
    }
    for (size_t n=0;n<100000;++n) {
        std::string value=std::to_string(rng()%10000000000000ULL);
        value.insert(0,13-value.size(),'0'); check_time(value);
        if (n%3==0) for (char& c:value) c=char(rng()%256);
        else value[rng()%value.size()]=char(rng()%256);
        check_time(value);
    }
    for (size_t width:{20,25,70}) {
        std::string value(width-13,'0'); value+="1791071890000"; check_time(value);
    }
    for (SV value:{"2028-02-29T00:00:00Z","2028-02-29T23:59:59Z","2028-02-29T23:59:59+23:59",
                   "2028-02-29T23:59:59-23:59","2028-02-29T23:59:59+24:00","2028-02-29T23:59:59+05:60",
                   "2028-02-29T24:00:00Z","2028-02-29T23:59:60Z","2028-02-29T23:59:59z","0000-02-29T00:00:00Z"}) check_time(value);
    size_t page=size_t(sysconf(_SC_PAGESIZE));
    char* memory=static_cast<char*>(mmap(nullptr,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    assert(memory!=MAP_FAILED && mprotect(memory+page,page,PROT_NONE)==0);
    std::memcpy(memory+page-13,"1791071890000",13); check_time(SV(memory+page-13,13));
    munmap(memory,page*2);
    for (size_t n=0;n<100000;++n) {
        std::string value(n%2 ? "2028-02-29T23:59:59Z" : "2028-02-29T23:59:59+05:30");
        if (n%3) value[rng()%value.size()]=char(rng()%256);
        int64_t fast=0,slow=0; bool ok=timestamp(value,slow);
        assert(cache.parse(value,fast)==ok && (!ok || fast==slow));
    }
    for (size_t n=0;n<30000;++n) {
        std::string line;
        for (size_t i=0,k=rng()%100;i<k;++i) {
            constexpr char alphabet[]="u0,x\" \r\t";
            line.push_back(alphabet[rng()%(sizeof(alphabet)-1)]);
        }
        size_t size=line.size();
        if (n%2) line.push_back('\n');
        std::array<SV,4> slow{},fast{}; bool ok=csv(SV(line.data(),size),slow);
        const char* p=line.data();
        assert(sprint::csv_next(p,line.data()+line.size(),fast)==ok);
        assert(p==line.data()+line.size());
        if (ok) assert(fast==slow);
    }
    for (size_t size=0;size<64;++size) {
        std::string first(size,'x'); first+=",\"\",z,4";
        std::string input=first+"\ny,2,3,4\n";
        const char* p=input.data(); std::array<SV,4> fields{};
        assert(sprint::csv_next(p,input.data()+input.size(),fields));
        assert(fields[0]==SV(first.data(),size) && fields[1].empty());
        assert(sprint::csv_next(p,input.data()+input.size(),fields));
        assert(fields[0]=="y" && p==input.data()+input.size());
    }
    std::vector<sprint::PersonEvent> sort_scratch;
    for (size_t count:{0,1,2,1023,1024,4097,50000}) {
        std::vector<sprint::PersonEvent> rows;
        for (size_t i=0;i<count;++i) rows.push_back({int64_t(rng()),uint32_t(i),uint32_t(i)});
        auto expected=rows;
        std::sort(expected.begin(),expected.end(),[](const auto& a,const auto& b) { return a.ts<b.ts; });
        sprint::sort_events(rows.data(),rows.size(),sort_scratch);
        for (size_t i=0;i<count;++i) assert(rows[i].ts==expected[i].ts && rows[i].category==expected[i].category);
        for (auto& row:rows) row.ts=1;
        sprint::sort_events(rows.data(),rows.size(),sort_scratch);
        for (const auto& row:rows) assert(row.ts==1);
    }
    // Exact collisions, invalid earliest winner, and unsupported ties must survive copy-free dedup.
    std::array<Partition<sprint::RawEvent>,PARTS> raw_events;
    std::array<Partition<sprint::RawClick>,PARTS> raw_clicks;
    std::array<Partition<sprint::Event>,PARTS> events;
    std::array<Partition<sprint::Click>,PARTS> clicks;
    raw_events[0].add({{1,2,10,20,0,100},{1,3,11,19,0,sprint::INVALID},{2,2,10,20,0,100}});
    raw_events[0].add({{1,4,12,21,0,200},{2,2,10,20,0,100}});
    sprint::dedup<true>(raw_events,raw_clicks,2,events,clicks);
    auto rows=events[mix(2)&(PARTS-1)].flatten();
    assert(rows.size()==1 && rows[0].user==2 && rows[0].price==100);
    assert(raw_events[0].blocks.empty());
    raw_events[0].add({{1,2,10,20,0,100},{1,3,10,20,0,100}});
    bool unsupported=false;
    try { sprint::dedup<true>(raw_events,raw_clicks,2,events,clicks); }
    catch (const sprint::Unsupported&) { unsupported=true; }
    assert(unsupported);
    // Worker completion order must not select a later physical copy in fallback.
    Identity identity; Catalog catalog;
    std::array<RawPart,PARTS> raw;
    std::array<Partition<ss::Event>,PARTS> base_events;
    std::array<Partition<ss::Click>,PARTS> base_clicks;
    std::string first="same,uA,1,2,view,p0,0",second="same,uB,1,2,view,p0,0";
    uint32_t fingerprint=hash("same");
    Raw later{}; later.line=second.data(); later.length=second.size(); later.fingerprint=fingerprint;
    later.ingest=2; later.ts=1; later.user="uB";
    Raw earlier=later; earlier.line=first.data(); earlier.length=first.size(); earlier.user="uA";
    raw[0].blocks.emplace_back(1,std::vector<Raw>{later});
    raw[0].blocks.emplace_back(0,std::vector<Raw>{earlier});
    ::dedup<true>(raw,2,base_events,base_clicks);
    auto base_rows=base_events[hash("uA")&(PARTS-1)].flatten();
    assert(base_rows.size()==1 && base_rows[0].user=="uA");
    std::cout<<"fast tests passed (exact IDs, 100000 timestamp mutations, 30000 CSV rows, dedup)\n";
}
