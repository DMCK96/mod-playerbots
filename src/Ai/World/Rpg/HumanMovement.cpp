/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "HumanMovement.h"

#include "CellImpl.h"
#include "Creature.h"
#include "DetourNavMeshQuery.h"
#include "G3D/Vector2.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "IVMapMgr.h"
#include "Log.h"
#include "Map.h"
#include "MapDefines.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Random.h"
#include "Timer.h"
#include "World.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace HumanMovement
{
namespace
{
constexpr float CellSize = 3.0f;
constexpr float PersonalSpace = 1.0f;
constexpr float OverlapDistance = 0.5f;
constexpr uint32 SoftCapPerCell = 6;
constexpr uint32 PickTries = 6;
constexpr float WidenPerTry = 0.35f;
constexpr uint32 SpotTtlMs = 15 * 60 * 1000;
constexpr uint32 IdleBoardMs = 10 * 60 * 1000;
constexpr uint32 BoardSweepMs = 60 * 1000;
constexpr float LiveRadiusNpc = 6.0f;
constexpr float LiveRadiusPlace = 20.0f;
constexpr float LiveScanMaxDistance = 300.0f;
constexpr float MaxSegmentTurnCos = 0.26f;  // cos(75 degrees): sharper turns stay on the stock path
constexpr size_t MinCurvePoints = 5;
constexpr float LaneTaperYards = 8.0f;
constexpr float SpotMeshHorizontal = 1.5f;
constexpr float SpotMeshVertical = 2.5f;

struct Radii
{
    float min;
    float max;
};

Radii RadiiFor(SpotKind kind)
{
    switch (kind)
    {
        case SpotKind::Npc:
            return {0.8f, 6.0f};
        case SpotKind::Hub:
            return {2.0f, 8.0f};
        case SpotKind::Flight:
            return {2.0f, 6.0f};
        default:
            return {3.0f, 25.0f};
    }
}

struct Spot
{
    ObjectGuid::LowType bot;
    float x;
    float y;
    uint32 stamp;
};

struct Grid
{
    std::mutex lock;
    std::unordered_map<uint64, std::vector<Spot>> cells;
    std::unordered_map<ObjectGuid::LowType, uint64> where;
    HumanMovementRules::IntentTable intents;
    uint32 lastUsed{0};
};

thread_local bool probing = false;
thread_local uint32 claimSerial = 0;

std::mutex gridsLock;
std::unordered_map<uint64, std::shared_ptr<Grid>> grids;
uint32 nextBoardSweep{0};

// Instance maps come and go; a board nobody touched for IdleBoardMs and holding nothing is dropped. Boards in use
// are skipped, and holders keep a dropped board alive through their shared_ptr.
void SweepIdleBoards(uint32 now)
{
    for (auto it = grids.begin(); it != grids.end();)
    {
        Grid& grid = *it->second;
        bool idle = false;
        if (uint32(now - grid.lastUsed) > IdleBoardMs && grid.lock.try_lock())
        {
            grid.intents.Prune(now);
            idle = grid.intents.Empty() && grid.cells.empty();
            grid.lock.unlock();
        }
        it = idle ? grids.erase(it) : std::next(it);
    }
}

std::shared_ptr<Grid> GridFor(Map* map)
{
    uint64 const key = (uint64(map->GetId()) << 32) | map->GetInstanceId();
    uint32 const now = getMSTime();
    std::lock_guard<std::mutex> guard(gridsLock);
    if (int32(now - nextBoardSweep) >= 0)
    {
        nextBoardSweep = now + BoardSweepMs;
        SweepIdleBoards(now);
    }
    std::shared_ptr<Grid>& grid = grids[key];
    if (!grid)
        grid = std::make_shared<Grid>();
    grid->lastUsed = now;
    return grid;
}

int32 CellOf(float v)
{
    return int32(std::floor(v / CellSize));
}

uint64 CellKey(int32 cx, int32 cy)
{
    return (uint64(uint32(cx)) << 32) | uint32(cy);
}

bool Expired(Spot const& spot, uint32 now)
{
    return uint32(now - spot.stamp) > SpotTtlMs;
}

struct Neighbours
{
    float nearest2{1.0e9f};
    uint32 inCell{0};
};

Neighbours Probe(Grid& grid, ObjectGuid::LowType self, float x, float y, uint32 now)
{
    Neighbours result;
    int32 const cx = CellOf(x);
    int32 const cy = CellOf(y);
    for (int32 dx = -1; dx <= 1; ++dx)
    {
        for (int32 dy = -1; dy <= 1; ++dy)
        {
            auto it = grid.cells.find(CellKey(cx + dx, cy + dy));
            if (it == grid.cells.end())
                continue;
            for (Spot const& spot : it->second)
            {
                if (spot.bot == self || Expired(spot, now))
                    continue;
                float const ox = spot.x - x;
                float const oy = spot.y - y;
                float const d2 = ox * ox + oy * oy;
                result.nearest2 = std::min(result.nearest2, d2);
                if (d2 < CellSize * CellSize)
                    ++result.inCell;
            }
        }
    }
    return result;
}

void Release(Grid& grid, ObjectGuid::LowType bot)
{
    auto where = grid.where.find(bot);
    if (where == grid.where.end())
        return;
    auto cell = grid.cells.find(where->second);
    if (cell != grid.cells.end())
    {
        auto& spots = cell->second;
        spots.erase(std::remove_if(spots.begin(), spots.end(), [bot](Spot const& s) { return s.bot == bot; }),
                    spots.end());
        if (spots.empty())
            grid.cells.erase(cell);
    }
    grid.where.erase(where);
}

void Claim(Grid& grid, ObjectGuid::LowType bot, float x, float y, uint32 now)
{
    if (probing)
        return;
    Release(grid, bot);
    uint64 const key = CellKey(CellOf(x), CellOf(y));
    std::vector<Spot>& spots = grid.cells[key];
    spots.erase(std::remove_if(spots.begin(), spots.end(), [now](Spot const& s) { return Expired(s, now); }),
                spots.end());
    spots.push_back({bot, x, y, now});
    grid.where[bot] = key;
}

struct Counters
{
    std::atomic<uint64> picks{0};
    std::atomic<uint64> retries{0};
    std::atomic<uint64> fallbacks{0};
    std::atomic<uint64> overlaps{0};
    std::atomic<uint64> smooth{0};
    std::atomic<uint64> plain{0};
    std::atomic<uint64> dwells{0};
    std::atomic<uint64> dwellMs{0};
    std::atomic<uint64> hops{0};
    std::atomic<uint64> emotes{0};
    std::atomic<uint64> targets{0};
    std::atomic<uint64> targetRepeats{0};
    std::atomic<uint64> liveScans{0};
    std::atomic<uint64> occupancySum[3]{};
    std::atomic<uint64> occupancyPicks[3]{};
    std::atomic<uint64> pauses{0};
    std::atomic<uint64> roadPlans{0};
    std::atomic<uint64> roadNoGraph{0};
    std::atomic<uint64> roadUsed{0};
    std::atomic<uint64> roadNoRoad{0};
    std::atomic<uint64> roadTooLong{0};
    std::atomic<uint64> roadWaypoints{0};
    std::atomic<uint64> roadAbandons{0};
    std::atomic<uint64> roadMicros{0};
    std::atomic<uint64> micros[3]{};
    std::atomic<uint64> straight[4]{};
    std::atomic<uint64> snaps{0};
    std::atomic<uint64> snapCentiyards{0};
    std::atomic<uint64> floorMisses{0};
    std::atomic<uint64> approaches{0};
    std::atomic<uint64> teleports{0};
    std::atomic<uint32> nextLog{0};
};

Counters counters;

// One example position per kind of pathing failure and log interval, to look at in game.
struct PathSample
{
    bool set{false};
    uint32 mapId{0};
    G3D::Vector3 from;
    G3D::Vector3 to;
};

constexpr size_t SampleStraight = 0;
constexpr size_t SampleFloorMiss = 1;
constexpr size_t SampleTeleport = 2;
std::mutex samplesLock;
std::array<PathSample, 3> samples;

void KeepSample(size_t slot, uint32 mapId, G3D::Vector3 const& from, G3D::Vector3 const& to)
{
    std::lock_guard<std::mutex> guard(samplesLock);
    if (!samples[slot].set)
        samples[slot] = {true, mapId, from, to};
}

std::string DescribeSample(PathSample const& sample)
{
    if (!sample.set)
        return "-";
    return Acore::StringFormat("map {} ({:.1f}, {:.1f}, {:.1f}) -> ({:.1f}, {:.1f}, {:.1f})", sample.mapId,
                               sample.from.x, sample.from.y, sample.from.z, sample.to.x, sample.to.y, sample.to.z);
}

void LogPathing(uint32 interval)
{
    std::array<PathSample, 3> taken;
    {
        std::lock_guard<std::mutex> guard(samplesLock);
        taken = samples;
        samples = {};
    }
    uint64 straight[4];
    uint64 straightTotal = 0;
    for (size_t why = 0; why < 4; ++why)
        straightTotal += straight[why] = counters.straight[why].exchange(0);
    uint64 const snaps = counters.snaps.exchange(0);
    uint64 const snapCentiyards = counters.snapCentiyards.exchange(0);
    LOG_INFO("playerbots",
             "coa pathing (last {}s): straight lines {} (no tile {}, start off mesh {}, end off mesh {}, no route {}), "
             "snapped {} (avg {:.1f} yd), floor misses {} (approached {}), stuck teleports {}; "
             "samples: straight {}, floor miss {}, teleport {}",
             interval, straightTotal, straight[size_t(Straight::NoTile)], straight[size_t(Straight::StartOffMesh)],
             straight[size_t(Straight::EndOffMesh)], straight[size_t(Straight::NoRoute)], snaps,
             snaps ? double(snapCentiyards) / double(snaps) / 100.0 : 0.0, counters.floorMisses.exchange(0),
             counters.approaches.exchange(0), counters.teleports.exchange(0), DescribeSample(taken[SampleStraight]),
             DescribeSample(taken[SampleFloorMiss]), DescribeSample(taken[SampleTeleport]));
}

dtQueryFilter const& BotMeshFilter()
{
    static dtQueryFilter const filter = []()
    {
        dtQueryFilter f;
        f.setIncludeFlags(NAV_GROUND | NAV_WATER);
        f.setExcludeFlags(NAV_MAGMA | NAV_SLIME | NAV_GROUND_STEEP);
        return f;
    }();
    return filter;
}

void MaybeLog()
{
    uint32 const interval = sPlayerbotAIConfig.coaMovementMetricsSeconds;
    if (!interval)
        return;
    uint32 const now = uint32(std::time(nullptr));
    uint32 next = counters.nextLog.load(std::memory_order_relaxed);
    if (now < next || !counters.nextLog.compare_exchange_strong(next, now + interval))
        return;
    if (!next)
        return;

    uint64 const picks = counters.picks.exchange(0);
    uint64 const overlaps = counters.overlaps.exchange(0);
    uint64 const smooth = counters.smooth.exchange(0);
    uint64 const plain = counters.plain.exchange(0);
    uint64 const dwells = counters.dwells.exchange(0);
    uint64 const dwellMs = counters.dwellMs.exchange(0);
    uint64 const pickMicros = counters.micros[CostSlotPick].exchange(0);
    uint64 const launchMicros = counters.micros[CostSlotLaunch].exchange(0);
    uint64 const targets = counters.targets.exchange(0);
    uint64 const repeats = counters.targetRepeats.exchange(0);
    uint64 const chooseMicros = counters.micros[CostSlotChoose].exchange(0);
    double occupancy[3];
    for (uint32 kind = 0; kind < 3; ++kind)
    {
        uint64 const sum = counters.occupancySum[kind].exchange(0);
        uint64 const n = counters.occupancyPicks[kind].exchange(0);
        occupancy[kind] = n ? double(sum) / double(n) : 0.0;
    }
    uint64 const roadPlans = counters.roadPlans.exchange(0);
    uint64 const roadNoGraph = counters.roadNoGraph.exchange(0);
    uint64 const roadMicros = counters.roadMicros.exchange(0);
    uint64 const roadSearches = roadPlans - std::min(roadPlans, roadNoGraph);
    LOG_INFO("playerbots",
             "coa movement (last {}s): spots {} (overlap {} = {:.1f}%, retries {}, fallbacks {}), lanes {} plain {}, "
             "dwells {} (avg {:.1f}s), hops {}, emotes {}, cost: pick {:.1f}us avg, launch {:.1f}us avg, "
             "targets {} (repeat {:.1f}%, live scans {}, occupancy hub {:.1f} spread {:.1f} place {:.1f}, "
             "choose {:.1f}us avg), pauses {}, road plans {} (used {}, no graph {}, no road {}, too long {}), "
             "waypoints {}, abandoned {}, plan {:.1f}us avg",
             interval, picks, overlaps, picks ? 100.0 * double(overlaps) / double(picks) : 0.0,
             counters.retries.exchange(0), counters.fallbacks.exchange(0), smooth, plain, dwells,
             dwells ? double(dwellMs) / double(dwells) / 1000.0 : 0.0, counters.hops.exchange(0),
             counters.emotes.exchange(0), picks ? double(pickMicros) / double(picks) : 0.0,
             (smooth + plain) ? double(launchMicros) / double(smooth + plain) : 0.0, targets,
             targets ? 100.0 * double(repeats) / double(targets) : 0.0, counters.liveScans.exchange(0), occupancy[0],
             occupancy[1], occupancy[2], targets ? double(chooseMicros) / double(targets) : 0.0,
             counters.pauses.exchange(0), roadPlans, counters.roadUsed.exchange(0), roadNoGraph,
             counters.roadNoRoad.exchange(0), counters.roadTooLong.exchange(0), counters.roadWaypoints.exchange(0),
             counters.roadAbandons.exchange(0), roadSearches ? double(roadMicros) / double(roadSearches) : 0.0);
    LogPathing(interval);
}

uint32 LivePlayersNear(Player* bot, WorldPosition const& pos, float radius)
{
    if (bot->GetExactDist2d(pos.GetPositionX(), pos.GetPositionY()) > LiveScanMaxDistance)
        return 0;

    float const x = pos.GetPositionX();
    float const y = pos.GetPositionY();
    uint32 count = 0;
    auto const countNear = [&](Player* player)
    {
        if (player != bot && player->IsAlive() && player->GetExactDist2d(x, y) <= radius)
            ++count;
    };
    Acore::PlayerWorker<decltype(countNear)> worker(bot, countNear);
    Cell::VisitObjects(x, y, bot->GetMap(), worker, radius);
    counters.liveScans.fetch_add(1, std::memory_order_relaxed);
    return count;
}

float Gauss()
{
    float const u1 = std::max(float(rand_norm()), 1.0e-6f);
    float const u2 = float(rand_norm());
    return std::sqrt(-2.0f * std::log(u1)) * std::cos(2.0f * float(M_PI) * u2);
}

bool UsableHeight(float h)
{
    return h > INVALID_HEIGHT && h != VMAP_INVALID_HEIGHT_VALUE;
}
}  // namespace

CostTimer::~CostTimer()
{
    auto const elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started);
    counters.micros[slot].fetch_add(uint64(elapsed.count()), std::memory_order_relaxed);
}

