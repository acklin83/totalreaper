// ChannelMap.cpp — reaper.ini parser for [alias_in_*] channel mappings.

#include "ChannelMap.h"

#include "reaper_plugin_functions.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace totalreaper::reaper {

namespace {

std::unordered_map<int, int> g_inputMap;  // REAPER input channel  → hardware
std::unordered_map<int, int> g_outputMap; // REAPER output channel → hardware

// Returns true if the line is a section header beginning with prefix.
// Out-args set to the section name (between brackets) when matched.
bool isSectionHeader(const char* line, const char* prefix) {
    if (line[0] != '[') return false;
    const std::size_t prefixLen = std::strlen(prefix);
    return std::strncmp(line + 1, prefix, prefixLen) == 0;
}

// Parse a "chN=M" line. On match, sets *outReaper = N, *outHardware = M.
bool parseChannelMapping(const char* line, int* outReaper, int* outHardware) {
    if (line[0] != 'c' || line[1] != 'h') return false;
    const char* eq = std::strchr(line, '=');
    if (eq == nullptr) return false;

    char* end = nullptr;
    const long n = std::strtol(line + 2, &end, 10);
    if (end != eq) return false; // garbage between "ch" digits and "="

    const long m = std::strtol(eq + 1, &end, 10);
    if (end == eq + 1) return false; // empty value

    *outReaper = static_cast<int>(n);
    *outHardware = static_cast<int>(m);
    return true;
}

// Strip trailing newline / carriage return / whitespace.
void rstrip(char* line) {
    std::size_t len = std::strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                       line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }
}

} // namespace

void loadChannelMap() {
    g_inputMap.clear();
    g_outputMap.clear();

    const char* iniPath = get_ini_file();
    if (iniPath == nullptr) return;

    FILE* f = std::fopen(iniPath, "r");
    if (f == nullptr) return;

    enum SectionKind { Other, InputAlias, OutputAlias };
    SectionKind section = Other;

    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        rstrip(line);

        if (line[0] == '[') {
            if (isSectionHeader(line, "alias_in_"))       section = InputAlias;
            else if (isSectionHeader(line, "alias_out_")) section = OutputAlias;
            else                                          section = Other;
            // Mappings from multiple device sections merge last-wins. The
            // active device's section overwrites any stale entries.
            continue;
        }

        if (section == Other) continue;

        int reaperCh = 0, hwCh = 0;
        if (parseChannelMapping(line, &reaperCh, &hwCh)) {
            (section == InputAlias ? g_inputMap : g_outputMap)[reaperCh] = hwCh;
        }
    }
    std::fclose(f);
}

int reaperInputToHardware(int reaperChannel) {
    auto it = g_inputMap.find(reaperChannel);
    return it != g_inputMap.end() ? it->second : reaperChannel;
}

int reaperOutputToHardware(int reaperChannel) {
    auto it = g_outputMap.find(reaperChannel);
    return it != g_outputMap.end() ? it->second : reaperChannel;
}

} // namespace totalreaper::reaper
