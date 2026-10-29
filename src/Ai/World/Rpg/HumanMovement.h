/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_HUMANMOVEMENT_H
#define PLAYERBOTS_HUMANMOVEMENT_H

#include "Define.h"
#include "HumanMovementRules.h"
#include "MoveSplineInitArgs.h"
#include "RoadGraph.h"
#include "TravelMgr.h"

#include <chrono>
#include <memory>
#include <unordered_set>
#include <vector>

class Creature;
class Map;
class Player;
class WorldObject;

// Ambient (out of combat, RPG) movement that reads as a player's: destinations that cluster around a point of
// interest without sharing a coordinate, varied standing time, curved paths. Every part has an
// AiPlayerbot.Coa* switch and reports to the "coa movement" log line.
namespace HumanMovement
{
enum class SpotKind
{
    Npc,     // vendor, innkeeper, trainer, quest giver
    Hub,     // auction house, bank, mailbox, inn, camp
    Flight,  // flight master
    Field    // grind area, quest objective, random teleport
};

// A spot around `anchor` for the bot to stand at. Returns the anchor itself when nothing better is found or the
// feature is off; the choice is recorded for the overlap figures either way. `maxRadius` > 0 caps the radius.
WorldPosition PickSpotNear(Player* bot, Map* map, WorldPosition const& anchor, SpotKind kind, float maxRadius = 0.0f);

struct Candidate
{
    HumanMovementRules::TargetKey key;
    HumanMovementRules::TargetKind kind;
    WorldPosition pos;
};

// Index of the candidate to visit, weighted by kind, how many bots are heading to or standing at it, whether the
// bot was there lately and (when `weighDistance`) distance; -1 when none qualifies. The choice is claimed on the
// bot's map. Pickers that already bound their range by distance pass `weighDistance` false.
int32 ChooseTarget(Player* bot, std::vector<Candidate> const& candidates,
                   HumanMovementRules::RecentTargets const& recent, bool weighDistance = true);
void ReleaseTarget(Player* bot);
void RefreshTarget(Player* bot);
// Targets claimed on this thread so far: a status change that claimed nothing new releases the old target.
uint32 ClaimSerial();
bool Probing();

// Pickers called inside this scope choose without claiming a target or a spot (status availability checks).
class ProbeOnly
{
public:
    ProbeOnly();
    ~ProbeOnly();
    ProbeOnly(ProbeOnly const&) = delete;
    ProbeOnly& operator=(ProbeOnly const&) = delete;

private:
    bool previous;
};
HumanMovementRules::TargetKind KindForNpc(Creature const* npc);
// A creature by its spawn, so a respawn keeps its key; other objects by their GUID counter.
HumanMovementRules::TargetKey NpcTargetKey(WorldObject const* object);
// The 50-yard cell a grind area, camp or building point falls in.
HumanMovementRules::TargetKey PlaceKeyFor(WorldPosition const& pos);

// Long-tailed standing time: most values near `medianMs`, a few up to `maxMs`.
uint32 DwellMs(uint32 minMs, uint32 medianMs, uint32 maxMs);
// 0-8 s so bots sent by one trigger do not leave together.
uint32 StartDelayMs();
// `baseMs` scaled by 0.6-1.4, the same factor for the same `seed`.
uint32 JitterMs(uint32 baseMs, uint32 seed);
bool Chance(uint32 percent);

// A navmesh path moved into the bot's own lane, a fixed sideways offset that eases in and out at the ends, so
// bots on one route do not walk in single file. Returns false when the path should stay as it is.
bool Humanize(Player* bot, Movement::PointsArray const& in, Movement::PointsArray& out);

// The nearest point of the navmesh bots walk on, within `horizontal` yards across and `vertical` up or down.
bool NearestMeshPoint(Map* map, float x, float y, float z, float horizontal, float vertical, G3D::Vector3& out);
bool HasMeshTile(Map* map, float x, float y, float z);
// Ground height for a point known only by x and y (quest areas): the terrain when the navmesh lies on it,
// otherwise the topmost surface as before.
float PoiHeight(Map* map, float x, float y);

// Why a move fell back to the core's straight line.
enum class Straight
{
    NoTile,
    StartOffMesh,
    EndOffMesh,
    NoRoute
};
void NoteStraight(Straight why, uint32 mapId, G3D::Vector3 const& from, G3D::Vector3 const& to);
void NoteSnap(float yards);
void NoteFloorMiss(bool approached, uint32 mapId, G3D::Vector3 const& from, G3D::Vector3 const& to);
void NoteStuckTeleport(uint32 mapId, G3D::Vector3 const& from, G3D::Vector3 const& to);

void NoteDwell(uint32 ms);
void NoteHop();
void NoteEmote();
void NoteSmoothLaunch(bool smooth);
void NotePause();

// Road graph of a map from DataDir/roads/<map>.roads, loaded on first use; null when the map has none.
std::shared_ptr<RoadRules::RoadGraph const> RoadGraphFor(uint32 mapId);
// Edges bots failed to walk, skipped by road plans on that map for RoadRules::BlockMs.
void BlockRoadEdge(uint32 mapId, uint32_t edge);
std::unordered_set<uint32_t> BlockedRoadEdges(uint32 mapId);
void NoteRoadPlan(RoadRules::PlanResult result, bool hadGraph, uint32 micros);
void NoteRoadWaypoint();
void NoteRoadAbandon();

class CostTimer
{
public:
    explicit CostTimer(uint32 slot) : slot(slot), started(std::chrono::steady_clock::now()) {}
    ~CostTimer();

private:
    uint32 slot;
    std::chrono::steady_clock::time_point started;
};

constexpr uint32 CostSlotPick = 0;
constexpr uint32 CostSlotLaunch = 1;
constexpr uint32 CostSlotChoose = 2;
}  // namespace HumanMovement

#endif