WorldPosition PickSpotNear(Player* bot, Map* map, WorldPosition const& anchor, SpotKind kind, float maxRadius)
{
    if (!bot || !map || anchor == WorldPosition() || probing)
        return anchor;

    CostTimer timer(CostSlotPick);
    counters.picks.fetch_add(1, std::memory_order_relaxed);
    MaybeLog();

    std::shared_ptr<Grid> const board = GridFor(map);
    Grid& grid = *board;
    ObjectGuid::LowType const self = bot->GetGUID().GetCounter();
    uint32 const now = getMSTime();
    float const ax = anchor.GetPositionX();
    float const ay = anchor.GetPositionY();

    if (!sPlayerbotAIConfig.coaCrowdSpots || anchor.GetMapId() != map->GetId())
    {
        std::lock_guard<std::mutex> guard(grid.lock);
        if (Probe(grid, self, ax, ay, now).nearest2 < OverlapDistance * OverlapDistance)
            counters.overlaps.fetch_add(1, std::memory_order_relaxed);
        Claim(grid, self, ax, ay, now);
        return anchor;
    }

    Radii radii = RadiiFor(kind);
    if (maxRadius > 0.0f)
    {
        radii.max = std::min(radii.max, maxRadius);
        radii.min = std::min(radii.min, radii.max * 0.5f);
    }
    bool const open = kind == SpotKind::Field;
    uint32 const phase = bot->GetPhaseMask();
    float const collision = bot->GetCollisionHeight();

    for (uint32 attempt = 0; attempt < PickTries; ++attempt)
    {
        if (attempt)
            counters.retries.fetch_add(1, std::memory_order_relaxed);

        float const u = rand_norm();
        float const radius = (radii.min + (radii.max - radii.min) * std::pow(u, 1.5f)) * (1.0f + WidenPerTry * attempt);
        float const angle = frand(0.0f, 2.0f * float(M_PI));
        float const x = ax + radius * std::cos(angle);
        float const y = ay + radius * std::sin(angle);

        float const ground = map->GetHeight(phase, x, y, anchor.GetPositionZ() + 2.0f);
        if (!UsableHeight(ground))
            continue;
        if (std::fabs(ground - anchor.GetPositionZ()) > (open ? 8.0f : 4.0f))
            continue;
        if (map->IsInWater(phase, x, y, ground, collision))
            continue;
        G3D::Vector3 onMesh;
        if (HasMeshTile(map, x, y, ground) &&
            !NearestMeshPoint(map, x, y, ground, SpotMeshHorizontal, SpotMeshVertical, onMesh))
            continue;
        if (!open && !map->isInLineOfSight(ax, ay, anchor.GetPositionZ() + 1.5f, x, y, ground + 1.5f, phase,
                                           LINEOFSIGHT_CHECK_VMAP, VMAP::ModelIgnoreFlags::Nothing))
            continue;

        std::lock_guard<std::mutex> guard(grid.lock);
        Neighbours const nearby = Probe(grid, self, x, y, now);
        if (nearby.nearest2 < PersonalSpace * PersonalSpace || nearby.inCell >= SoftCapPerCell)
            continue;
        Claim(grid, self, x, y, now);
        return WorldPosition(anchor.GetMapId(), x, y, ground);
    }

    counters.fallbacks.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> guard(grid.lock);
    if (Probe(grid, self, ax, ay, now).nearest2 < OverlapDistance * OverlapDistance)
        counters.overlaps.fetch_add(1, std::memory_order_relaxed);
    Claim(grid, self, ax, ay, now);
    return anchor;
}

