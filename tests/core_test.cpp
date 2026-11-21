#include "core.hpp"
#include <cassert>
#include <random>
#include <set>
#include <iostream>
using namespace ss;
int main() {
    std::array<SV,2> f;
    assert(csv("\" u1\",\"\"",f) && f[0]==" u1" && f[1].empty());
    assert(csv("u1,1000\r",f) && f[1]=="1000\r");
    for (SV row:{"","a","a,b,c","\"a,b","a\"b,c","\"a\"x,b"}) assert(!csv(row,f));
    uint64_t price; assert(digits("0000250",10000000,price) && price==250);
    for (SV value:{"","-1","1.0","10000001","٢٥٠","２５０","1000\r"}) assert(!digits(value,10000000,price));
    int64_t ts,a,b;
    assert(timestamp("2026-10-03T23:58:10Z",a));
    assert(timestamp("2026-10-03T19:58:10-04:00",b) && a==b);
    assert(timestamp("00000001791071890000",b) && a==b);
    assert(timestamp(std::string(12,'0')+"1791071890000",b) && a==b);
    assert(timestamp("2028-02-29T00:00:00-00:00",ts));
    for (SV value:{"2027-02-29T00:00:00Z","2028-02-30T00:00:00Z","2026-01-01T24:00:00Z",
                  "2026-01-01T00:00:60Z","2026-01-01T00:00:00+24:00","2026-01-01T00:00:00+05:60",
                  "2026-01-01t00:00:00Z","2026-01-01T00:00:00z","1791071890000.0","-5","2026-01-01T00:00:00Z\r"})
        assert(!timestamp(value,ts));
    assert(timestamp("86400000",ts) && ts==DAY);
    assert(!timestamp("99999999999999999999999999999999999",ts));
    assert(day_of(-1)==-1 && day_of(-DAY)==-1 && day_of(0)==0);
    assert(civil_days(1970,1,1)==0);
    // Differential check: interval union must match the independent naive session scan.
    std::mt19937_64 rng(1);
    for (int round=0;round<1000;++round) {
        std::vector<Session> sessions;
        for (int i=0,n=rng()%100+1;i<n;++i) sessions.push_back({int64_t(rng()%(14*DAY)),int64_t(rng()%2 ? 0 : rng()%1000+1)});
        std::sort(sessions.begin(),sessions.end(),[](auto a,auto b){return a.start<b.start;});
        std::vector<Click> clicks;
        for (int i=0,n=rng()%100;i<n;++i) clicks.push_back({"u","id",int64_t(rng()%(21*DAY))-WEEK,uint32_t(rng()%5+1)});
        std::sort(clicks.begin(),clicks.end(),[](auto a,auto b){return a.ts<b.ts;});
        std::vector<Stats> fast(6*14),slow(6*14); std::vector<size_t> covered(6);
        attribute(sessions,clicks.data(),clicks.size(),0,14,fast,covered);
        for (auto s:sessions) {
            const Click* best=nullptr; std::set<uint32_t> assists;
            for (auto& c:clicks) if (c.ts<=s.start && s.start-c.ts<=WEEK) {best=&c;assists.insert(c.campaign);}
            auto& v=slow[(best?best->campaign:0)*14+day_of(s.start)];
            ++v.sessions;v.purchasing+=s.revenue>0;v.revenue+=s.revenue;
            for (auto c:assists) ++slow[c*14+day_of(s.start)].assisted;
        }
        for (size_t i=0;i<fast.size();++i) {
            assert(fast[i].sessions==slow[i].sessions && fast[i].purchasing==slow[i].purchasing);
            assert(fast[i].revenue==slow[i].revenue && fast[i].assisted==slow[i].assisted);
        }
    }
    std::cout<<"core tests passed (1000 attribution differential cases)\n";
}
