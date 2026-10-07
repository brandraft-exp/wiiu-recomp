#pragma once
#include <chrono>
#include <cstdio>
#include <cstdlib>
namespace gpu {
/* Opt-in cumulative wall timings; draw includes texture uploads. */
struct Profile {
    using Clock=std::chrono::steady_clock;
    inline static bool enabled=std::getenv("WIIU_TRACE_GPU")!=nullptr;
    inline static double milliseconds[4]{};
    inline static unsigned counts[4]{};
    inline static Clock::time_point epoch=Clock::now();
    unsigned stage;bool active;Clock::time_point start;
    explicit Profile(unsigned s):stage(s),active(enabled){if(active)start=Clock::now();}
    void finish(){if(active){milliseconds[stage]+=std::chrono::duration<double,std::milli>(Clock::now()-start).count();counts[stage]++;active=false;}}
    ~Profile(){finish();}
    static void report(unsigned frames){if(!enabled||frames%120)return;
        double elapsed=std::chrono::duration<double>(Clock::now()-epoch).count();
        std::fprintf(stderr,"GPU %u frames / %.2fs: translate %.1fms/%u draw %.1fms/%u upload %.1fms/%u present %.1fms/%u\n",frames,elapsed,
            milliseconds[0],counts[0],milliseconds[1],counts[1],milliseconds[2],counts[2],milliseconds[3],counts[3]);
    }
};
}