int32 ChooseTarget(Player* bot, std::vector<Candidate> const& candidates,
                   HumanMovementRules::RecentTargets const& recent, bool weighDistance)
{
    using namespace HumanMovementRules;

    if (!bot || !bot->GetMap() || candidates.empty())
        return -1;

    CostTimer timer(CostSlotChoose);
    std::shared_ptr<Grid> const board = GridFor(bot->GetMap());
    ObjectGuid::LowType const self = bot->GetGUID().GetCounter();
    uint32 const now = getMSTime();
    size_t const n = candidates.size();
    std::vector<uint32> intent(n, 0);
    std::vector<float> weights(n, 0.0f);
    auto const distanceOf = [&](Candidate const& candidate)
    { return weighDistance ? bot->GetExactDist(candidate.pos) : 0.0f; };

    {
        std::lock_guard<std::mutex> guard(board->lock);
        TargetKey const own = board->intents.ClaimOf(self, now);
        for (size_t i = 0; i < n; ++i)
        {
            Candidate const& candidate = candidates[i];
            if (candidate.pos.GetMapId() != bot->GetMapId())
                continue;
            uint32 const claimed = board->intents.Count(candidate.key, now);
            intent[i] = candidate.key == own && claimed ? claimed - 1 : claimed;
            weights[i] = TargetWeight(candidate.kind, intent[i], recent.Contains(candidate.key), distanceOf(candidate));
        }
    }

    std::vector<uint32> occupancy(intent);
    for (size_t i : TopIndices(weights, probing ? 0 : MaxLiveScans))
    {
        if (weights[i] <= 0.0f)
            continue;
        Candidate const& candidate = candidates[i];
        float const radius = candidate.kind == TargetKind::Place ? LiveRadiusPlace : LiveRadiusNpc;
        occupancy[i] = Occupancy(intent[i], LivePlayersNear(bot, candidate.pos, radius));
        weights[i] = TargetWeight(candidate.kind, occupancy[i], recent.Contains(candidate.key), distanceOf(candidate));
    }

    int32 const chosen = PickWeighted(weights, float(rand_norm()));
    if (chosen < 0 || probing)
        return chosen;

    Candidate const& picked = candidates[chosen];
    {
        std::lock_guard<std::mutex> guard(board->lock);
        board->intents.Claim(self, picked.key, now);
    }
    ++claimSerial;
    uint32 const kind = uint32(picked.kind);
    counters.targets.fetch_add(1, std::memory_order_relaxed);
    if (recent.Contains(picked.key))
        counters.targetRepeats.fetch_add(1, std::memory_order_relaxed);
    counters.occupancySum[kind].fetch_add(occupancy[chosen], std::memory_order_relaxed);
    counters.occupancyPicks[kind].fetch_add(1, std::memory_order_relaxed);
    MaybeLog();
    return chosen;
}

