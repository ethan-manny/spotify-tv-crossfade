#pragma once
#include <cstdint>
#include <string>

namespace xfade {

// Process-wide status shared between the audio front end and JNI.
void setCrossfadeMs(int ms);            // clamped to 0..12000; the global setting a new player starts from
int crossfadeMs();
void setPlayerInfo(const char* state, uint32_t rateHz, uint32_t channels);  // state: "no-player" | "passthrough" | "active"

// A live pipeline registers itself to add its counters to statusLine(). stats() is called with the status
// lock held and must not call back into this file.
class StatsSource {
public:
    virtual ~StatsSource() = default;
    virtual std::string stats() = 0;   // "lead_ms=N crossfade_ms=N fades=N last_delta_ms=+N underruns=N"
};
void setStatsSource(StatsSource* source);     // register; blocks while a statusLine() call is using it
void clearStatsSource(StatsSource* source);   // unregister, but only if 'source' is the registered one:
                                              // a pipeline that is stopping must not silence a newer one

std::string statusLine();               // "state=active rate=44100 ch=2 lead_ms=6120 crossfade_ms=5000 fades=3 last_delta_ms=+40 underruns=0"

}  // namespace xfade
