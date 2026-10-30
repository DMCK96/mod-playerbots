/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_HUMANMOVEMENTTARGETS_H
#define PLAYERBOTS_HUMANMOVEMENTTARGETS_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

// Target identity and the short memory of visited targets. Split from HumanMovementRules.h because NewRpgInfo.h,
// and through it most of the module, holds a RecentTargets; the rules themselves can then change without
// rebuilding everything.
namespace HumanMovementRules
{
using TargetKey = uint64_t;
constexpr TargetKey NoTarget = 0;

constexpr size_t RecentSize = 4;

class RecentTargets
{
public:
    void Push(TargetKey key)
    {
        if (key == NoTarget)
            return;
        auto const end = keys.begin() + used;
        auto const found = std::find(keys.begin(), end, key);
        if (found != end)
        {
            std::rotate(found, found + 1, end);
            keys[used - 1] = key;
            return;
        }
        if (used < RecentSize)
        {
            keys[used++] = key;
            return;
        }
        std::rotate(keys.begin(), keys.begin() + 1, keys.end());
        keys[RecentSize - 1] = key;
    }

    bool Contains(TargetKey key) const
    {
        return key != NoTarget && std::find(keys.begin(), keys.begin() + used, key) != keys.begin() + used;
    }

    void Clear() { used = 0; }

private:
    std::array<TargetKey, RecentSize> keys{};
    size_t used{0};
};
}  // namespace HumanMovementRules

#endif