void ReleaseTarget(Player* bot)
{
    if (!bot || !bot->GetMap())
        return;
    std::shared_ptr<Grid> const board = GridFor(bot->GetMap());
    ObjectGuid::LowType const self = bot->GetGUID().GetCounter();
    std::lock_guard<std::mutex> guard(board->lock);
    board->intents.Release(self);
    Release(*board, self);
}

uint32 ClaimSerial()
{
    return claimSerial;
}

bool Probing()
{
    return probing;
}

ProbeOnly::ProbeOnly() : previous(probing)
{
    probing = true;
}

ProbeOnly::~ProbeOnly()
{
    probing = previous;
}

void RefreshTarget(Player* bot)
{
    if (!bot || !bot->GetMap())
        return;
    std::shared_ptr<Grid> const board = GridFor(bot->GetMap());
    std::lock_guard<std::mutex> guard(board->lock);
    board->intents.Refresh(bot->GetGUID().GetCounter(), getMSTime());
}

HumanMovementRules::TargetKind KindForNpc(Creature const* npc)
{
    static constexpr NPCFlags HubFlags[] = {UNIT_NPC_FLAG_AUCTIONEER,  UNIT_NPC_FLAG_BANKER,
                                            UNIT_NPC_FLAG_GUILD_BANKER, UNIT_NPC_FLAG_INNKEEPER,
                                            UNIT_NPC_FLAG_FLIGHTMASTER, UNIT_NPC_FLAG_BATTLEMASTER};
    for (NPCFlags flag : HubFlags)
        if (npc && npc->HasNpcFlag(flag))
            return HumanMovementRules::TargetKind::Hub;
    return HumanMovementRules::TargetKind::Spread;
}

