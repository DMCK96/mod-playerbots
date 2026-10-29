/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_HUMANMOVEMENTRULES_H
#define PLAYERBOTS_HUMANMOVEMENTRULES_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <unordered_map>
#include <vector>

// Decisions behind ambient bot movement that need no game state: how attractive a target is, which one to pick,
// which targets a bot saw last and when a long run is cut for a pause. Kept free of core headers so the unit
// tests can pin them.
namespace HumanMovementRules
{
enum class TargetKind : uint8_t
{
    Hub,     // auction house, bank, inn, flight master, battlemaster: crowds are welcome
    Spread,  // every other RPG NPC: one bot at a time reads best
    Place    // grind areas and quest objectives
};

using TargetKey = uint64_t;
constexpr TargetKey NoTarget = 0;

constexpr uint32_t ClaimTtlMs = 180000;
constexpr size_t RecentSize = 4;
constexpr float RecencyFactor = 0.05f;
constexpr size_t MaxLiveScans = 8;

inline TargetKey NpcKey(uint32_t spawnLow)
{
    return (TargetKey(1) << 56) | spawnLow;
}

inline TargetKey PlaceKey(int32_t cellX, int32_t cellY)
{
    constexpr uint64_t mask = (uint64_t(1) << 28) - 1;
    return (TargetKey(2) << 56) | ((uint64_t(uint32_t(cellX)) & mask) << 28) | (uint64_t(uint32_t(cellY)) & mask);
}

inline float DistanceFactor(float yards)
{
    return 1.0f / (1.0f + std::max(yards, 0.0f) / 60.0f);
}

inline float OccupancyFactor(TargetKind kind, uint32_t occupancy)
{
    float const n = float(occupancy);
    if (kind == TargetKind::Hub)
        return (1.0f + 0.15f * std::min(n, 4.0f)) / (1.0f + 0.25f * std::max(0.0f, n - 12.0f));
    return 1.0f / ((1.0f + n) * (1.0f + n));
}

inline float TargetWeight(TargetKind kind, uint32_t occupancy, bool recent, float distance)
{
    return DistanceFactor(distance) * OccupancyFactor(kind, occupancy) * (recent ? RecencyFactor : 1.0f);
}

// A bot standing at its target is both claimed and seen there; count it once.
inline uint32_t Occupancy(uint32_t intent, uint32_t live)
{
    return std::max(intent, live);
}

// Weighted draw with `u` in [0, 1]; -1 when nothing has a positive weight.
inline int32_t PickWeighted(std::vector<float> const& weights, float u)
{
    float total = 0.0f;
    int32_t lastPositive = -1;
    for (size_t i = 0; i < weights.size(); ++i)
    {
        if (weights[i] > 0.0f)
        {
            total += weights[i];
            lastPositive = int32_t(i);
        }
    }
    if (!(total > 0.0f))
        return -1;

    float const target = std::clamp(u, 0.0f, 1.0f) * total;
    float running = 0.0f;
    for (size_t i = 0; i < weights.size(); ++i)
    {
        if (weights[i] <= 0.0f)
            continue;
        running += weights[i];
        if (running > target)
            return int32_t(i);
    }
    return lastPositive;
}

inline std::vector<size_t> TopIndices(std::vector<float> const& weights, size_t count)
{
    std::vector<size_t> order(weights.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(), [&weights](size_t a, size_t b) { return weights[a] > weights[b]; });
    if (order.size() > count)
        order.resize(count);
    return order;
}

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

// Which target each bot is heading to or standing at. A claim that is not refreshed for ClaimTtlMs no longer
// counts, so bots that log out or teleport without releasing stop blocking their target.
class IntentTable
{
public:
    void Claim(uint32_t bot, TargetKey key, uint32_t nowMs)
    {
        Release(bot);
        if (key == NoTarget)
            return;
        claims[bot] = {key, nowMs};
        byTarget[key].push_back(bot);
    }

    void Release(uint32_t bot)
    {
        auto const claim = claims.find(bot);
        if (claim == claims.end())
            return;
        Unlink(claim->second.key, bot);
        claims.erase(claim);
    }

    void Refresh(uint32_t bot, uint32_t nowMs)
    {
        auto const claim = claims.find(bot);
        if (claim != claims.end())
            claim->second.stamp = nowMs;
    }

