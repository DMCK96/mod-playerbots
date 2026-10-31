/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NAVTELEMETRYRULES_H
#define PLAYERBOTS_NAVTELEMETRYRULES_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

// What the worldserver says about itself in Playerbots.log so the navigation collector can tell builds apart: the
// identity read from a build-info file, a fingerprint of the behaviour settings and the line that carries both.
// Kept free of core headers so the unit tests can pin the formats.
namespace NavTelemetryRules
{
struct BuildInfo
{
    std::string sha;
    std::string subject;
};

constexpr std::size_t MaxSubjectBytes = 120;

// Options that only change what the server logs, where it finds this identity or how bots are named and announced:
// editing them is not a new behaviour, so a key containing one of these words stays out of the fingerprint.
constexpr std::array<char const*, 8> NonBehaviourWords = {"Metrics",      "BuildInfo", "Telemetry", "LogTargets",
                                                          "StatusFile",   "StatusInterval", "Surname", "Announce"};

inline bool ChangesBehaviour(std::string const& key)
{
    return std::none_of(NonBehaviourWords.begin(), NonBehaviourWords.end(),
                        [&key](char const* word) { return key.find(word) != std::string::npos; });
}

// FNV-1a 64 over the sorted "key=value\n" lines of the options that change behaviour.
inline std::string Fingerprint(std::vector<std::pair<std::string, std::string>> options)
{
    options.erase(std::remove_if(options.begin(), options.end(),
                                 [](auto const& option) { return !ChangesBehaviour(option.first); }),
                  options.end());
    std::sort(options.begin(), options.end(), [](auto const& a, auto const& b) { return a.first < b.first; });

    uint64_t hash = 14695981039346656037ull;
    auto feed = [&hash](std::string const& text)
    {
        for (unsigned char c : text)
        {
            hash ^= c;
            hash *= 1099511628211ull;
        }
    };
    for (auto const& [key, value] : options)
    {
        feed(key);
        feed("=");
        feed(value);
        feed("\n");
    }

    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

inline bool IsGitSha(std::string const& text)
{
    if (text.size() < 7 || text.size() > 40)
        return false;
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

inline std::string Trim(std::string text)
{
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.pop_back();
    std::size_t start = 0;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t'))
        ++start;
    return text.substr(start);
}

inline BuildInfo ParseBuildInfo(std::string const& text)
{
    BuildInfo info{"unknown", ""};
    bool subjectSeen = false;
    std::size_t position = 0;
    while (position <= text.size())
    {
        std::size_t end = text.find('\n', position);
        if (end == std::string::npos)
            end = text.size();
        std::string const line = text.substr(position, end - position);
        position = end + 1;

        if (line.rfind("sha=", 0) == 0)
        {
            std::string const sha = Trim(line.substr(4));
            if (IsGitSha(sha))
                info.sha = sha;
        }
        else if (!subjectSeen && line.rfind("subject=", 0) == 0)
        {
            info.subject = Trim(line.substr(8));
            subjectSeen = true;
        }
    }
    return info;
}

inline std::string CleanSubject(std::string const& subject)
{
    std::string out;
    for (unsigned char c : subject)
    {
        char next = static_cast<char>(c);
        if (c == '"')
            next = '\'';
        else if (c < 0x20 || c == 0x7f)
            next = ' ';
        if (next == ' ' && (out.empty() || out.back() == ' '))
            continue;
        out.push_back(next);
    }
    if (out.size() > MaxSubjectBytes)
    {
        std::size_t cut = MaxSubjectBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
            --cut;
        out.resize(cut);
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

inline std::string FormatBuildLine(BuildInfo const& info, std::string const& fingerprint)
{
    return "coa build sha=" + info.sha + " cfg=" + fingerprint + " subject=\"" + CleanSubject(info.subject) + "\"";
}
}  // namespace NavTelemetryRules

#endif