HumanMovementRules::TargetKey NpcTargetKey(WorldObject const* object)
{
    if (!object)
        return HumanMovementRules::NoTarget;
    Creature const* creature = object->ToCreature();
    uint32 const id = creature && creature->GetSpawnId() ? creature->GetSpawnId() : object->GetGUID().GetCounter();
    return HumanMovementRules::NpcKey(id);
}

HumanMovementRules::TargetKey PlaceKeyFor(WorldPosition const& pos)
{
    return HumanMovementRules::PlaceKey(int32(std::floor(pos.GetPositionX() / 50.0f)),
                                        int32(std::floor(pos.GetPositionY() / 50.0f)));
}

uint32 DwellMs(uint32 minMs, uint32 medianMs, uint32 maxMs)
{
    float const value = float(medianMs) * std::exp(0.7f * Gauss());
    return uint32(std::clamp(value, float(minMs), float(maxMs)));
}

uint32 StartDelayMs()
{
    return urand(0, 8000);
}

uint32 JitterMs(uint32 baseMs, uint32 seed)
{
    seed ^= seed >> 16;
    seed *= 0x45d9f3bu;
    seed ^= seed >> 16;
    return uint32(float(baseMs) * (0.6f + 0.8f * float(seed % 1000u) / 999.0f));
}