    uint32_t Count(TargetKey key, uint32_t nowMs)
    {
        auto const target = byTarget.find(key);
        if (target == byTarget.end())
            return 0;
        std::vector<uint32_t> expired;
        for (uint32_t bot : target->second)
            if (Expired(claims.at(bot), nowMs))
                expired.push_back(bot);
        for (uint32_t bot : expired)
            Release(bot);
        auto const remaining = byTarget.find(key);
        return remaining == byTarget.end() ? 0 : uint32_t(remaining->second.size());
    }

    TargetKey ClaimOf(uint32_t bot, uint32_t nowMs) const
    {
        auto const claim = claims.find(bot);
        if (claim == claims.end() || Expired(claim->second, nowMs))
            return NoTarget;
        return claim->second.key;
    }

    bool Empty() const { return claims.empty(); }

    void Prune(uint32_t nowMs)
    {
        std::vector<uint32_t> expired;
        for (auto const& [bot, claim] : claims)
            if (Expired(claim, nowMs))
                expired.push_back(bot);
        for (uint32_t bot : expired)
            Release(bot);
    }

private:
    struct Entry
    {
        TargetKey key;
        uint32_t stamp;
    };

    static bool Expired(Entry const& entry, uint32_t nowMs) { return uint32_t(nowMs - entry.stamp) > ClaimTtlMs; }

    void Unlink(TargetKey key, uint32_t bot)
    {
        auto const target = byTarget.find(key);
        if (target == byTarget.end())
            return;
        auto& bots = target->second;
        bots.erase(std::remove(bots.begin(), bots.end(), bot), bots.end());
        if (bots.empty())
            byTarget.erase(target);
    }

    std::unordered_map<uint32_t, Entry> claims;
    std::unordered_map<TargetKey, std::vector<uint32_t>> byTarget;
};

// Chance that a path leg is cut short for a pause, given the trip still ahead and the leg about to start.
inline float PauseChance(float remainingYards, float legYards, uint32_t routePauseYards)
{
    if (!routePauseYards || remainingYards <= 80.0f || legYards < 30.0f)
        return 0.0f;
    return std::min(legYards / float(routePauseYards), 0.5f);
}

// Last path index kept when a leg of `pointCount` points is cut for a pause; 0 = the leg is too short to cut.
inline size_t PauseCutIndex(size_t pointCount, float u)
{
    if (pointCount < 3)
        return 0;
    size_t const last = pointCount - 1;
    size_t const lo = std::max<size_t>(1, (3 * last + 9) / 10);
    size_t const hi = std::max(lo, std::min(pointCount - 2, 7 * last / 10));
    size_t const pick = lo + size_t(std::floor(std::clamp(u, 0.0f, 1.0f) * float(hi - lo + 1)));
    return std::min(pick, hi);
}

constexpr float MinLaneYards = 0.6f;
constexpr float MaxLaneYards = 2.0f;

// A bot's own sideways offset from the centre of every path, signed by side; fixed per seed so a bot keeps its lane.
inline float LaneYards(uint32_t seed)
{
    seed ^= seed >> 16;
    seed *= 0x7feb352du;
    seed ^= seed >> 15;
    seed *= 0x846ca68bu;
    seed ^= seed >> 16;
    float const width = MinLaneYards + (MaxLaneYards - MinLaneYards) * float(seed % 1000u) / 999.0f;
    return (seed & 0x10000u) ? width : -width;
}

// Share of the lane offset at each point, given the distance walked to it: 0 at both ends, easing to 1 over
// `taperYards` (smoothstep), so a path never swings from one side to the other.
inline std::vector<float> LaneWeights(std::vector<float> const& along, float taperYards)
{
    std::vector<float> weights(along.size(), 0.0f);
    if (along.size() < 3 || taperYards <= 0.0f)
        return weights;
    float const total = along.back();
    for (size_t i = 1; i + 1 < along.size(); ++i)
    {
        float const t = std::clamp(std::min(along[i], total - along[i]) / taperYards, 0.0f, 1.0f);
        weights[i] = t * t * (3.0f - 2.0f * t);
    }
    return weights;
}

// A partial path that ends beside its target but a floor above or below it.
inline bool IsFloorMiss(float endX, float endY, float endZ, float x, float y, float z)
{
    return std::hypot(endX - x, endY - y) <= 8.0f && std::fabs(endZ - z) > 3.0f;
}
}  // namespace HumanMovementRules

#endif
