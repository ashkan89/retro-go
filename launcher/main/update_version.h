#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Accept numeric release versions (including v4.5); retain a suffix for local
// git-describe builds. Compare components numerically, never lexicographically.
static bool update_parse_version(const char *text, uint32_t numbers[3], const char **suffix)
{
    if (!text) return false;
    if (*text == 'v' || *text == 'V') ++text;
    memset(numbers, 0, 3 * sizeof(*numbers));
    int count = 0;
    do
    {
        if (count == 3 || *text < '0' || *text > '9') return false;
        uint32_t value = 0;
        while (*text >= '0' && *text <= '9')
        {
            unsigned digit = *text++ - '0';
            if (value > (UINT32_MAX - digit) / 10) return false;
            value = value * 10 + digit;
        }
        numbers[count++] = value;
        if (*text != '.') break;
        ++text;
    } while (true);
    if (count < 2 || (*text && *text != '-' && *text != '+')) return false;
    *suffix = text;
    return true;
}

static bool update_version_is_newer(const char *release, const char *installed)
{
    uint32_t next[3], current[3];
    const char *next_suffix, *current_suffix;
    if (!update_parse_version(release, next, &next_suffix) || *next_suffix ||
        !update_parse_version(installed, current, &current_suffix)) return false;
    for (int i = 0; i < 3; ++i)
        if (next[i] != current[i]) return next[i] > current[i];
    // Same numeric version: stable supersedes a prerelease, but not a local
    // git-describe build (4.5-3-gabc or 4.5-dirty).
    if (*current_suffix != '-') return false;
    ++current_suffix;
    return *current_suffix && (*current_suffix < '0' || *current_suffix > '9') &&
           strcmp(current_suffix, "dirty") != 0;
}

static bool update_image_asset_matches(const char *name, const char *version, const char *target)
{
    if (!name || !version || !target || strncmp(name, "retro-go_", 9) != 0) return false;
    name += 9;
    size_t length = strlen(version);
    if (strncmp(name, version, length) != 0 || name[length] != '_') return false;
    name += length + 1;
    length = strlen(target);
    for (size_t i = 0; i < length; ++i)
    {
        char actual = name[i], expected = target[i];
        if (actual >= 'A' && actual <= 'Z') actual += 'a' - 'A';
        if (expected >= 'A' && expected <= 'Z') expected += 'a' - 'A';
        if (actual != expected) return false;
    }
    return strcmp(name + length, ".img") == 0;
}