bool Chance(uint32 percent)
{
    return urand(0, 99) < percent;
}

bool Humanize(Player* bot, Movement::PointsArray const& in, Movement::PointsArray& out)
{
    if (!sPlayerbotAIConfig.coaSmoothPaths || in.size() < MinCurvePoints || !bot->IsOutdoors())
        return false;

    for (size_t i = 1; i + 1 < in.size(); ++i)
    {
        G3D::Vector2 const a(in[i].x - in[i - 1].x, in[i].y - in[i - 1].y);
        G3D::Vector2 const b(in[i + 1].x - in[i].x, in[i + 1].y - in[i].y);
        float const la = a.length();
        float const lb = b.length();
        if (la < 0.01f || lb < 0.01f)
            continue;
        if (a.dot(b) / (la * lb) < MaxSegmentTurnCos)
            return false;
    }

    Map* map = bot->GetMap();
    uint32 const phase = bot->GetPhaseMask();
    float const collision = bot->GetCollisionHeight();
    G3D::Vector3 const& end = in.back();
    PositionFullTerrainStatus status;
    map->GetFullTerrainStatusForPosition(phase, end.x, end.y, end.z, collision, status);
    if (!status.outdoors)
        return false;

    std::vector<float> along(in.size(), 0.0f);
    for (size_t i = 1; i < in.size(); ++i)
        along[i] = along[i - 1] + std::hypot(in[i].x - in[i - 1].x, in[i].y - in[i - 1].y);
    std::vector<float> const weights = HumanMovementRules::LaneWeights(along, LaneTaperYards);
    float const lane = HumanMovementRules::LaneYards(bot->GetGUID().GetCounter());

    out = in;
    auto const clear = [&](G3D::Vector3 const& a, G3D::Vector3 const& b)
    {
        return map->isInLineOfSight(a.x, a.y, a.z + 1.5f, b.x, b.y, b.z + 1.5f, phase, LINEOFSIGHT_CHECK_VMAP,
                                    VMAP::ModelIgnoreFlags::Nothing);
    };
    for (size_t i = 1; i + 1 < in.size(); ++i)
    {
        if (weights[i] <= 0.0f)
            continue;
        G3D::Vector2 dir(in[i + 1].x - in[i - 1].x, in[i + 1].y - in[i - 1].y);
        float const length = dir.length();
        if (length < 0.01f)
            continue;
        dir /= length;
        float const offset = lane * weights[i];
        float const x = in[i].x - dir.y * offset;
        float const y = in[i].y + dir.x * offset;
        float const z = map->GetHeight(phase, x, y, in[i].z + 1.0f);
        if (!UsableHeight(z) || std::fabs(z - in[i].z) > 1.2f || map->IsInWater(phase, x, y, z, collision))
            continue;
        G3D::Vector3 const shifted(x, y, z);
        if (!clear(in[i], shifted) || !clear(out[i - 1], shifted) || !clear(shifted, in[i + 1]))
            continue;
        out[i] = shifted;
    }
    return true;
}

