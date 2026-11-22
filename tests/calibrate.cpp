#define main shopping_cli_main
#include "../src/main.cpp"
#undef main
#include <bit>

int main(int argc,char** argv) {
    try {
        if (argc==2 && SV(argv[1])=="empty") return 0;
        if (argc==3 && SV(argv[1])=="scan") {
            size_t width=v5::vector_width();
            if (!width) throw std::runtime_error("scan calibration requires AVX2");
            fs::path root=argv[2];
            std::vector<fs::path> paths;
            for (const char* name:{"events","clicks","links"}) {
                auto part=files(root/name); paths.insert(paths.end(),part.begin(),part.end());
            }
            paths.push_back(root/"products.csv"); paths.push_back(root/"campaigns.csv");
            auto maps=map_files(paths); auto work=chunks(maps);
            uint64_t threads; if (!digits(setting("NCPU","8"),128,threads) || !threads) throw std::runtime_error("invalid NCPU");
            cpu_set_t cpus; CPU_ZERO(&cpus);
            if (!sched_getaffinity(0,sizeof(cpus),&cpus)) threads=std::min<uint64_t>(threads,CPU_COUNT(&cpus));
            std::vector<uint64_t> counts(threads); v5::Pool pool(threads);
            pool.run(work.size(),[&](size_t j,size_t t) {
                const char* p=work[j].begin; const char* end=work[j].end;
                for (;size_t(end-p)>=width;p+=width) counts[t]+=std::popcount(width==64 ? v5::mask512(p) : v5::mask256(p));
                while (p<end) { counts[t]+=*p==',' || *p=='\n' || *p=='"'; ++p; }
            });
            uint64_t bytes=0,total=0; for (const auto& m:maps) bytes+=m.size; for (auto c:counts) total+=c;
            std::cout<<"bytes="<<bytes<<" delimiters="<<total<<" simd="<<width*8<<"\n";
            return 0;
        }
        if (argc==4 && SV(argv[1])=="write") {
            fs::path expected=argv[2],output=argv[3];
            fs::create_directories(output/"user_daily"); fs::create_directories(output/"campaign_daily");
            Mapping users(expected/"user_daily.csv"),campaigns(expected/"campaign_daily.csv");
            std::vector<UserRow> rows;
            auto each=[&](const Mapping& map,auto fn) {
                auto nl=static_cast<const char*>(std::memchr(map.data,'\n',map.size));
                if (nl) lines({nl+1,map.data+map.size},fn);
            };
            each(users,[&](SV row) {
                std::array<SV,6> f; if (!csv(row,f)) throw std::runtime_error("invalid user answer CSV");
                int64_t day; std::string date=std::string(f[1])+"T00:00:00Z";
                if (!timestamp(date,day)) throw std::runtime_error("invalid DATE");
                uint64_t sessions,events,revenue;
                if (!digits(f[2],INT64_MAX,sessions) || !digits(f[3],INT64_MAX,events) || !digits(f[4],INT64_MAX,revenue)) throw std::runtime_error("invalid counts");
                rows.push_back({f[0],f[5],int32_t(day_of(day)),int64_t(sessions),int64_t(events),int64_t(revenue),!f[5].empty()});
            });
            write_users(output/"user_daily"/"part-0.parquet",rows);
            using C=pq::Column;
            std::vector<C> cols{{"campaign_id",C::STRING},{"channel",C::STRING},{"day",C::DATE},{"sessions",C::INT64},
                                {"purchasing_sessions",C::INT64},{"revenue_cents",C::INT64},{"assisted_sessions",C::INT64}};
            each(campaigns,[&](SV row) {
                std::array<SV,7> f; if (!csv(row,f)) throw std::runtime_error("invalid campaign answer CSV");
                cols[0].strings.emplace_back(f[0]); cols[1].strings.emplace_back(f[1]);
                int64_t day; std::string date=std::string(f[2])+"T00:00:00Z";
                if (!timestamp(date,day)) throw std::runtime_error("invalid DATE");
                cols[2].integers.push_back(day_of(day));
                for (size_t i=3;i<7;++i) { uint64_t count; if (!digits(f[i],INT64_MAX,count)) throw std::runtime_error("invalid counts"); cols[i].integers.push_back(count); }
            });
            pq::write((output/"campaign_daily"/"part-0.parquet").string(),cols);
            return 0;
        }
        throw std::runtime_error("usage: calibrate empty | scan DATA | write ANSWER_CSV_DIR OUTPUT");
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
