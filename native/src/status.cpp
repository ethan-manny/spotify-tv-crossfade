#include "status.h"
#include <mutex>

namespace xfade {
namespace {
std::mutex g_mu;
int g_crossfadeMs = 0;
std::string g_state = "no-player";
uint32_t g_rate = 0, g_channels = 0;
StatsSource* g_stats = nullptr;
}  // namespace

void setCrossfadeMs(int ms) {
    if (ms < 0) ms = 0;
    if (ms > 12000) ms = 12000;
    std::lock_guard<std::mutex> lock(g_mu);
    g_crossfadeMs = ms;
}

int crossfadeMs() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_crossfadeMs;
}

void setPlayerInfo(const char* state, uint32_t rateHz, uint32_t channels) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_state = state;
    g_rate = rateHz;
    g_channels = channels;
}

void setStatsSource(StatsSource* source) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_stats = source;
}

// Two players can be live at once (the eSDK creates the new one before destroying the old), and they
// stop in whatever order it chooses; an unconditional clear would take the newer one's stats away.
void clearStatsSource(StatsSource* source) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_stats == source) g_stats = nullptr;
}

std::string statusLine() {
    std::lock_guard<std::mutex> lock(g_mu);
    std::string line = "state=" + g_state + " rate=" + std::to_string(g_rate) + " ch=" + std::to_string(g_channels) + " ";
    if (g_stats) return line + g_stats->stats();
    return line + "crossfade_ms=" + std::to_string(g_crossfadeMs);
}

}  // namespace xfade