bool NearestMeshPoint(Map* map, float x, float y, float z, float horizontal, float vertical, G3D::Vector3& out)
{
    dtNavMeshQuery const* query = map->GetMapCollisionData().GetMMapData().GetNavMeshQuery();
    if (!query)
        return false;
    float const point[3] = {y, z, x};
    float const extents[3] = {horizontal, vertical, horizontal};
    dtPolyRef ref = 0;
    float closest[3] = {0.0f, 0.0f, 0.0f};
    if (dtStatusFailed(query->findNearestPoly(point, extents, &BotMeshFilter(), &ref, closest)) || !ref)
        return false;
    out = G3D::Vector3(closest[2], closest[0], closest[1]);
    return true;
}

bool HasMeshTile(Map* map, float x, float y, float z)
{
    dtNavMesh const* mesh = map->GetMapCollisionData().GetMMapData().GetNavMesh();
    if (!mesh)
        return false;
    float const point[3] = {y, z, x};
    int tx = -1;
    int ty = -1;
    mesh->calcTileLoc(point, &tx, &ty);
    return tx >= 0 && ty >= 0 && mesh->getTileAt(tx, ty, 0) != nullptr;
}

float PoiHeight(Map* map, float x, float y)
{
    float const terrain = map->GetHeight(x, y, MAX_HEIGHT, false);
    G3D::Vector3 onMesh;
    if (UsableHeight(terrain) && NearestMeshPoint(map, x, y, terrain, 3.0f, 4.0f, onMesh))
        return onMesh.z;
    return map->GetHeight(x, y, MAX_HEIGHT);
}

void NoteStraight(Straight why, uint32 mapId, G3D::Vector3 const& from, G3D::Vector3 const& to)
{
    counters.straight[size_t(why)].fetch_add(1, std::memory_order_relaxed);
    KeepSample(SampleStraight, mapId, from, to);
    MaybeLog();
}

void NoteSnap(float yards)
{
    counters.snaps.fetch_add(1, std::memory_order_relaxed);
    counters.snapCentiyards.fetch_add(uint64(yards * 100.0f), std::memory_order_relaxed);
}

void NoteFloorMiss(bool approached, uint32 mapId, G3D::Vector3 const& from, G3D::Vector3 const& to)
{
    counters.floorMisses.fetch_add(1, std::memory_order_relaxed);
    if (approached)
        counters.approaches.fetch_add(1, std::memory_order_relaxed);
    else
        KeepSample(SampleFloorMiss, mapId, from, to);
    MaybeLog();
}

void NoteStuckTeleport(uint32 mapId, G3D::Vector3 const& from, G3D::Vector3 const& to)
{
    counters.teleports.fetch_add(1, std::memory_order_relaxed);
    KeepSample(SampleTeleport, mapId, from, to);
    MaybeLog();
}

void NoteDwell(uint32 ms)
{
    counters.dwells.fetch_add(1, std::memory_order_relaxed);
    counters.dwellMs.fetch_add(ms, std::memory_order_relaxed);
    MaybeLog();
}

void NoteHop()
{
    counters.hops.fetch_add(1, std::memory_order_relaxed);
}

void NoteEmote()
{
    counters.emotes.fetch_add(1, std::memory_order_relaxed);
}

void NoteSmoothLaunch(bool smooth)
{
    (smooth ? counters.smooth : counters.plain).fetch_add(1, std::memory_order_relaxed);
    MaybeLog();
}

void NotePause()
{
    counters.pauses.fetch_add(1, std::memory_order_relaxed);
}

namespace
{
struct Walk
{
    uint32 mapId{0};
    float x{0.0f};
    float y{0.0f};
    float accrued{0.0f};
    bool seen{false};
};

std::mutex walksLock;
std::unordered_map<ObjectGuid::LowType, Walk> walks;
}  // namespace

float AccruePauseYards(Player const* bot)
{
    std::lock_guard<std::mutex> guard(walksLock);
    Walk& walk = walks[bot->GetGUID().GetCounter()];
    float const x = bot->GetPositionX();
    float const y = bot->GetPositionY();
    if (walk.seen && walk.mapId == bot->GetMapId())
        walk.accrued = HumanMovementRules::AccrueWalked(walk.accrued, std::hypot(x - walk.x, y - walk.y));
    else
        walk.accrued = 0.0f;
    walk.seen = true;
    walk.mapId = bot->GetMapId();
    walk.x = x;
    walk.y = y;
    return walk.accrued;
}

void SpendPauseYards(Player const* bot)
{
    std::lock_guard<std::mutex> guard(walksLock);
    walks[bot->GetGUID().GetCounter()].accrued = 0.0f;
}

std::shared_ptr<RoadRules::RoadGraph const> RoadGraphFor(uint32 mapId)
{
    static std::mutex lock;
    static std::unordered_map<uint32, std::shared_ptr<RoadRules::RoadGraph const>> graphs;
    std::lock_guard<std::mutex> guard(lock);
    auto const cached = graphs.find(mapId);
    if (cached != graphs.end())
        return cached->second;

    std::shared_ptr<RoadRules::RoadGraph const> graph;
    std::string const path = Acore::StringFormat("{}roads/{:03}.roads", sWorld->GetDataPath(), mapId);
    std::ifstream file(path, std::ios::binary);
    if (!file)
        LOG_DEBUG("playerbots", "coa movement: no road graph for map {} ({})", mapId, path);
    else
    {
        file.seekg(0, std::ios::end);
        std::vector<uint8_t> bytes(size_t(std::max<std::streamoff>(file.tellg(), 0)));
        file.seekg(0, std::ios::beg);
        file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
        if (std::optional<RoadRules::RoadGraph> parsed = RoadRules::RoadGraph::Parse(bytes.data(), bytes.size()))
        {
            graph = std::make_shared<RoadRules::RoadGraph const>(std::move(*parsed));
            LOG_INFO("playerbots", "coa movement: road graph for map {}: {} nodes, {} edges", mapId,
                     graph->NodeCount(), graph->EdgeCount());
        }
        else
            LOG_ERROR("playerbots", "coa movement: road graph {} is damaged or of another version, ignored", path);
    }
    graphs[mapId] = graph;
    return graph;
}

namespace
{
std::mutex roadBlockLock;
std::unordered_map<uint32, RoadRules::EdgeBlocklist> roadBlocks;
}  // namespace

void BlockRoadEdge(uint32 mapId, uint32_t edge)
{
    std::lock_guard<std::mutex> guard(roadBlockLock);
    roadBlocks[mapId].Block(edge, getMSTime());
}

std::unordered_set<uint32_t> BlockedRoadEdges(uint32 mapId)
{
    std::lock_guard<std::mutex> guard(roadBlockLock);
    auto const blocks = roadBlocks.find(mapId);
    return blocks == roadBlocks.end() ? std::unordered_set<uint32_t>{} : blocks->second.Snapshot(getMSTime());
}

void NoteRoadPlan(RoadRules::PlanResult result, bool hadGraph, uint32 micros)
{
    counters.roadPlans.fetch_add(1, std::memory_order_relaxed);
    if (!hadGraph)
    {
        counters.roadNoGraph.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    counters.roadMicros.fetch_add(micros, std::memory_order_relaxed);
    switch (result)
    {
        case RoadRules::PlanResult::Used:
            counters.roadUsed.fetch_add(1, std::memory_order_relaxed);
            break;
        case RoadRules::PlanResult::TooLong:
            counters.roadTooLong.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            counters.roadNoRoad.fetch_add(1, std::memory_order_relaxed);
            break;
    }
}

void NoteRoadWaypoint()
{
    counters.roadWaypoints.fetch_add(1, std::memory_order_relaxed);
}

void NoteRoadAbandon()
{
    counters.roadAbandons.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace HumanMovement
