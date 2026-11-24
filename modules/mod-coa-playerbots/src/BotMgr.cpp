#include "BotPerformance.h"
#include "BotScheduling.h"
#include "BotWorkBudget.h"
#include "training/TrainingArenaManager.h"
#include "engine/BotDebugLog.h"
#include "BotMgr.h"
#include "Bag.h"
#include <map>
#include "BotAddonChat.h"
#include "BotProtectionPolicy.h"
#include "BotGear.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include "AccountMgr.h"
#include "AscensionClassServiceBridge.h"
#include "AscensionCoATalentData.h"
#include "BotAI.h"
#include "BotMovement.h"
#include "BotEconomy.h"
#include "BotEconomyPolicy.h"
#include "engine/SpecStrategyRegistry.h"
#include "engine/ActionEvaluator.h"
#include "BotSpawnRandom.h"
#include "BotTalentBuilds.h"
#include "BotProgression.h"
#include "BotZoneProgression.h"
#include "WorldBrain.h"
#include "world/encounters/WorldEncounterDirector.h"
#include "ClassSpecRoles.h"
#include "CellImpl.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Config.h"
#include "Corpse.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "BotGearPolicy.h"
#include "Group.h"
#include "GroupScript.h"
#include "engine/GroupDirector.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "GuildPackets.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "LootMgr.h"
#include "Mail.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PetDefines.h"
#include "Player.h"
#include "PlayerScript.h"
#include "QueryHolder.h"
#include "Realm.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "TradeData.h"
#include "SpellAuraDefines.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
    // A client without the custom races patch crashes on a unit of race 12 or above; the world refuses them.
    bool CustomRacesOff()
    {
        return !sConfigMgr->GetOption<bool>("CoACustomRaces.Enable", false);
    }

    bool IsCustomRace(uint8 race)
    {
        return race == 9 || race > 11;
    }
}

namespace
{
// Acore::NearestHostileUnitCheck (GridNotifiers.h) does exactly this shrinking-radius
// "nearest" search already, but its constructor only accepts a Creature const* -- a bot is a
// Player, so it can't be reused directly. This is the same check, generalized to any Unit.
class NearestHostileUnitInObjectRangeCheck
{
public:
    explicit NearestHostileUnitInObjectRangeCheck(Unit const* me, float range) : _me(me), _range(range) { }
    bool operator()(Unit* u)
    {
        if (!_me->IsWithinDistInMap(u, _range, true, false, false))
            return false;
        if (!_me->IsValidAttackTarget(u))
            return false;
        _range = _me->GetDistance(u); // shrink the search radius to the closest hit so far
        return true;
    }

private:
    Unit const* _me;
    float _range;
};

char const* RoleToString(BotRole role)
{
    switch (role)
    {
        case BotRole::Tank: return "tank";
        case BotRole::Healer: return "healer";
        case BotRole::Support: return "support";
        case BotRole::Dps: default: return "dps";
    }
}

// itemEntry -> every learned-spell id with a SPELL_EFFECT_CREATE_ITEM effect producing it.
// Built once (first CraftOrder call) by scanning the full spell store -- crafting orders are
// rare, one-off events, not a per-tick operation, so a one-time O(spell count) scan is cheap
// relative to how infrequently this runs, and far simpler than trying to reach the same data
// through SkillLineAbility/profession bookkeeping this module doesn't otherwise track. A bot
// merely *knowing* the resulting spell (Player::HasSpell) already proves it leveled the right
// profession to the right skill -- no separate skill-level check needed.
std::unordered_map<uint32, std::vector<uint32>> const& CraftingRecipeIndex()
{
    static std::unordered_map<uint32, std::vector<uint32>> index = []
    {
        std::unordered_map<uint32, std::vector<uint32>> map;
        for (uint32 id = 0; id < sSpellMgr->GetSpellInfoStoreSize(); ++id)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
            if (!info)
                continue;
            for (SpellEffectInfo const& effect : info->GetEffects())
                if (effect.Effect == SPELL_EFFECT_CREATE_ITEM && effect.ItemType)
                    map[effect.ItemType].push_back(id);
        }
        return map;
    }();
    return index;
}

// Trade Goods (item_template.class = 7) subclass -> the gather-order category name the addon
// shows. Subclass 8 (Meat) also covers raw fish -- WotLK's own DB never splits them into
// separate subclasses -- so entries in that subclass get a real, data-driven split below
// instead of a name-guess: an item present in fishing_loot_template is Fish, anything else in
// that subclass is Meat.
struct GatherCategoryItem
{
    uint32 entry;
    std::string name;
};

// category name -> every Trade Goods item this realm's own loot tables actually let a bot
// obtain by gathering/looting on its own (no bot AI targets a specific item -- TryStartGathering
// opens whatever herbalism/mining node it finds, TryProcessPendingLoot takes whatever a kill
// drops -- so GuildGather's "order N of item X" is a background wait against opportunistic
// pickup either way; this catalog only exists so the addon can offer a real, obtainable item
// instead of the player typing an arbitrary id). Sourced from a single one-time query joining
// item_template against gameobject_loot_template (herb/ore nodes), skinning_loot_template
// (hides/meat from skinning), fishing_loot_template (raw fish), and creature_loot_template
// (humanoid cloth drops, beast meat) -- deliberately excludes refined/crafted items in the same
// subclasses (bars, bolts, cured leather) since those only ever come from a crafting cast, never
// from anything a bot's autonomous gathering loop can produce, which a plain class/subclass
// filter alone would have wrongly included. Built once and cached -- confirmed via EXPLAIN this
// runs in ~0.1s as a JOIN (a correlated-EXISTS version of the same query took ~9.5s against this
// realm's live creature_loot_template, unacceptable for anything running on the map thread).
std::unordered_map<std::string, std::vector<GatherCategoryItem>> const& GatherableCatalog()
{
    static std::unordered_map<std::string, std::vector<GatherCategoryItem>> catalog = []
    {
        std::unordered_map<std::string, std::vector<GatherCategoryItem>> map;
        QueryResult result = WorldDatabase.Query(
            "SELECT it.entry, it.name, it.subclass, (fi.item IS NOT NULL) AS is_fish "
            "FROM item_template it "
            "JOIN (SELECT item FROM gameobject_loot_template UNION SELECT item FROM skinning_loot_template "
            "UNION SELECT item FROM fishing_loot_template UNION SELECT item FROM creature_loot_template) src "
            "ON src.item = it.entry "
            "LEFT JOIN (SELECT DISTINCT item FROM fishing_loot_template) fi ON fi.item = it.entry "
            "WHERE it.class = 7 AND it.subclass IN (5,6,7,8,9) "
            "AND it.name NOT LIKE '%MISSING%' AND it.name NOT LIKE 'Z:%' "
            "ORDER BY it.subclass, it.RequiredLevel, it.entry");
        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 entry = fields[0].Get<uint32>();
                std::string name = fields[1].Get<std::string>();
                uint32 subclass = fields[2].Get<uint32>();
                bool isFish = fields[3].Get<bool>();

                char const* category = "misc";
                switch (subclass)
                {
                    case 5: category = "cloth"; break;
                    case 6: category = "leather"; break;
                    case 7: category = "ore"; break;
                    case 9: category = "herb"; break;
                    case 8: category = isFish ? "fish" : "meat"; break;
                }
                map[category].push_back({ entry, std::move(name) });
            } while (result->NextRow());
        }
        return map;
    }();
    return catalog;
}

// Spots an order already tried count as "tried" within this many yards (both node and hunt picks).
constexpr float GATHER_TRIED_SPOT_RADIUS = 120.0f;
// A fresh pick is random among this many nearest untried spawns on the bot's map, so repeated
// retries spread over a zone instead of naming the same (empty, pooled-out) spawn every time.
constexpr uint32 GATHER_NEAREST_CANDIDATES = 6;
// How many nearest spawns are pulled per query before the tried-spot filter runs.
constexpr uint32 GATHER_NEAREST_FETCH = 40;

bool IsTriedSpot(std::vector<BotMgr::GuildGatherOrder::TriedSpot> const& tried, uint32 mapId, float x, float y)
{
    for (BotMgr::GuildGatherOrder::TriedSpot const& t : tried)
    {
        if (t.mapId != mapId)
            continue;
        float const dx = t.x - x, dy = t.y - y;
        if (dx * dx + dy * dy <= GATHER_TRIED_SPOT_RADIUS * GATHER_TRIED_SPOT_RADIUS)
            return true;
    }
    return false;
}

// Tries `fromWhereSql` (a full FROM..WHERE clause selecting map/position_x/position_y/position_z
// as its first four columns, table aliased __t, any item-entry placeholder already substituted)
// restricted to the bot's own current map first: a random one of the GATHER_NEAREST_CANDIDATES
// nearest spawns that is not near a spot this order already tried. Picking only "the single
// nearest" made every retry name the very spot the bot was standing on -- a node spawn that is
// pooled out or already picked, so the order looped on one empty spot until it gave up with
// nothing gathered. Falls back to a random spot on any of the other three continents only if
// nothing on the current map qualifies at all (no automated boat/zeppelin travel exists here).
bool PickNearestThenAnyLocation(std::string const& fromWhereSql, uint32 currentMapId, float currentX, float currentY,
    uint32& mapId, float& x, float& y, float& z, std::vector<BotMgr::GuildGatherOrder::TriedSpot> const* tried = nullptr)
{
    struct Spot { uint32 mapId; float x, y, z; };
    std::vector<Spot> fresh, all;
    if (QueryResult nearRes = WorldDatabase.Query(
            (fromWhereSql + " AND __t.map = {} ORDER BY POW(__t.position_x - {}, 2) + POW(__t.position_y - {}, 2) ASC LIMIT {}").c_str(),
            currentMapId, currentX, currentY, GATHER_NEAREST_FETCH))
    {
        do
        {
            Field* fields = nearRes->Fetch();
            Spot const spot{ fields[0].Get<uint32>(), fields[1].Get<float>(), fields[2].Get<float>(), fields[3].Get<float>() };
            all.push_back(spot);
            if (!tried || !IsTriedSpot(*tried, spot.mapId, spot.x, spot.y))
                fresh.push_back(spot);
        } while (nearRes->NextRow());
    }

    std::vector<Spot> const& pool = !fresh.empty() ? fresh : all;
    if (!pool.empty() && (!fresh.empty() || !tried || tried->size() < 3))
    {
        std::size_t const n = std::min<std::size_t>(GATHER_NEAREST_CANDIDATES, pool.size());
        Spot const& pick = pool[urand(0, uint32(n - 1))];
        mapId = pick.mapId;
        x = pick.x;
        y = pick.y;
        z = pick.z;
        return true;
    }

    // Nothing untried on this map (or nothing at all): another continent, at random.
    QueryResult res = WorldDatabase.Query(
        (fromWhereSql + " AND __t.map IN (0, 1, 530, 571) AND __t.map <> {} ORDER BY RAND() LIMIT 1").c_str(), currentMapId);
    if (!res)
    {
        if (all.empty())
            return false;
        Spot const& pick = all[urand(0, uint32(all.size() - 1))]; // only this map has it: any spawn of it
        mapId = pick.mapId;
        x = pick.x;
        y = pick.y;
        z = pick.z;
        return true;
    }

    Field* fields = res->Fetch();
    mapId = fields[0].Get<uint32>();
    x = fields[1].Get<float>();
    y = fields[2].Get<float>();
    z = fields[3].Get<float>();
    return true;
}

// currentMapId/currentX/currentY: the bot's own position right now, so a same-continent result
// can be walked to instead of always being a fresh teleport (see PickNearestThenAnyLocation and,
// for the caller side, GuildGather/RetryGuildGatherLocation only teleporting on an actual map
// change now). Confirmed live 2026-09-29 as a real problem: re-resolving on every retry with a
// pure `ORDER BY RAND()` had bots (and, at fleet scale, dozens of them) teleport-hopping node to
// node/hole to hole instantly with no travel time at all.
bool ResolveGatherLocationForItem(uint32 itemEntry, uint32 currentMapId, float currentX, float currentY,
    uint32& mapId, float& x, float& y, float& z, std::vector<BotMgr::GuildGatherOrder::TriedSpot> const* tried = nullptr,
    std::vector<uint32> const* nodeGoEntries = nullptr)
{
    // 0. A herb/ore item: only spawns of the real Herbalism/Mining nodes that yield it. The plain
    // loot join below also matches chests and crates (Tin Ore is in Armor Crate and quest-chest
    // loot), and a bot sent there finds no node to gather, stalls and burns a spot retry.
    if (nodeGoEntries && !nodeGoEntries->empty())
    {
        std::string ids;
        for (uint32 entry : *nodeGoEntries)
            ids += (ids.empty() ? "" : ",") + std::to_string(entry);
        if (PickNearestThenAnyLocation(
                "SELECT __t.map, __t.position_x, __t.position_y, __t.position_z "
                "FROM gameobject __t WHERE __t.id IN (" + ids + ")",
            currentMapId, currentX, currentY, mapId, x, y, z, tried))
            return true;
    }

    // 1. Try gameobject loot (mining, herbalism, chests)
    if (PickNearestThenAnyLocation(
            "SELECT __t.map, __t.position_x, __t.position_y, __t.position_z "
            "FROM gameobject __t "
            "JOIN gameobject_template gt ON __t.id = gt.entry "
            "JOIN gameobject_loot_template glt ON gt.data1 = glt.Entry "
            "WHERE glt.Item = " + std::to_string(itemEntry),
        currentMapId, currentX, currentY, mapId, x, y, z, tried))
        return true;

    // 2. Try creature loot (cloth, meat)
    if (PickNearestThenAnyLocation(
            "SELECT __t.map, __t.position_x, __t.position_y, __t.position_z "
            "FROM creature __t "
            "JOIN creature_template ct ON __t.id = ct.entry "
            "JOIN creature_loot_template clt ON ct.lootid = clt.Entry "
            "WHERE clt.Item = " + std::to_string(itemEntry),
        currentMapId, currentX, currentY, mapId, x, y, z, tried))
        return true;

    // 3. Try skinning loot (leather)
    if (PickNearestThenAnyLocation(
            "SELECT __t.map, __t.position_x, __t.position_y, __t.position_z "
            "FROM creature __t "
            "JOIN creature_template ct ON __t.id = ct.entry "
            "JOIN skinning_loot_template slt ON ct.skinloot = slt.Entry "
            "WHERE slt.Item = " + std::to_string(itemEntry),
        currentMapId, currentX, currentY, mapId, x, y, z, tried))
        return true;

    // 4. Try fishing loot. A `game_tele` entry (the old fallback here) is just "somewhere someone
    // once wanted to teleport to" -- no guarantee of being anywhere near water, which is the one
    // thing TryStartFishing's own FindNearbyWater can't fix (it only samples a ring around
    // wherever the bot already is). A real GameObjectTemplate type 25 (FISHINGHOLE) spawn is
    // guaranteed to actually sit in water, so use one of those instead. It doesn't need to be a
    // hole that happens to yield this exact item -- open-water fishing (the real SPELL_FISHING
    // cast TryStartFishing uses) resolves its own loot from the zone the bobber lands in, same as
    // a real player fishing there, regardless of which pool (if any) is nearby.
    QueryResult fiRes = WorldDatabase.Query(
        "SELECT entry FROM fishing_loot_template WHERE item = {} LIMIT 1", itemEntry);
    if (fiRes && PickNearestThenAnyLocation(
            "SELECT __t.map, __t.position_x, __t.position_y, __t.position_z "
            "FROM gameobject __t JOIN gameobject_template gt ON gt.entry = __t.id "
            "WHERE gt.type = 25",
            currentMapId, currentX, currentY, mapId, x, y, z, tried))
        return true;

    return false;
}

// --- Gather-order sourcing, assignment and lifecycle tuning -------------------------------
// Level slack either way: a bot may take a source up to this many levels above itself, and a
// hunt spawn counts as "level-appropriate" for location picking while its maxlevel is within it.
constexpr uint32 GATHER_LEVEL_TOLERANCE = 2;
// Beyond this many levels above the source a bot still qualifies, but every extra level makes it
// a worse pick than a closer-level bot (so a level 80 is not sent for Linen while a 12 is free).
constexpr uint32 GATHER_LEVEL_OVER_SOFT = 10;
// RetryGuildGatherLocation calls (one per stall in BotAI.cpp: ~25 s at an empty node spot, ~45 s for a
// hunt) without a single item gathered in between before the order is given up as stuck.
constexpr uint32 GUILD_GATHER_MAX_STALLED_RETRIES = 15;
// An order whose bot is online and in the world but deposits nothing for this long is given up
// too -- catches loops the stall counter can't see (e.g. killing mobs that never drop the item).
constexpr uint32 GUILD_GATHER_NO_PROGRESS_MS = 30 * MINUTE * IN_MILLISECONDS;
// Retry picks avoid spawns this close (yards) to the spot the order was already pointed at.
constexpr float HUNT_RETRY_AVOID_RADIUS = 100.0f;
// A retry/first pick chooses at random among this many nearest level-appropriate spawns, so
// repeated retries spread over a hunting area instead of always naming the same single mob.
constexpr std::size_t HUNT_NEAREST_CANDIDATES = 6;
// A new spot on the bot's own map further than this is teleported to rather than walked: a
// "nearest" node spawn can sit on the far side of a continent (or across water), and walking
// there took most of the order's time before anything was gathered. Closer spots are walked so
// bots don't hop node to node.
constexpr float GATHER_ORDER_TELEPORT_BEYOND_YARDS = 400.0f;
// Remembered empty spots per order (oldest dropped first).
constexpr std::size_t GATHER_MAX_TRIED_SPOTS = 12;
// A bot that joins a real player's group stops working its order (it follows the leader). After
// this long the rest of the order goes back to the front of the queue for the next free bot.
constexpr uint32 GUILD_GATHER_GROUPED_HANDOFF_MS = 2 * MINUTE * IN_MILLISECONDS;
// Guild-bank deposit attempts per order are spaced this far apart (a full bank used to be
// retried every world tick).
constexpr uint32 GATHER_DEPOSIT_RETRY_MS = 5 * IN_MILLISECONDS;

bool IsOpenWorldContinent(uint32 mapId)
{
    return mapId == 0 || mapId == 1 || mapId == 530 || mapId == 571;
}

// Same formula as Spell::EffectSkinning's reqValue: the skill a creature of `level` needs.
uint32 SkinningSkillForLevel(uint32 level)
{
    return level < 10 ? 0 : level < 20 ? (level - 10) * 10 : level * 5;
}

void CollectLootEntries(std::string const& sql, std::unordered_set<uint32>& out)
{
    if (QueryResult r = WorldDatabase.Query(sql))
    {
        do
        {
            out.insert(r->Fetch()[0].Get<uint32>());
        } while (r->NextRow());
    }
}

// One-time (per item, see BotMgr::GetGatherSourceInfo's cache) world-DB lookup of everything a
// gather order for itemEntry can be sourced from. Loot tables come from SQL; templates and spawns
// come from ObjectMgr's in-memory stores (already loaded, and the CoA `creature` schema's renamed
// columns never come into play). Reference loot is followed one level deep only: an item that is
// only reachable through a reference nested inside another reference is not found here.
BotMgr::GatherSourceInfo BuildGatherSourceInfo(uint32 itemEntry)
{
    BotMgr::GatherSourceInfo info;
    std::string const item = std::to_string(itemEntry);

    // Only a real herbalism/mining node counts as "node-sourced". gameobject_loot_template also
    // holds chests, crates and footlockers -- Linen Cloth and Light Leather are both in it (6 and
    // 10 loot ids on a stock AzerothCore DB) -- and treating those as nodes is what sent cloth
    // orders to random chest spawns before; such items are hunted instead.
    std::unordered_set<uint32> goLootIds;
    CollectLootEntries("SELECT DISTINCT Entry FROM gameobject_loot_template WHERE Item = " + item, goLootIds);
    if (!goLootIds.empty())
    {
        uint32 bestSkill = (std::numeric_limits<uint32>::max)();
        if (GameObjectTemplateContainer const* goTemplates = sObjectMgr->GetGameObjectTemplates())
        {
            for (auto const& [goEntry, goTemplate] : *goTemplates)
            {
                if (!goLootIds.count(goTemplate.GetLootId()))
                    continue;
                LockEntry const* lock = sLockStore.LookupEntry(goTemplate.GetLockId());
                if (!lock)
                    continue;
                for (uint8 j = 0; j < MAX_LOCK_CASE; ++j)
                {
                    if (lock->Type[j] != LOCK_KEY_SKILL)
                        continue;
                    SkillType const skill = SkillByLockType(LockType(lock->Index[j]));
                    if (skill != SKILL_HERBALISM && skill != SKILL_MINING)
                        continue;
                    info.nodeSourced = true;
                    if (info.nodeGoEntries.empty() || info.nodeGoEntries.back() != goEntry)
                        info.nodeGoEntries.push_back(goEntry);
                    if (lock->Skill[j] < bestSkill)
                    {
                        bestSkill = lock->Skill[j];
                        info.nodeSkillId = skill;
                    }
                }
            }
        }
        if (info.nodeSourced)
        {
            info.nodeRequiredSkill = bestSkill;
            // Best per-node chance, for orderstatus: a 2% item needs ~50 nodes per item.
            std::string lootIds;
            for (uint32 goEntry : info.nodeGoEntries)
                if (GameObjectTemplate const* goTemplate = sObjectMgr->GetGameObjectTemplate(goEntry))
                    lootIds += (lootIds.empty() ? "" : ",") + std::to_string(goTemplate->GetLootId());
            if (!lootIds.empty())
                if (QueryResult chance = WorldDatabase.Query("SELECT MAX(Chance) FROM gameobject_loot_template WHERE Item = {} AND Entry IN ({})",
                        itemEntry, lootIds))
                    info.nodeBestChance = chance->Fetch()[0].Get<float>();
            return info; // node items keep the node search; no hunt data needed
        }
    }

    if (QueryResult fish = WorldDatabase.Query("SELECT 1 FROM fishing_loot_template WHERE Item = {} LIMIT 1", itemEntry))
    {
        info.fishSourced = true;
        return info;
    }

    std::unordered_set<uint32> corpseLootIds;
    CollectLootEntries("SELECT DISTINCT Entry FROM creature_loot_template WHERE Reference = 0 AND Item = " + item, corpseLootIds);
    CollectLootEntries("SELECT DISTINCT clt.Entry FROM creature_loot_template clt "
        "JOIN reference_loot_template rlt ON rlt.Entry = clt.Reference "
        "WHERE clt.Reference <> 0 AND rlt.Reference = 0 AND rlt.Item = " + item, corpseLootIds);

    std::unordered_set<uint32> skinLootIds;
    CollectLootEntries("SELECT DISTINCT Entry FROM skinning_loot_template WHERE Reference = 0 AND Item = " + item, skinLootIds);
    CollectLootEntries("SELECT DISTINCT slt.Entry FROM skinning_loot_template slt "
        "JOIN reference_loot_template rlt ON rlt.Entry = slt.Reference "
        "WHERE slt.Reference <> 0 AND rlt.Reference = 0 AND rlt.Item = " + item, skinLootIds);

    if (corpseLootIds.empty() && skinLootIds.empty())
        return info;

    std::unordered_map<uint32, std::pair<uint8, uint8>> levels; // creature entry -> min/max level
    if (CreatureTemplateContainer const* templates = sObjectMgr->GetCreatureTemplates())
    {
        for (auto const& [entry, ct] : *templates)
        {
            // Normal-rank only: elites, rares and bosses are not something a lone bot should be
            // sent to farm (and usually live in instances anyway).
            if (ct.rank != CREATURE_ELITE_NORMAL)
                continue;
            bool const corpse = ct.lootid && corpseLootIds.count(ct.lootid);
            // Skinning loot opened with Herbalism/Mining/Engineering is not something the hunt
            // loop's Skinning cast can open, so those creatures don't count as hide sources.
            bool const skin = ct.SkinLootId && skinLootIds.count(ct.SkinLootId) && ct.GetRequiredLootSkill() == SKILL_SKINNING;
            if (!corpse && !skin)
                continue;
            if (corpse)
                info.huntCorpseEntries.insert(entry);
            if (skin)
                info.huntSkinEntries.insert(entry);
            levels[entry] = { ct.minlevel, ct.maxlevel };
        }
    }

    uint8 minLevel = 0;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        if (!IsOpenWorldContinent(data.mapid))
            continue;
        auto lv = levels.find(data.id);
        if (lv == levels.end())
            continue;
        BotMgr::HuntSpawn spawn;
        spawn.mapId = data.mapid;
        spawn.x = data.posX;
        spawn.y = data.posY;
        spawn.z = data.posZ;
        spawn.minLevel = lv->second.first;
        spawn.maxLevel = std::max(lv->second.first, lv->second.second);
        info.huntSpawns.push_back(spawn);
        if (!minLevel || spawn.minLevel < minLevel)
            minLevel = spawn.minLevel;
    }
    info.huntMinLevel = minLevel;
    return info;
}

// Picks a hunting spot for a drop/skin-sourced order. Order of preference:
//  1. same map as the bot, maxlevel <= bot level + GATHER_LEVEL_TOLERANCE -- random among the
//     HUNT_NEAREST_CANDIDATES nearest (walkable, and a retry lands somewhere new);
//  2. another continent, same level rule -- random (teleport);
//  3. any level, lowest-level spawns first, same map preferred;
// always skipping spawns within HUNT_RETRY_AVOID_RADIUS of `avoid*` when hasAvoid (a retry) and
// at least one other spawn exists.
bool ResolveHuntLocation(BotMgr::GatherSourceInfo const& info, Player* bot, bool hasAvoid, uint32 avoidMap,
    float avoidX, float avoidY, uint32& mapId, float& x, float& y, float& z,
    std::vector<BotMgr::GuildGatherOrder::TriedSpot> const* tried = nullptr)
{
    if (info.huntSpawns.empty())
        return false;

    uint32 const level = bot->GetLevel();
    uint32 const botMap = bot->GetMapId();
    float const bx = bot->GetPositionX();
    float const by = bot->GetPositionY();
    auto avoided = [&](BotMgr::HuntSpawn const& s)
    {
        if (hasAvoid && tried && IsTriedSpot(*tried, s.mapId, s.x, s.y))
            return true;
        if (!hasAvoid || s.mapId != avoidMap)
            return false;
        float const dx = s.x - avoidX, dy = s.y - avoidY;
        return dx * dx + dy * dy < HUNT_RETRY_AVOID_RADIUS * HUNT_RETRY_AVOID_RADIUS;
    };
    auto distSq = [&](BotMgr::HuntSpawn const& s)
    {
        float const dx = s.x - bx, dy = s.y - by;
        return dx * dx + dy * dy;
    };
    auto pick = [&](BotMgr::HuntSpawn const& s)
    {
        mapId = s.mapId;
        x = s.x;
        y = s.y;
        z = s.z;
        return true;
    };

    for (int pass = 0; pass < 2; ++pass) // pass 1 drops the retry-avoid rule if it left nothing
    {
        bool const useAvoid = pass == 0;
        std::vector<BotMgr::HuntSpawn const*> sameMap, otherMap, anyLevel;
        for (BotMgr::HuntSpawn const& s : info.huntSpawns)
        {
            if (useAvoid && avoided(s))
                continue;
            anyLevel.push_back(&s);
            if (s.maxLevel > level + GATHER_LEVEL_TOLERANCE)
                continue;
            (s.mapId == botMap ? sameMap : otherMap).push_back(&s);
        }

        if (!sameMap.empty())
        {
            std::size_t const n = std::min(HUNT_NEAREST_CANDIDATES, sameMap.size());
            std::partial_sort(sameMap.begin(), sameMap.begin() + n, sameMap.end(),
                [&](BotMgr::HuntSpawn const* a, BotMgr::HuntSpawn const* b) { return distSq(*a) < distSq(*b); });
            return pick(*sameMap[urand(0, uint32(n - 1))]);
        }
        if (!otherMap.empty())
            return pick(*otherMap[urand(0, uint32(otherMap.size() - 1))]);
        if (!anyLevel.empty())
        {
            std::sort(anyLevel.begin(), anyLevel.end(), [&](BotMgr::HuntSpawn const* a, BotMgr::HuntSpawn const* b)
            {
                if (a->maxLevel != b->maxLevel)
                    return a->maxLevel < b->maxLevel;
                bool const aSame = a->mapId == botMap, bSame = b->mapId == botMap;
                if (aSame != bSame)
                    return aSame;
                return aSame && distSq(*a) < distSq(*b);
            });
            return pick(*anyLevel.front());
        }
    }
    return false;
}

std::string GatherItemName(uint32 itemEntry)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
    return proto ? proto->Name1 : ("item " + std::to_string(itemEntry));
}

// Human-readable text for an ORDERFAIL reason token (the token itself goes on the addon wire).
char const* GatherFailReasonText(std::string const& reason)
{
    if (reason == "nosource")   return "no place to get it from was found";
    if (reason == "stuck")      return "it kept finding nothing to gather there";
    if (reason == "noprogress") return "it made no progress for 30 minutes";
    if (reason == "noskinning") return "it doesn't know Skinning well enough for that hide";
    if (reason == "leftguild")  return "it left the guild";
    if (reason == "nobot")      return "no guild bot is online and ungrouped";
    if (reason == "lowskill")   return "no free guild bot has a high enough gathering skill for it yet";
    if (reason == "noskill")    return "the bot could not learn the gathering profession it needs";
    return "it gave up";
}

bool CrafterHasReagentsFor(Player* crafter, SpellInfo const* spellInfo)
{
    for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
    {
        if (spellInfo->Reagent[i] <= 0 || !spellInfo->ReagentCount[i])
            continue;
        if (crafter->GetItemCount(uint32(spellInfo->Reagent[i]), false) < spellInfo->ReagentCount[i])
            return false;
    }
    return true;
}

// Mails a single already-in-inventory item stack from `sender` to `receiver`, real MailDraft
// path -- same sequence WorldSession::HandleSendMail uses (item removed from the sender's
// inventory/DB, ownership transferred, then attached to the draft), just without the gold
// cost or a client-authored subject/body. Works whether `receiverCharLowGuid` is online or
// not (MailReceiver's lowguid-only constructor doesn't require a live Player).
void MailCraftedItem(Player* sender, ObjectGuid::LowType receiverCharLowGuid, Item* item, std::string const& subject)
{
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    sender->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);
    item->DeleteFromInventoryDB(trans);
    if (item->GetState() == ITEM_UNCHANGED)
        item->FSetState(ITEM_CHANGED);
    item->SetOwnerGUID(ObjectGuid::Create<HighGuid::Player>(receiverCharLowGuid));
    item->SaveToDB(trans);

    MailDraft(subject, "")
        .AddItem(item)
        .SendMailTo(trans, MailReceiver(receiverCharLowGuid), MailSender(sender), MAIL_CHECK_MASK_COPIED);

    sender->SaveInventoryAndGoldToDB(trans);
    CharacterDatabase.CommitTransaction(trans);
}

struct ProfessionSkillEntry
{
    uint32 skillId;
    char const* name;
};

// The 11 standard WotLK profession skill lines (8 primary crafting/gathering + First
// Aid/Cooking/Fishing) -- stable, well-known skill line ids, not custom to this server.
constexpr ProfessionSkillEntry PROFESSION_SKILLS[] =
{
    { 164, "Blacksmithing" }, { 165, "Leatherworking" }, { 171, "Alchemy" },
    { 182, "Herbalism" },     { 186, "Mining" },         { 197, "Tailoring" },
    { 202, "Engineering" },   { 333, "Enchanting" },     { 393, "Skinning" },
    { 755, "Jewelcrafting" }, { 773, "Inscription" },
    { 129, "First Aid" },     { 185, "Cooking" },        { 356, "Fishing" },
    { 732, "Woodcutting" },   { 757, "Woodworking" },    // CoA's own trades (mod-woodworking)
};

}

// Same account-naming convention BotSpawnRandom.cpp's FindOrCreateBotAccount uses to create
// bot-hosting accounts ("<prefix>N") -- reused here as the one safety rail that stops
// RestoreGroupBotsOnLogin from ever auto-logging-in some other real player's alt just because
// it was left in a group with our commander. Case-insensitive since account names are stored
// upper-cased by AccountMgr::CreateAccount regardless of how the prefix is cased in config.
bool BotMgr::IsBotAccountId(uint32 accountId)
{
    // AccountMgr::GetName is a synchronous login-database query. The question is asked per player in per-tick loops (the encounter director asks it
    // twice for every online player every five seconds), and at 850 bots those queries alone took over half a second of the world thread. An account's
    // name does not change while the server runs, so the answer is kept; accounts that could not be resolved are not remembered.
    static std::mutex cacheMutex;
    static std::unordered_map<uint32, bool> cache;
    if (!accountId)
        return false;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto const it = cache.find(accountId);
        if (it != cache.end())
            return it->second;
    }

    std::string prefix = sConfigMgr->GetOption<std::string>("CoaBots.RandomSpawn.AccountPrefix", "CoaBotHost");
    std::string name;
    if (!AccountMgr::GetName(accountId, name))
        return false;
    bool const isBot = name.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), name.begin(), [](char a, char b)
    {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    std::lock_guard<std::mutex> lock(cacheMutex);
    cache[accountId] = isBot;
    return isBot;
}

BotMgr* BotMgr::instance()
{
    static BotMgr instance;
    return &instance;
}

WorldSession* BotMgr::FindBotSession(ObjectGuid::LowType charLowGuid) const
{
    auto itr = _botSessionByLowGuid.find(charLowGuid);
    if (itr != _botSessionByLowGuid.end())
    {
        WorldSession* session = itr->second;
        if (session && session->GetPlayer() && session->GetPlayer()->GetGUID().GetCounter() == charLowGuid)
            return session;
    }
    return nullptr;
}

namespace
{
bool ClampBotLevel(Player* bot)
{
    uint32 const cap = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
    if (!bot || !cap || bot->GetLevel() <= cap || bot->IsInCombat() || !bot->IsAlive())
        return false;
    bot->GiveLevel(static_cast<uint8>(cap));
    bot->SetUInt32Value(PLAYER_XP, 0);
    BotTalentBuilds::ApplyBuildForLevel(bot, bot->GetLevel());
    BotGear::Queue(bot);
    bot->SaveToDB(false, false);
    LOG_INFO("module.coa-playerbots", "BotMgr: clamped '{}' to server level cap {}.", bot->GetName(), cap);
    return true;
}
}

bool BotMgr::BotsEnabled()
{
    static bool enabled = true;
    static uint32 checkedAt = 0;
    uint32 const now = getMSTime();
    if (!checkedAt || getMSTimeDiff(checkedAt, now) >= 10000)
    {
        checkedAt = now ? now : 1;
        enabled = sConfigMgr->GetOption<bool>("CoaBots.Enable", true);
    }
    return enabled;
}

void BotMgr::SpawnBot(ObjectGuid::LowType charLowGuid, ChatHandler* handler, std::function<void(Player*)> onReady)
{
    if (!BotsEnabled())
    {
        if (handler)
            handler->SendSysMessage("BotMgr: companions are switched off (CoaBots.Enable = 0).");
        return;
    }

    ObjectGuid playerGuid = ObjectGuid::Create<HighGuid::Player>(charLowGuid);

    // Detached sessions do not pass through WorldSessionMgr's duplicate-login
    // protection. A second Player with this GUID overwrites ObjectAccessor while
    // FindBotPlayer still returns the first: native targeted casts then lose it.
    if (WorldSession* existing = FindBotSession(charLowGuid))
    {
        if (handler) handler->PSendSysMessage("BotMgr: guid {} is already logged in; reusing its companion session.", charLowGuid);
        if (onReady) onReady(existing->GetPlayer());
        return;
    }
    if (auto pending = _pendingBotLogins.find(charLowGuid); pending != _pendingBotLogins.end())
    {
        if (onReady) pending->second.push_back(std::move(onReady));
        if (handler) handler->PSendSysMessage("BotMgr: login for guid {} is already pending.", charLowGuid);
        return;
    }
    uint32 const benchmarkCap = sConfigMgr->GetOption<uint32>("CoaBots.Benchmark.MaxOnline", 0);
    if (benchmarkCap && _botSessions.size() >= benchmarkCap)
    {
        if (handler) handler->SendSysMessage("Benchmark population cap reached");
        return;
    }
    if (ObjectAccessor::FindPlayer(playerGuid))
    {
        if (handler) handler->PSendSysMessage("BotMgr: guid {} is already online outside the companion manager.", charLowGuid);
        return;
    }

    uint32 accountId = sCharacterCache->GetCharacterAccountIdByGuid(playerGuid);
    if (!accountId)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: no character with guid {} found in the character cache.", charLowGuid);
        return;
    }

    if (!BotSpawn::PrepareNameForLogin(charLowGuid, accountId))
    {
        LOG_ERROR("module.coa-playerbots", "BotMgr: could not prepare a valid login name for guid {} account {}", charLowGuid, accountId);
        return;
    }

    // Real WorldSession, real Player, but a null socket instead of a real
    // client connection — this is the core mechanism proven in pilot/. No
    // bot-aware constructor flag: CoA's WorldSession constructor is used
    // exactly as-is (see mod-coa-playerbots' core-diff-analysis.md for why
    // playerbots-fork's own constructor adds one and why this project
    // deliberately doesn't, yet).
    //
    // The last argument marks the session as a companion's. The core and the CoA modules key a lot of
    // per-client work off it - the client-side view refresh of Destiny Weaver (which otherwise walks every
    // player of the map for every creature update while any bot group changes), the account roster query
    // and collection state sent at login, the Welcome Warchest, the Path to Ascension and item-recovery
    // trackers, the stricter walkable-terrain filter in PathGenerator - none of which a companion has a client
    // for. Setting CoaBots.MarkSessionsAsBots = 0 restores the previous behaviour.
    bool const markAsBot = sConfigMgr->GetOption<bool>("CoaBots.MarkSessionsAsBots", true);
    WorldSession* botSession = new WorldSession(accountId, "", 0x0, nullptr, SEC_PLAYER,
        EXPANSION_WRATH_OF_THE_LICH_KING, time_t(0), sWorld->GetDefaultDbcLocale(), 0, false, false, 0, markAsBot);

    std::shared_ptr<LoginQueryHolder> holder = std::make_shared<LoginQueryHolder>(accountId, playerGuid);
    if (!holder->Initialize())
    {
        LOG_ERROR("module.coa-playerbots", "BotMgr: LoginQueryHolder::Initialize() failed for guid {}", charLowGuid);
        delete botSession;
        return;
    }

    _botSessions.push_back(botSession);
    _botSessionByLowGuid[charLowGuid] = botSession;
    auto& callbacks = _pendingBotLogins[charLowGuid];
    if (onReady) callbacks.push_back(std::move(onReady));

    // Deliberately sWorld->AddQueryHolderCallback, not botSession->AddQueryHolderCallback:
    // a detached, null-socket session is never registered with WorldSessionMgr, so its own
    // (private, World-friend-only) ProcessQueryCallbacks() never gets driven by anything.
    // World::ProcessQueryCallbacks() runs unconditionally every tick regardless of session
    // registration, so callbacks queued on the World-level processor still complete. See
    // pilot/README.md for the full story of how this was found.
    sWorld->AddQueryHolderCallback(CharacterDatabase.DelayQueryHolder(holder)).AfterComplete(
        [this, botSession, charLowGuid](SQLQueryHolderBase const& completedHolder)
        {
            botSession->HandlePlayerLoginFromDB(static_cast<LoginQueryHolder const&>(completedHolder));
            auto callbacks = std::move(_pendingBotLogins[charLowGuid]);
            _pendingBotLogins.erase(charLowGuid);

            if (Player* bot = botSession->GetPlayer())
            {
                ClampBotLevel(bot);
                LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: bot '{}' ({}) logged in successfully.",
                    bot->GetName(), bot->GetGUID().ToString());

                // Ensure all newly spawned bots relocate to appropriate zones (fixes bots clustering at spawn points).
                // This is called for all spawn paths; if onReady also relocates (e.g., ApplyFreshBotSetup),
                // calling it twice is safe and idempotent.
                bool const readback = sConfigMgr->GetOption<bool>("CoaBots.Benchmark.SuspendOnLogin", false);
                if (readback)
                {
                    bot->SetGameMaster(true);
                    bot->SetGMVisible(false);
                    BotAI::SetSuspended(bot->GetGUID(), true);
                }
                else if (!sConfigMgr->GetOption<bool>("CoaBots.Benchmark.PreserveLoginPosition", false)
                    && !IsProtectedBot(bot))
                    BotZoneProgression::RelocateBot(bot, true /*force initial relocation*/);

                for (auto& onReady : callbacks)
                    if (onReady) onReady(bot);
            }
            else
            {
                LOG_ERROR("module.coa-playerbots", "BotMgr: bot login failed for guid {} account {}, Player is null after HandlePlayerLoginFromDB.", charLowGuid, botSession->GetAccountId());
            }
        });

    if (handler)
        handler->PSendSysMessage("BotMgr: login query issued for guid {}, watch the server log for the result.", charLowGuid);
}

namespace
{
    // Confirmed live (2026-09-27): a bot standing right next to its leader -- same map, same
    // instance, a few yards apart -- still got hard-teleported on every invite accept and every
    // follow-across-maps tick, exactly the "poltergeist" symptom the wall-clip investigation
    // traced back to a bad landing spot. A real player who's already there just walks over; only
    // a genuinely different map/instance (can't walk there at all) or a real gap on the same map
    // (fell behind, needs more than a plain MoveFollow tick to close) justifies a teleport.
    constexpr float LEADER_TELEPORT_DISTANCE_YD = 100.0f;

    bool NeedsTeleportToLeader(Player const* bot, Player const* leader)
    {
        if (bot->GetMapId() != leader->GetMapId() || bot->GetInstanceId() != leader->GetInstanceId())
            return true; // different map/instance: no walkable route exists at all, must teleport

        // Inside one dungeon instance the leader is reachable on foot, however far ahead: a player who died and came back at the
        // start runs to the group, and so does a bot.
        if (Map const* map = bot->GetMap(); map && (map->IsDungeon() || map->IsRaid()))
            return false;

        return bot->GetDistance(leader) > LEADER_TELEPORT_DISTANCE_YD;
    }
}

void BotMgr::DoAcceptInvite(WorldSession* session)
{
    // HandleGroupAcceptOpcode only does recvData.read_skip<uint32>() before the real logic
    // (RemoveInvite, validation, Create-if-new, AddMember, BroadcastGroupUpdate) — calling it
    // directly with a minimal padding packet reuses that real logic verbatim instead of
    // duplicating it by hand. Same trick pilot/ already used for login.
    WorldPacket fakePacket;
    fakePacket << uint32(0);
    session->HandleGroupAcceptOpcode(fakePacket);

    Player* bot = session->GetPlayer();
    if (!bot)
    {
        LOG_ERROR(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: DoAcceptInvite: session has no Player after HandleGroupAcceptOpcode.");
        return;
    }

    Group* group = bot->GetGroup();
    if (!group)
    {
        LOG_ERROR(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: DoAcceptInvite: bot '{}' has no group after HandleGroupAcceptOpcode -- AddMember must have failed or returned early.", bot->GetName());
        return;
    }

    LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: bot '{}' is now in a group, leader guid {}.", bot->GetName(), group->GetLeaderGUID().ToString());

    if (Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID()))
    {
        LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: resolved leader '{}' (in world: {}).", leader->GetName(), leader->IsInWorld());
        if (leader != bot && NeedsTeleportToLeader(bot, leader))
        {
            LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: teleporting bot '{}' to group leader '{}'.",
                bot->GetName(), leader->GetName());
            bot->TeleportTo(leader->GetWorldLocation());

            // Don't fire the ack here -- see FinishPendingTeleport's comment in the
            // header for why doing it in the same tick as TeleportTo() crashes.
            _pendingTeleportAck.push_back(session);
        }
        // Already close enough on the same map: no teleport needed at all -- the bot's normal
        // ResumeFollowingLeader tick (BotAI.cpp) picks up the freshly-joined group and MoveFollows
        // the leader like any other out-of-combat bot, walking over instead of popping in place.
    }
    else
    {
        LOG_ERROR("module.coa-playerbots", "BotMgr: DoAcceptInvite: ObjectAccessor::FindPlayer could not resolve leader guid {}.",
            group->GetLeaderGUID().ToString());
    }
}

void BotMgr::EnsureBotBankRights(Player* bot, Guild* guild)
{
    if (!guild || !bot)
        return;

    guild->EnsureFirstBankTab();
    guild->EnsureRankBankRights(bot->GetRank(),
        GUILD_BANK_RIGHT_DEPOSIT_ITEM | GUILD_BANK_RIGHT_VIEW_TAB,
        50,
        50 * GOLD);
}

void BotMgr::DoAcceptGuildInvite(WorldSession* session)
{
    Player* bot = session->GetPlayer();
    if (!bot)
    {
        LOG_ERROR(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: DoAcceptGuildInvite: session has no Player.");
        return;
    }

    uint32 invitedGuildId = bot->GetGuildIdInvited();
    if (!invitedGuildId)
    {
        LOG_WARN(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: DoAcceptGuildInvite: bot '{}' has no pending guild invite.", bot->GetName());
        return;
    }

    WorldPacket packet(CMSG_GUILD_ACCEPT, 0);
    WorldPackets::Guild::AcceptGuildInvite acceptPacket(std::move(packet));
    session->HandleGuildAcceptOpcode(acceptPacket);

    // If accept failed (e.g. guild disbanded, cross-faction disallowed), clear the invite
    // so the auto-accept loop doesn't spin forever.
    if (bot->GetGuildIdInvited() == invitedGuildId && !bot->GetGuildId())
    {
        LOG_WARN(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: DoAcceptGuildInvite: failed to join guild {} for bot '{}', clearing invite.",
            invitedGuildId, bot->GetName());
        bot->SetGuildIdInvited(0);
    }
    else
    {
        LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: bot '{}' successfully joined guild '{}' (id {}).",
            bot->GetName(), bot->GetGuildName(), bot->GetGuildId());
        if (Guild* guild = bot->GetGuild())
            EnsureBotBankRights(bot, guild);
    }
}

void BotMgr::FinishPendingTeleport(WorldSession* session)
{
    Player* bot = session->GetPlayer();
    if (!bot)
        return;

    // HandleMoveWorldportAck() already has a no-packet "for server-side calls"
    // overload; the near case needs a minimal packed-guid packet built the same
    // way as every other "call the real handler directly" trick this module uses.
    if (bot->IsBeingTeleportedNear())
    {
        WorldPacket ackPacket;
        ackPacket << bot->GetGUID().WriteAsPacked();
        ackPacket << uint32(0); // flags, unused by the handler
        ackPacket << uint32(0); // time, unused by the handler
        session->HandleMoveTeleportAck(ackPacket);
    }
    else if (bot->IsBeingTeleportedFar())
    {
        session->HandleMoveWorldportAck();
    }
    else
    {
        // Not (or no longer) mid-teleport -- nothing to finish.
        return;
    }

    LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: finished pending teleport for bot '{}'.", bot->GetName());

    // Bots have no client to simulate gravity or emit falling/landing packets.
    // Snap the bot's Z to walkable terrain/water surface and broadcast the corrected position
    // to nearby observing clients so the bot doesn't render frozen/floating mid-air.
    if (!bot->CanFly())
    {
        float z = bot->GetPositionZ();
        bot->UpdateAllowedPositionZ(bot->GetPositionX(), bot->GetPositionY(), z);
        bot->UpdatePosition(bot->GetPositionX(), bot->GetPositionY(), z, bot->GetOrientation());
    }

    // Start following only once the teleport has actually landed -- doing this
    // before the ack would give MoveFollow a stale/pre-teleport position to work from.
    if (Group* group = bot->GetGroup())
    {
        if (Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID()))
        {
            if (leader != bot)
                BotMovement::Follow(bot, MoveOwner::Travel, leader, BotAI::ComputeFollowDistance(bot), BotAI::ComputeFollowAngle(bot));
        }
    }
}

void BotMgr::TryReturnGhostToCorpseMap(WorldSession* session)
{
    Player* bot = session->GetPlayer();
    if (!bot || CoaTraining::Owns(bot->GetGUID()) || !bot->IsInWorld() || bot->IsAlive())
        return;

    if (bot->IsBeingTeleportedNear() || bot->IsBeingTeleportedFar())
        return;

    // BG ghosts are meant to stay at the graveyard they were sent to -- see
    // BotAI::UpdateDeathHandling's comment on why corpse-running is deliberately skipped there.
    if (!bot->HasPlayerFlag(PLAYER_FLAGS_GHOST) || bot->InBattleground())
        return;

    Corpse* corpse = bot->GetCorpse();
    if (!corpse || corpse->GetMapId() == bot->GetMapId())
        return;

    LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: ghost '{}' is on map {} but its corpse is on map {} -- teleporting to the corpse's map.",
        bot->GetName(), bot->GetMapId(), corpse->GetMapId());

    bot->TeleportTo(corpse->GetMapId(), corpse->GetPositionX(), corpse->GetPositionY(), corpse->GetPositionZ(), bot->GetOrientation());
    _pendingTeleportAck.push_back(session);
}

void BotMgr::QueueTeleportAck(WorldSession* session)
{
    _pendingTeleportAck.push_back(session);
}

void BotMgr::QueueBotGroupLeave(ObjectGuid botGuid)
{
    _pendingGroupLeaves.push_back(botGuid);
}

Player* BotMgr::FindBotPlayer(ObjectGuid::LowType charLowGuid) const
{
    WorldSession* session = FindBotSession(charLowGuid);
    return session ? session->GetPlayer() : nullptr;
}

bool BotMgr::IsProtectedBot(ObjectGuid::LowType charLowGuid) const
{
    if (!charLowGuid)
        return true;

    BotProtectionPolicy::ProtectionFacts facts;

    ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(charLowGuid);
    if (Player* onlineBot = FindBotPlayer(charLowGuid))
    {
        facts.inGuild = onlineBot->GetGuildId() != 0;
        facts.hasGuildInvite = onlineBot->GetGuildIdInvited() != 0;
        facts.inOnlineGroupOrRaid = onlineBot->GetGroup() != nullptr;
    }

    facts.hasActiveOrder = (_guildGatherOrders.find(guid) != _guildGatherOrders.end())
        || (_craftOrders.find(guid) != _craftOrders.end())
        || (!BotEconomy::GoldOrderTask(const_cast<Player*>(FindBotPlayer(charLowGuid))).empty());

    if (BotProtectionPolicy::IsProtected(facts))
        return true;

    if (QueryResult guildRes = CharacterDatabase.Query("SELECT 1 FROM guild_member WHERE guid = {} LIMIT 1", charLowGuid))
        facts.inGuild = true;

    if (!facts.inGuild)
    {
        if (QueryResult groupRes = CharacterDatabase.Query("SELECT 1 FROM group_member WHERE memberGuid = {} LIMIT 1", charLowGuid))
            facts.inPersistedGroupOrRaid = true;
    }

    if (!facts.inGuild && !facts.inPersistedGroupOrRaid)
    {
        if (QueryResult friendRes = CharacterDatabase.Query(
            "SELECT DISTINCT c.account FROM character_social cs "
            "JOIN characters c ON c.guid = cs.guid "
            "WHERE cs.friend = {} AND (cs.flags & 1) <> 0", charLowGuid))
        {
            std::string accountPrefix = sConfigMgr->GetOption<std::string>("CoaBots.RandomSpawn.AccountPrefix", "CoaBotHost");
            do
            {
                uint32 ownerAccountId = (*friendRes)[0].Get<uint32>();
                QueryResult botAcct = LoginDatabase.Query(
                    "SELECT 1 FROM account WHERE id = {} AND id > 2 AND username LIKE '{}%' LIMIT 1",
                    ownerAccountId, accountPrefix);
                if (!botAcct)
                {
                    facts.hasIncomingPlayerFriend = true;
                    break;
                }
            } while (friendRes->NextRow());
        }
    }

    return BotProtectionPolicy::IsProtected(facts);
}

bool BotMgr::IsProtectedBot(Player const* bot) const
{
    if (!bot)
        return true;
    return IsProtectedBot(bot->GetGUID().GetCounter());
}

ObjectGuid::LowType BotMgr::FindEligibleOfflineBot(TeamId team, uint8 minLevel, uint8 maxLevel,
    std::unordered_set<ObjectGuid::LowType> const& excludedGuids) const
{
    std::string accountPrefix = sConfigMgr->GetOption<std::string>("CoaBots.RandomSpawn.AccountPrefix", "CoaBotHost");
    QueryResult accounts = LoginDatabase.Query("SELECT id FROM account WHERE id > 2 AND username LIKE '{}%'", accountPrefix);
    if (!accounts)
        return 0;

    std::ostringstream accountIds;
    do
    {
        accountIds << (*accounts)[0].Get<uint32>() << ",";
    } while (accounts->NextRow());
    std::string idList = accountIds.str();
    idList.pop_back();

    std::string query = Acore::StringFormat(
        "SELECT guid, race, level FROM characters c WHERE c.account IN ({}) AND c.level BETWEEN {} AND {} "
        "AND NOT EXISTS (SELECT 1 FROM guild_member gm WHERE gm.guid = c.guid) "
        "AND NOT EXISTS (SELECT 1 FROM group_member grm WHERE grm.memberGuid = c.guid)",
        idList, uint32(minLevel), uint32(maxLevel));
    QueryResult chars = CharacterDatabase.Query(query);
    if (!chars)
        return 0;

    std::vector<ObjectGuid::LowType> candidates;
    do
    {
        Field* fields = chars->Fetch();
        ObjectGuid::LowType guid = fields[0].Get<uint32>();
        uint8 race = fields[1].Get<uint8>();

        if (team != TEAM_NEUTRAL && Player::TeamIdForRace(race) != team)
            continue;

        if (IsCustomRace(race) && CustomRacesOff())
            continue;

        if (excludedGuids.count(guid) || FindBotSession(guid))
            continue;

        if (IsProtectedBot(guid))
            continue;

        candidates.push_back(guid);
    } while (chars->NextRow());

    if (candidates.empty())
        return 0;

    return candidates[urand(0, uint32(candidates.size()) - 1)];
}

std::vector<Player*> BotMgr::GetOnlineBots() const
{
    std::vector<Player*> bots;
    bots.reserve(_botSessions.size());
    for (WorldSession* session : _botSessions)
        if (Player* bot = session->GetPlayer())
            bots.push_back(bot);
    return bots;
}

void BotMgr::TryFollowLeaderAcrossMaps(WorldSession* session)
{
    Player* bot = session->GetPlayer();
    if (!bot || CoaTraining::Owns(bot->GetGUID()) || !bot->IsInWorld())
        return;

    // Already mid-teleport (this tick's own queue below, or something else entirely) --
    // let FinishPendingTeleport settle it first rather than stacking a second request.
    if (bot->IsBeingTeleportedNear() || bot->IsBeingTeleportedFar())
        return;

    // A ghost runs back to its corpse (and TryReturnGhostToCorpseMap covers a corpse on another map); pulling it
    // to the leader would cancel that run every time the group moved on, and it would never get there.
    if (!bot->IsAlive())
        return;

    Group* group = bot->GetGroup();
    if (!group)
        return;

    Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID());
    if (!leader || leader == bot || !leader->IsInWorld())
        return;

    // A leader who is dead or running back as a ghost is not followed across maps: the ghost is at the graveyard, the group stays where it fell.
    if (!leader->IsAlive() || leader->HasPlayerFlag(PLAYER_FLAGS_GHOST))
        return;

    // Inside a battleground or arena the battleground AI plays the bot (BotBattlegroundAI). Teleporting it to the group leader on top of that, as this
    // did once the bot was further than 100 yards from the leader, made the two fight: the bot was pulled back to the leader every few seconds. A
    // bot and a leader in different battlegrounds, or one in and one out, cannot be brought together by a plain teleport anyway.
    if (bot->InBattleground() || leader->InBattleground())
        return;

    if (!NeedsTeleportToLeader(bot, leader))
        return; // same map, close enough -- ResumeFollowingLeader's plain MoveFollow closes this

    LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: leader '{}' is on map {} (instance {}), bot '{}' is on map {} (instance {}), {:.0f}yd away -- teleporting bot to leader.",
        leader->GetName(), leader->GetMapId(), leader->GetInstanceId(), bot->GetName(), bot->GetMapId(), bot->GetInstanceId(),
        bot->GetMapId() == leader->GetMapId() && bot->GetInstanceId() == leader->GetInstanceId() ? bot->GetDistance(leader) : 0.0f);

    bot->TeleportTo(leader->GetWorldLocation());
    _pendingTeleportAck.push_back(session);
}

void BotMgr::AcceptInvite(ObjectGuid::LowType charLowGuid, ChatHandler* handler)
{
    WorldSession* session = FindBotSession(charLowGuid);
    if (!session)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: no active bot session for guid {} (spawn it first).", charLowGuid);
        return;
    }

    Player* bot = session->GetPlayer();
    if (!bot)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: bot session for guid {} has no Player yet (login still pending?).", charLowGuid);
        return;
    }

    if (!bot->GetGroupInvite())
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: bot '{}' has no pending group invite — invite it first from a real client.", bot->GetName());
        return;
    }

    DoAcceptInvite(session);

    if (handler)
        handler->PSendSysMessage("BotMgr: accept-invite issued for bot '{}', check .group list to confirm.", bot->GetName());
}

void BotMgr::AcceptGuildInvite(ObjectGuid::LowType charLowGuid, ChatHandler* handler)
{
    WorldSession* session = FindBotSession(charLowGuid);
    if (!session)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: no active bot session for guid {} (spawn it first).", charLowGuid);
        return;
    }

    Player* bot = session->GetPlayer();
    if (!bot)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: bot session for guid {} has no Player yet (login still pending?).", charLowGuid);
        return;
    }

    if (bot->GetGuildId() != 0)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: bot '{}' is already in a guild (id {}, '{}').",
                bot->GetName(), bot->GetGuildId(), bot->GetGuildName());
        return;
    }

    if (!bot->GetGuildIdInvited())
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: bot '{}' has no pending guild invite — invite it first.", bot->GetName());
        return;
    }

    DoAcceptGuildInvite(session);

    if (handler)
    {
        if (bot->GetGuildId() != 0)
            handler->PSendSysMessage("BotMgr: accept-guild-invite succeeded for bot '{}', joined guild '{}' (id {}).",
                bot->GetName(), bot->GetGuildName(), bot->GetGuildId());
        else
            handler->PSendSysMessage("BotMgr: accept-guild-invite failed for bot '{}'.", bot->GetName());
    }
}

void BotMgr::GuildInvite(ObjectGuid::LowType charLowGuid, std::string const& targetName, ChatHandler* handler)
{
    WorldSession* session = FindBotSession(charLowGuid);
    if (!session)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: no active bot session for guid {} (spawn it first).", charLowGuid);
        return;
    }

    Player* bot = session->GetPlayer();
    if (!bot)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: bot session for guid {} has no Player yet (login still pending?).", charLowGuid);
        return;
    }

    Guild* guild = bot->GetGuild();
    if (!guild)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: bot '{}' is not in a guild.", bot->GetName());
        return;
    }

    guild->HandleInviteMember(session, targetName);

    if (handler)
        handler->PSendSysMessage("BotMgr: guild invite for '{}' issued by '{}' (guild '{}').",
            targetName, bot->GetName(), guild->GetName());
}

void BotMgr::GuildCreate(ObjectGuid::LowType charLowGuid, std::string const& guildName, ChatHandler* handler)
{
    WorldSession* session = FindBotSession(charLowGuid);
    if (!session)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: no active bot session for guid {} (spawn it first).", charLowGuid);
        return;
    }

    Player* bot = session->GetPlayer();
    if (!bot)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: bot session for guid {} has no Player yet.", charLowGuid);
        return;
    }

    if (bot->GetGuildId())
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: bot '{}' is already in guild '{}' (id {}).",
                bot->GetName(), bot->GetGuildName(), bot->GetGuildId());
        return;
    }

    if (guildName.empty())
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: guild name cannot be empty.");
        return;
    }

    if (sGuildMgr->GetGuildByName(guildName))
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: guild with name '{}' already exists.", guildName);
        return;
    }

    Guild* guild = new Guild;
    if (!guild->Create(bot, guildName))
    {
        delete guild;
        if (handler)
            handler->PSendSysMessage("BotMgr: failed to create guild '{}' for bot '{}'.", guildName, bot->GetName());
        return;
    }

    sGuildMgr->AddGuild(guild);
    EnsureBotBankRights(bot, guild);

    if (handler)
        handler->PSendSysMessage("BotMgr: bot '{}' created guild '{}' (id {}).",
            bot->GetName(), guild->GetName(), guild->GetId());
    LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: bot '{}' created guild '{}' (id {}).",
        bot->GetName(), guild->GetName(), guild->GetId());
}

uint32 BotMgr::GuildDepositItem(ObjectGuid::LowType charLowGuid, uint32 itemEntry, uint32 count, ChatHandler* handler)
{
    WorldSession* session = FindBotSession(charLowGuid);
    if (!session)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: no active bot session for guid {}.", charLowGuid);
        return 0;
    }

    Player* bot = session->GetPlayer();
    if (!bot)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: bot session for guid {} has no Player yet.", charLowGuid);
        return 0;
    }

    // A bot keeps its profession tools, whoever asks: a guild gather order or an explicit
    // `.botcmd guilddeposit` must never empty its bags of the pick or knife it gathers with.
    if (BotAI::IsProfessionTool(sObjectMgr->GetItemTemplate(itemEntry)))
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: bot '{}' keeps its profession tools; item {} was not deposited.",
                bot->GetName(), itemEntry);
        return 0;
    }

    Guild* guild = bot->GetGuild();
    if (!guild)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: bot '{}' is not in a guild.", bot->GetName());
        return 0;
    }

    EnsureBotBankRights(bot, guild);

    if (guild->GetPurchasedTabsSize() == 0)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: guild '{}' has no bank tabs.", guild->GetName());
        return 0;
    }

    uint32 totalDeposited = 0;
    uint32 targetToDeposit = (count == 0) ? 0xFFFFFFFF : count;

    while (totalDeposited < targetToDeposit)
    {
        Item* matchingItem = nullptr;
        uint8 foundBag = NULL_BAG;
        uint8 foundSlot = NULL_SLOT;

        // Search backpack
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            if (Item* it = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                if (it->GetEntry() == itemEntry)
                {
                    matchingItem = it;
                    foundBag = INVENTORY_SLOT_BAG_0;
                    foundSlot = slot;
                    break;
                }
            }
        }

        // Search bags
        if (!matchingItem)
        {
            for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            {
                if (Bag* bag = bot->GetBagByPos(bagSlot))
                {
                    for (uint8 slot = 0; slot < bag->GetBagSize(); ++slot)
                    {
                        if (Item* it = bag->GetItemByPos(slot))
                        {
                            if (it->GetEntry() == itemEntry)
                            {
                                matchingItem = it;
                                foundBag = bagSlot;
                                foundSlot = slot;
                                break;
                            }
                        }
                    }
                    if (matchingItem)
                        break;
                }
            }
        }

        if (!matchingItem)
            break;

        uint32 inStack = matchingItem->GetCount();
        ObjectGuid matchingItemGuid = matchingItem->GetGUID();
        uint32 remainingNeeded = targetToDeposit - totalDeposited;
        uint32 moveAmount = (remainingNeeded < inStack) ? remainingNeeded : 0; // 0 = entire stack
        uint32 actualMoved = (moveAmount > 0) ? moveAmount : inStack;

        // Try tab 0 first (merges into an existing stack there if one exists), then each
        // further purchased tab in order -- a multi-tab guild bank must not leave everything
        // after tab 0 untouched just because tab 0 personally happens to be full. Stops at the
        // first tab that actually accepts it.
        bool moved = false;
        for (uint8 tabId = 0; tabId < guild->GetPurchasedTabsSize(); ++tabId)
        {
            guild->SwapItemsWithInventory(bot, false, tabId, NULL_SLOT, foundBag, foundSlot, moveAmount);

            // SwapItemsWithInventory returns void and silently does nothing if this tab has no
            // room left (full, or every free slot already holds something else) -- without this
            // check the exact same stack gets "found" again next iteration and totalDeposited
            // keeps climbing while nothing actually moves, looping until it wraps a uint32
            // (targetToDeposit is 0xFFFFFFFF for a "deposit everything" call). Confirmed live
            // 2026-09-29: banking every resource type across 21 bots eventually filled the
            // guild's one tab and the next deposit call hung the entire world thread for good.
            Item* stillThere = (foundBag == INVENTORY_SLOT_BAG_0)
                ? bot->GetItemByPos(INVENTORY_SLOT_BAG_0, foundSlot)
                : (bot->GetBagByPos(foundBag) ? bot->GetBagByPos(foundBag)->GetItemByPos(foundSlot) : nullptr);
            if (!(stillThere && stillThere->GetGUID() == matchingItemGuid && stillThere->GetCount() == inStack))
            {
                moved = true;
                break;
            }
            // This tab had no room for it -- fall through and try the next one.
        }

        // No tab in the whole bank had room for this stack: stop instead of retrying it forever.
        if (!moved)
            break;

        totalDeposited += actualMoved;
    }

    if (handler)
    {
        if (totalDeposited > 0)
            handler->PSendSysMessage("BotMgr: bot '{}' deposited {}x item {} into guild bank.",
                bot->GetName(), totalDeposited, itemEntry);
        else
            handler->PSendSysMessage("BotMgr: bot '{}' has no item {} in inventory to deposit.",
                bot->GetName(), itemEntry);
    }
    LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: bot '{}' deposited {}x item {} into guild bank (guild '{}').",
        bot->GetName(), totalDeposited, itemEntry, guild->GetName());

    return totalDeposited;
}

std::unordered_map<uint32, uint32> BotMgr::GuildBankTradeGoods(uint32 guildId) const
{
    std::unordered_map<uint32, uint32> counts;
    Guild* guild = sGuildMgr->GetGuildById(guildId);
    if (!guild)
        return counts;
    for (uint8 tabId = 0; tabId < guild->GetPurchasedTabsSize(); ++tabId)
    {
        for (uint8 slotId = 0; slotId < GUILD_BANK_MAX_SLOTS; ++slotId)
            if (Item const* item = guild->GetBankItem(tabId, slotId))
                if (ItemTemplate const* proto = item->GetTemplate(); proto && proto->Class == ITEM_CLASS_TRADE_GOODS)
                    counts[item->GetEntry()] += item->GetCount();
    }
    return counts;
}

uint32 BotMgr::GuildWithdrawItem(ObjectGuid::LowType charLowGuid, uint32 itemEntry, uint32 count, ChatHandler* handler)
{
    WorldSession* session = FindBotSession(charLowGuid);
    if (!session)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: no active bot session for guid {}.", charLowGuid);
        return 0;
    }

    Player* bot = session->GetPlayer();
    if (!bot)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: bot session for guid {} has no Player yet.", charLowGuid);
        return 0;
    }

    Guild* guild = bot->GetGuild();
    if (!guild)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: bot '{}' is not in a guild.", bot->GetName());
        return 0;
    }

    EnsureBotBankRights(bot, guild);

    uint32 totalWithdrawn = 0;
    uint32 targetToWithdraw = (count == 0) ? 1 : count;

    for (uint8 tabId = 0; tabId < guild->GetPurchasedTabsSize() && totalWithdrawn < targetToWithdraw; ++tabId)
    {
        for (uint8 slotId = 0; slotId < GUILD_BANK_MAX_SLOTS && totalWithdrawn < targetToWithdraw; ++slotId)
        {
            Item const* bankItem = guild->GetBankItem(tabId, slotId);
            if (!bankItem || bankItem->GetEntry() != itemEntry)
                continue;

            uint32 inBank = bankItem->GetCount();
            uint32 remainingNeeded = targetToWithdraw - totalWithdrawn;
            uint32 moveAmount = (remainingNeeded < inBank) ? remainingNeeded : 0; // 0 = entire stack
            uint32 actualMoved = (moveAmount > 0) ? moveAmount : inBank;

            guild->SwapItemsWithInventory(bot, true, tabId, slotId, NULL_BAG, NULL_SLOT, moveAmount);
            totalWithdrawn += actualMoved;
        }
    }

    if (handler)
    {
        if (totalWithdrawn > 0)
            handler->PSendSysMessage("BotMgr: bot '{}' withdrew {}x item {} from guild bank.",
                bot->GetName(), totalWithdrawn, itemEntry);
        else
            handler->PSendSysMessage("BotMgr: item {} not found in guild bank (or no withdraw rights).", itemEntry);
    }
    LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: bot '{}' withdrew {}x item {} from guild bank (guild '{}').",
        bot->GetName(), totalWithdrawn, itemEntry, guild->GetName());

    return totalWithdrawn;
}

void BotMgr::GuildDepositMoney(ObjectGuid::LowType charLowGuid, uint32 copper, ChatHandler* handler)
{
    WorldSession* session = FindBotSession(charLowGuid);
    if (!session)
        return;
    Player* bot = session->GetPlayer();
    if (!bot)
        return;
    Guild* guild = bot->GetGuild();
    if (!guild)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: bot '{}' is not in a guild.", bot->GetName());
        return;
    }

    if (bot->GetMoney() < copper)
        copper = bot->GetMoney();

    if (copper == 0)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: bot '{}' has no money to deposit.", bot->GetName());
        return;
    }

    guild->HandleMemberDepositMoney(session, copper);
    if (handler)
        handler->PSendSysMessage("BotMgr: bot '{}' deposited {} copper into guild bank.", bot->GetName(), copper);
}

void BotMgr::GuildWithdrawMoney(ObjectGuid::LowType charLowGuid, uint32 copper, ChatHandler* handler)
{
    WorldSession* session = FindBotSession(charLowGuid);
    if (!session)
        return;
    Player* bot = session->GetPlayer();
    if (!bot)
        return;
    Guild* guild = bot->GetGuild();
    if (!guild)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: bot '{}' is not in a guild.", bot->GetName());
        return;
    }

    EnsureBotBankRights(bot, guild);

    bool ok = guild->HandleMemberWithdrawMoney(session, copper);
    if (handler)
    {
        if (ok)
            handler->PSendSysMessage("BotMgr: bot '{}' withdrew {} copper from guild bank.", bot->GetName(), copper);
        else
            handler->PSendSysMessage("BotMgr: failed to withdraw {} copper (not enough funds or limit reached).", copper);
    }
}

void BotMgr::DepositLooseResourcesToGuildBank(Player* bot)
{
    if (!bot || !bot->GetGuild())
        return;

    std::unordered_set<uint32> resourceEntries;
    auto collect = [&](Item* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (proto && proto->Class == ITEM_CLASS_TRADE_GOODS && !BotEconomy::IsReservedReagent(bot, proto->ItemId))
            resourceEntries.insert(proto->ItemId);
    };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        collect(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = bot->GetBagByPos(bagSlot))
            for (uint8 slot = 0; slot < bag->GetBagSize(); ++slot)
                collect(bag->GetItemByPos(slot));

    // GuildDepositItem already refuses profession tools on its own; every entry collected above is
    // Trade Goods, never a tool, so no extra filtering needed here.
    for (uint32 itemEntry : resourceEntries)
        GuildDepositItem(bot->GetGUID().GetCounter(), itemEntry, 0, nullptr);
}

void BotMgr::GuildGather(ObjectGuid::LowType charLowGuid, uint32 itemEntry, uint32 targetCount, ChatHandler* handler,
    ObjectGuid::LowType requesterLowGuid, bool mailToRequester)
{
    WorldSession* session = FindBotSession(charLowGuid);
    if (!session)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: no active bot session for guid {}.", charLowGuid);
        return;
    }
    Player* bot = session->GetPlayer();
    if (!bot)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: bot session for guid {} has no Player yet.", charLowGuid);
        return;
    }
    Guild* guild = bot->GetGuild();
    if (!guild && !mailToRequester)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: bot '{}' is not in a guild.", bot->GetName());
        return;
    }

    // A GM placing the order through .botcmd is its requester too (notices, cancel rights).
    if (!requesterLowGuid && handler && handler->GetSession() && handler->GetSession()->GetPlayer())
        requesterLowGuid = handler->GetSession()->GetPlayer()->GetGUID().GetCounter();

    if (mailToRequester && (!requesterLowGuid || FindBotSession(requesterLowGuid)))
    {
        if (handler)
            handler->SendSysMessage("BotMgr: a mailed gather order needs a player to mail the items to.");
        return;
    }

    if (guild)
        EnsureBotBankRights(bot, guild);

    // Hand out a task with clean bags: bank whatever raw materials it's already carrying (worth
    // more in the guild bank than sold for coppers or just sitting there) and clear out grey
    // vendor trash / gear this bot has already outgrown. Without this, a bot that had been
    // leveling unattended for a long time could have zero room left for the very thing it's about
    // to be sent to fetch -- confirmed live 2026-09-29 across several of 21 bots.
    if (!mailToRequester)
        DepositLooseResourcesToGuildBank(bot);
    BotAI::PrepareBagsForTask(bot);

    GuildGatherOrder order;
    order.itemEntry = itemEntry;
    order.targetCount = targetCount;
    order.requesterLowGuid = requesterLowGuid;
    order.mailToRequester = mailToRequester;

    // 1. Immediately hand over any matching items already held in inventory
    uint32 deposited = mailToRequester ? MailGatherItems(bot, requesterLowGuid, itemEntry, targetCount, OrderMailBody(bot))
                                       : GuildDepositItem(charLowGuid, itemEntry, targetCount, nullptr);

    if (deposited >= targetCount)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: guildgather complete -- bot '{}' deposited all {}x item {} immediately from inventory.",
                bot->GetName(), deposited, itemEntry);
        _guildGatherOrders.erase(bot->GetGUID());
        DeleteGuildGatherOrder(bot->GetGUID());
        order.gatheredCount = deposited;
        NotifyGatherOrderDone(bot, order, mailToRequester ? deposited : 0);
        return;
    }

    uint32 remaining = targetCount - deposited;
    order.gatheredCount = deposited;
    order.remainingCount = remaining;
    order.lastProgressMs = getMSTime();

    GatherSourceInfo const& info = GetGatherSourceInfo(itemEntry);
    std::string const itemName = GatherItemName(itemEntry);
    Player* requesterPlayer = requesterLowGuid && !FindBotSession(requesterLowGuid)
        ? ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(requesterLowGuid)) : nullptr;
    auto tellRequester = [&](std::string const& text)
    {
        if (requesterPlayer && requesterPlayer->GetSession() && (!handler || handler->GetSession() != requesterPlayer->GetSession()))
            ChatHandler(requesterPlayer->GetSession()).PSendSysMessage("|cff33ff99Guild order:|r {}", text);
        if (handler)
            handler->PSendSysMessage("BotMgr: {}", text);
    };

    // The tools first: every Mining cast fails without a Mining Pick in the bags and every
    // Skinning cast without a Skinning Knife -- an order would then "search" forever while each
    // node it found was skipped as a failed cast. GrantProfessionTools only adds what is missing.
    BotProgression::GrantProfessionTools(bot);

    // A gathering skill the bot does not have (or has too low) means the node search can never
    // open a single node: PickGatherBot only *prefers* a bot with the skill, so when no free bot
    // has it the order used to go to one that could only stand at the spot until the order timed
    // out. This realm lets bots learn every profession, so teach the one the order needs; if the
    // bot's level still caps it below what the item's nodes need, say so now.
    uint32 neededSkill = 0, neededValue = 0;
    if (info.nodeSourced && info.nodeSkillId)
    {
        neededSkill = info.nodeSkillId;
        neededValue = info.nodeRequiredSkill;
    }
    else if (info.IsHunt() && info.SkinOnly())
    {
        neededSkill = SKILL_SKINNING;
        neededValue = SkinningSkillForLevel(info.huntMinLevel);
    }
    if (neededSkill && !bot->HasSkill(neededSkill))
    {
        std::string const result = BotProgression::LearnProfession(bot, neededSkill);
        if (bot->HasSkill(neededSkill))
        {
            BotProgression::GrantProfessionTools(bot);
            SkillLineEntry const* line = sSkillLineStore.LookupEntry(neededSkill);
            char const* lineName = line ? line->name[sWorld->GetDefaultDbcLocale()] : nullptr;
            tellRequester(Acore::StringFormat("{} learned {} ({}) for this order.", bot->GetName(),
                lineName && *lineName ? lineName : "the gathering skill", bot->GetSkillValue(neededSkill)));
        }
        else
        {
            LOG_INFO("module.coa-playerbots", "BotMgr: bot '{}' could not learn skill {} for a gather order ({}).",
                bot->GetName(), neededSkill, result);
            NotifyGatherOrderFail(bot->GetGUID(), bot->GetName(), order, neededSkill == SKILL_SKINNING ? "noskinning" : "noskill");
            return;
        }
    }
    if (neededSkill && bot->GetSkillValue(neededSkill) < neededValue)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: bot '{}' has skill {} at {}, item {} needs {}.",
                bot->GetName(), neededSkill, bot->GetSkillValue(neededSkill), itemEntry, neededValue);
        NotifyGatherOrderFail(bot->GetGUID(), bot->GetName(), order, neededSkill == SKILL_SKINNING ? "noskinning" : "lowskill");
        return;
    }

    uint32 locMap = 0;
    float locX = 0.0f, locY = 0.0f, locZ = 0.0f;
    if (!ResolveOrderLocation(bot, order, false, locMap, locX, locY, locZ))
    {
        // Nothing in the world DB yields this item anywhere a bot can go: say so instead of
        // parking the bot on an order that can never progress.
        if (handler)
            handler->PSendSysMessage("BotMgr: no gather/hunt location found for item {} -- order not placed.", itemEntry);
        NotifyGatherOrderFail(bot->GetGUID(), bot->GetName(), order, "nosource");
        return;
    }

    order.targetMapId = locMap;
    order.targetX = locX;
    order.targetY = locY;
    order.targetZ = locZ;
    order.hasTargetLocation = true;
    order.heldCount = std::min(bot->GetItemCount(itemEntry, false), remaining);
    // Anything it still carries after the deposit above did not fit in the guild bank.
    order.bankBlocked = mailToRequester || order.heldCount > 0;
    SendBotToOrderSpot(bot, order, false);
    order.orderNo = _nextGatherOrderNo++;
    order.startProgress = GatherOrderProgress(order);

    _guildGatherOrders[bot->GetGUID()] = order;
    SaveGuildGatherOrder(bot->GetGUID(), order);

    if (handler)
        handler->PSendSysMessage("BotMgr: guildgather order placed for bot '{}': need {}x item {} (already deposited {}x).",
            bot->GetName(), remaining, itemEntry, deposited);
    if (requesterPlayer && requesterPlayer->GetSession() && (!handler || handler->GetSession() != requesterPlayer->GetSession()))
        ChatHandler(requesterPlayer->GetSession()).PSendSysMessage("|cff33ff99Guild order #{}:|r {} is {} {}x {} for {} ({}).",
            order.orderNo, bot->GetName(), info.IsHunt() ? "hunting for" : "gathering", targetCount, itemName,
            mailToRequester ? "your mailbox" : "the guild bank", order.status);
    LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: guildgather order for bot '{}': item {} need {} (already deposited {}){}.",
        bot->GetName(), itemEntry, remaining, deposited, info.IsHunt() ? ", targeted hunt" : "");
}

void BotMgr::SendBotToOrderSpot(Player* bot, GuildGatherOrder& order, bool rush)
{
    if (!bot || !order.hasTargetLocation)
        return;
    bool const sameMap = bot->GetMapId() == order.targetMapId;
    float const dist = sameMap ? bot->GetExactDist2d(order.targetX, order.targetY) : 0.0f;
    if (sameMap && !rush && dist <= GATHER_ORDER_TELEPORT_BEYOND_YARDS)
    {
        order.status = dist <= 40.0f ? "searching the spot" : "walking to the spot (" + std::to_string(uint32(dist)) + " yd)";
        return;
    }
    if (bot->IsBeingTeleported())
    {
        order.status = "teleporting to the spot";
        return; // the in-flight teleport lands first; UpdateSoloWorld re-sends it from there
    }
    if (bot->IsInCombat())
        bot->CombatStop(true);
    bot->TeleportTo(order.targetMapId, order.targetX, order.targetY, order.targetZ, 0.0f);
    QueueTeleportAck(bot->GetSession());
    order.status = "teleporting to the spot";
    std::string const why = rush ? std::string(" (prioritized)") : sameMap ? Acore::StringFormat(" ({:.0f} yd away)", dist) : std::string(" (other map)");
    LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: gather order for bot '{}' -- teleporting to map {} ({:.0f}, {:.0f}){}.",
        bot->GetName(), order.targetMapId, order.targetX, order.targetY, why);
}

BotMgr::GatherSourceInfo const& BotMgr::GetGatherSourceInfo(uint32 itemEntry)
{
    // Entries are never erased, and unordered_map nodes don't move, so the returned reference
    // stays valid; the lock only guards a first-time build racing a lookup.
    static std::mutex sourceInfoMutex;
    std::lock_guard<std::mutex> lock(sourceInfoMutex);
    auto it = _gatherSourceInfo.find(itemEntry);
    if (it == _gatherSourceInfo.end())
        it = _gatherSourceInfo.emplace(itemEntry, BuildGatherSourceInfo(itemEntry)).first;
    return it->second;
}

bool BotMgr::ResolveOrderLocation(Player* bot, GuildGatherOrder const& order, bool avoidCurrentTarget,
    uint32& mapId, float& x, float& y, float& z)
{
    GatherSourceInfo const& info = GetGatherSourceInfo(order.itemEntry);
    if (info.IsHunt() && ResolveHuntLocation(info, bot, avoidCurrentTarget && order.hasTargetLocation, order.targetMapId,
            order.targetX, order.targetY, mapId, x, y, z, &order.triedSpots))
        return true;
    // Nodes, fish, and anything the hunt data doesn't cover keep the original lookup -- but a retry
    // now skips every spot this order already came up empty at.
    return ResolveGatherLocationForItem(order.itemEntry, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), mapId, x, y, z,
        avoidCurrentTarget ? &order.triedSpots : nullptr, info.nodeSourced ? &info.nodeGoEntries : nullptr);
}

Player* BotMgr::PickGatherBot(uint32 guildId, uint32 itemEntry, bool allowBusy)
{
    GatherSourceInfo const& info = GetGatherSourceInfo(itemEntry);
    Player* best = nullptr;
    uint64 bestScore = (std::numeric_limits<uint64>::max)();
    for (Player* bot : GetOnlineBots())
    {
        if (bot->GetGuildId() != guildId || !bot->IsInWorld())
            continue;
        // Grouped bots follow their player/leader.
        if (bot->GetGroup())
            continue;
        bool const busy = _guildGatherOrders.count(bot->GetGUID()) || _craftOrders.count(bot->GetGUID());
        if (busy && !allowBusy)
            continue;

        // Lower is better: busy >> unsuitable >> levels too far above the source.
        uint64 score = busy ? 1000000 : 0;
        uint32 const level = bot->GetLevel();
        if (info.IsHunt())
        {
            uint32 const source = info.huntMinLevel;
            if (level + GATHER_LEVEL_TOLERANCE < source)
                score += 10000 + (source - level) * 10;
            else if (level > source + GATHER_LEVEL_OVER_SOFT)
                score += level - source - GATHER_LEVEL_OVER_SOFT;
            if (info.SkinOnly())
            {
                uint32 const needed = SkinningSkillForLevel(source);
                if (!bot->HasSkill(SKILL_SKINNING) || bot->GetSkillValue(SKILL_SKINNING) < needed)
                    score += 50000;
            }
            else if (!info.huntSkinEntries.empty() && bot->HasSkill(SKILL_SKINNING))
                score = score > 0 ? score - 1 : 0; // can also skin: a slightly better hide/meat source
        }
        else if (info.nodeSourced && info.nodeSkillId)
        {
            if (!bot->HasSkill(info.nodeSkillId))
                score += 20000;
            else if (bot->GetSkillValue(info.nodeSkillId) < info.nodeRequiredSkill)
                score += 10000 + (info.nodeRequiredSkill - bot->GetSkillValue(info.nodeSkillId));
        }

        if (score < bestScore)
        {
            bestScore = score;
            best = bot;
        }
    }
    return best;
}

void BotMgr::GatherOrder(ObjectGuid::LowType requesterCharLowGuid, uint32 itemEntry, uint32 count, ChatHandler* handler)
{
    Player* requester = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(requesterCharLowGuid));
    if (!requester)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: no online player with guid {} found.", requesterCharLowGuid);
        return;
    }

    uint32 guildId = requester->GetGuildId();
    if (!guildId)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: '{}' is not in a guild -- gather orders only look at guild-mate bots.", requester->GetName());
        return;
    }

    // Best free, ungrouped guild bot whose level/skill suits where the item comes from; when none
    // suits, the best free one anyway (the old "first free bot" behavior, just ranked).
    Player* chosen = PickGatherBot(guildId, itemEntry, false);
    if (!chosen)
    {
        GuildGatherOrder dummy;
        dummy.itemEntry = itemEntry;
        dummy.targetCount = count;
        dummy.requesterLowGuid = requesterCharLowGuid;
        if (!PickGatherBot(guildId, itemEntry, true))
        {
            if (handler)
                handler->PSendSysMessage("BotMgr: no online guild-mate bot found to gather item {}.", itemEntry);
            NotifyGatherOrderFail(ObjectGuid::Empty, "", dummy, "nobot");
            return;
        }

        // Every eligible bot already has an order: queue this one instead of overwriting theirs.
        QueuedGatherOrder queued;
        queued.queueId = _nextGatherQueueId++;
        queued.guildId = guildId;
        queued.requesterLowGuid = requesterCharLowGuid;
        queued.itemEntry = itemEntry;
        queued.count = count;
        _gatherQueue.push_back(queued);
        SaveGatherQueue();
        uint32 position = 0;
        for (QueuedGatherOrder const& q : _gatherQueue)
            if (q.guildId == guildId)
                ++position;
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cff33ff99Guild order:|r every guild bot is busy -- {}x {} is queued (#{}). It starts when a bot frees up.",
            count, GatherItemName(itemEntry), position);
        CoaBotSendAddonReply(requester, "ORDERQUEUED:" + std::to_string(queued.queueId) + ":" + std::to_string(itemEntry) + ":" +
            std::to_string(count) + ":" + std::to_string(position));
        return;
    }

    GuildGather(chosen->GetGUID().GetCounter(), itemEntry, count, handler, requesterCharLowGuid);
}

void BotMgr::ProcessGatherQueue()
{
    if (_gatherQueue.empty())
        return;
    bool changed = false;
    for (auto it = _gatherQueue.begin(); it != _gatherQueue.end(); )
    {
        Player* chosen = PickGatherBot(it->guildId, it->itemEntry, false);
        if (!chosen)
        {
            ++it;
            continue;
        }
        QueuedGatherOrder const queued = *it;
        it = _gatherQueue.erase(it);
        changed = true;
        LOG_DEBUG("module.coa-playerbots", "BotMgr: queued gather order {} ({}x item {}) assigned to '{}'.",
            queued.queueId, queued.count, queued.itemEntry, chosen->GetName());
        // May erase nothing from _gatherQueue (it only touches _guildGatherOrders), so `it` stays valid.
        GuildGather(chosen->GetGUID().GetCounter(), queued.itemEntry, queued.count, nullptr, queued.requesterLowGuid);
    }
    if (changed)
        SaveGatherQueue();
}

BotMgr::GuildGatherOrder const* BotMgr::GetGuildGatherOrder(ObjectGuid const& guid) const
{
    auto it = _guildGatherOrders.find(guid);
    return it != _guildGatherOrders.end() ? &it->second : nullptr;
}

void BotMgr::PauseGuildGatherWatchdog(ObjectGuid const& guid)
{
    auto it = _guildGatherOrders.find(guid);
    if (it == _guildGatherOrders.end())
        return;
    uint32 const nowMs = getMSTime();
    it->second.lastProgressMs = nowMs ? nowMs : 1;
}

BotMgr::CraftOrderState const* BotMgr::GetCraftOrder(ObjectGuid const& guid) const
{
    auto it = _craftOrders.find(guid);
    return it != _craftOrders.end() ? &it->second : nullptr;
}

void BotMgr::RetryGuildGatherLocation(ObjectGuid const& guid, bool rush)
{
    auto it = _guildGatherOrders.find(guid);
    if (it == _guildGatherOrders.end())
        return;

    WorldSession* session = FindBotSession(guid.GetCounter());
    Player* bot = session ? session->GetPlayer() : nullptr;
    if (!bot)
        return;

    if (rush)
        it->second.stalledRetries = 0;
    else if (++it->second.stalledRetries >= GUILD_GATHER_MAX_STALLED_RETRIES)
    {
        LOG_DEBUG(BotAI::BotDebugLog::LoggerName(guid), "BotMgr: guild gather order for bot '{}' (item {}) gave up after {} fruitless relocations.",
            bot->GetName(), it->second.itemEntry, it->second.stalledRetries);
        FailGuildGatherOrder(guid, "stuck");
        return;
    }

    // Remember the spot that just came up empty so the next pick goes somewhere else.
    GuildGatherOrder& order = it->second;
    if (order.hasTargetLocation)
    {
        order.triedSpots.push_back({ order.targetMapId, order.targetX, order.targetY });
        if (order.triedSpots.size() > GATHER_MAX_TRIED_SPOTS)
            order.triedSpots.erase(order.triedSpots.begin());
    }

    uint32 locMap = 0;
    float locX = 0.0f, locY = 0.0f, locZ = 0.0f;
    if (!ResolveOrderLocation(bot, order, true, locMap, locX, locY, locZ))
    {
        FailGuildGatherOrder(guid, "nosource");
        return;
    }

    order.targetMapId = locMap;
    order.targetX = locX;
    order.targetY = locY;
    order.targetZ = locZ;
    order.hasTargetLocation = true;
    SaveGuildGatherOrder(guid, order);

    // Close spots are walked (UpdateSoloWorld's "dist > 40 -> mount up and walk"), so bots don't
    // hop node to node; another map, a spot further than GATHER_ORDER_TELEPORT_BEYOND_YARDS, or a
    // player's explicit PRIORITIZEORDER (rush) is teleported to.
    SendBotToOrderSpot(bot, order, rush);
    LOG_DEBUG(BotAI::BotDebugLog::LoggerName(guid), "BotMgr: guild gather order for bot '{}' (item {}) moved to a fresh spot, try {}/{}{}.",
        bot->GetName(), order.itemEntry, order.stalledRetries, GUILD_GATHER_MAX_STALLED_RETRIES, rush ? " (prioritized by a player)" : "");
}

void BotMgr::NotifyGatherOrderDone(Player* bot, GuildGatherOrder const& order, uint32 mailedCount)
{
    if (!order.requesterLowGuid || FindBotSession(order.requesterLowGuid))
        return;
    Player* requester = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(order.requesterLowGuid));
    if (!requester || !requester->GetSession())
        return;
    if (mailedCount && order.mailToRequester)
        ChatHandler(requester->GetSession()).PSendSysMessage("|cff33ff99Order done:|r {} gathered {}x {} and mailed {} to you.",
            bot ? bot->GetName() : std::string("A bot"), order.targetCount, GatherItemName(order.itemEntry), mailedCount);
    else if (mailedCount)
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cff33ff99Guild order done:|r {} gathered {}x {} -- the guild bank had no room, so {}x {} mailed to you (check your mailbox).",
            bot ? bot->GetName() : std::string("A bot"), order.targetCount, GatherItemName(order.itemEntry), mailedCount,
            mailedCount == 1 ? "was" : "were");
    else
        ChatHandler(requester->GetSession()).PSendSysMessage("|cff33ff99Guild order done:|r {} put {}x {} in the guild bank.",
            bot ? bot->GetName() : std::string("A bot"), order.targetCount, GatherItemName(order.itemEntry));
    CoaBotSendAddonReply(requester, "ORDERDONE:" + std::to_string(bot ? bot->GetGUID().GetCounter() : 0) + ":" +
        std::to_string(order.itemEntry) + ":" + std::to_string(order.targetCount));
}

void BotMgr::NotifyGatherOrderFail(ObjectGuid const& botGuid, std::string const& botName, GuildGatherOrder const& order,
    std::string const& reason)
{
    LOG_INFO("module.coa-playerbots", "BotMgr: guild gather order for {}x item {} (bot '{}') failed: {}.",
        order.targetCount, order.itemEntry, botName, reason);
    if (!order.requesterLowGuid || FindBotSession(order.requesterLowGuid))
        return;
    Player* requester = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(order.requesterLowGuid));
    if (!requester || !requester->GetSession())
        return;
    std::string const who = botName.empty() ? std::string("The guild order") : botName;
    ChatHandler(requester->GetSession()).PSendSysMessage("|cffff4040Guild order failed:|r {} gave up on {}x {} ({}/{} delivered) -- {}.",
        who, order.targetCount, GatherItemName(order.itemEntry), order.gatheredCount, order.targetCount, GatherFailReasonText(reason));
    CoaBotSendAddonReply(requester, "ORDERFAIL:" + std::to_string(botGuid.GetCounter()) + ":" + std::to_string(order.itemEntry) + ":" +
        std::to_string(order.gatheredCount) + ":" + std::to_string(order.targetCount) + ":" + reason);
}

void BotMgr::FailGuildGatherOrder(ObjectGuid const& botGuid, std::string const& reason)
{
    auto it = _guildGatherOrders.find(botGuid);
    if (it == _guildGatherOrders.end())
        return;
    GuildGatherOrder const order = it->second;
    _guildGatherOrders.erase(it);
    DeleteGuildGatherOrder(botGuid);

    std::string name;
    if (Player* bot = FindBotPlayer(botGuid.GetCounter()))
        name = bot->GetName();
    else if (!sCharacterCache->GetCharacterNameByGuid(botGuid, name))
        name = "A bot";
    NotifyGatherOrderFail(botGuid, name, order, reason);
}

std::string BotMgr::OrderMailBody(Player* bot)
{
    return bot->GetName() + " gathered this for you.";
}

uint32 BotMgr::MailGatherItems(Player* bot, ObjectGuid::LowType requesterLowGuid, uint32 itemEntry, uint32 count,
    std::string const& body)
{
    if (!bot || !requesterLowGuid || !count)
        return 0;

    std::vector<Item*> stacks;
    uint32 total = 0;
    auto consider = [&](Item* item)
    {
        if (!item || item->GetEntry() != itemEntry || total >= count || item->IsInTrade() || item->IsNotEmptyBag())
            return;
        if (item->IsSoulBound() && !item->IsBoundAccountWide())
            return;
        if (bot->GetLootGUID() == item->GetGUID())
            return;
        stacks.push_back(item);
        total += item->GetCount();
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        consider(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = bot->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                consider(bag->GetItemByPos(slot));
    if (stacks.empty())
        return 0;

    ObjectGuid const receiverGuid = ObjectGuid::Create<HighGuid::Player>(requesterLowGuid);
    Player* receiverPlayer = ObjectAccessor::FindConnectedPlayer(receiverGuid);
    std::string const subject = (body.empty() ? "Guild order: " : "Order: ") + GatherItemName(itemEntry);
    uint32 mailed = 0;
    std::size_t next = 0;
    while (next < stacks.size())
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        MailDraft draft(subject, body.empty() ? bot->GetName() + " gathered this for you. The guild bank had no free slot for it." : body);
        for (std::size_t n = 0; n < MAX_MAIL_ITEMS && next < stacks.size(); ++n, ++next)
        {
            Item* item = stacks[next];
            // The last stack may hold more than the order still needs: split off just that part
            // (same CloneItem + SetCount sequence as Player::SplitItem); the bot keeps the rest.
            if (mailed + item->GetCount() > count)
            {
                uint32 const part = count - mailed;
                Item* piece = item->CloneItem(part, bot);
                if (!piece)
                    continue;
                item->SetCount(item->GetCount() - part);
                if (bot->IsInWorld())
                    item->SendUpdateToPlayer(bot);
                item->SetState(ITEM_CHANGED, bot);
                piece->SetOwnerGUID(receiverGuid);
                piece->SaveToDB(trans);
                draft.AddItem(piece);
                mailed += part;
                continue;
            }
            mailed += item->GetCount();
            bot->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);
            item->DeleteFromInventoryDB(trans);
            if (item->GetState() == ITEM_UNCHANGED)
                item->FSetState(ITEM_CHANGED);
            item->SetOwnerGUID(receiverGuid);
            item->SaveToDB(trans);
            draft.AddItem(item);
        }
        // An online receiver gets the mail in memory too (no relog needed to see it).
        MailReceiver const receiver = receiverPlayer ? MailReceiver(receiverPlayer, requesterLowGuid) : MailReceiver(requesterLowGuid);
        draft.SendMailTo(trans, receiver, MailSender(bot), MAIL_CHECK_MASK_COPIED);
        bot->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);
    }
    return mailed;
}

uint32 BotMgr::GatherOrderProgress(GuildGatherOrder const& order)
{
    return std::min(order.targetCount, order.gatheredCount + order.heldCount);
}

void BotMgr::SetGatherOrderStatus(ObjectGuid const& guid, std::string const& status)
{
    auto it = _guildGatherOrders.find(guid);
    if (it != _guildGatherOrders.end() && it->second.status != status)
        it->second.status = status;
}

namespace
{
// An order bot keeps at least this many bag slots free while it works (checked every
// GATHER_ROOM_CHECK_MS): a node can drop 2-3 different items, and anything that does not fit
// stays in the node -- the bot is seen gathering, yet nothing reaches its bags or the board.
constexpr uint32 GATHER_ORDER_MIN_FREE_SLOTS = 4;
constexpr uint32 GATHER_ROOM_CHECK_MS = 10 * IN_MILLISECONDS;

bool CanTakeOneMore(Player* bot, uint32 itemEntry)
{
    ItemPosCountVec dest;
    return bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemEntry, 1) == EQUIP_ERR_OK;
}

// Free slots in the backpack and normal bags only. Player::GetFreeInventorySpace also counts
// quiver/ammo-pouch and profession-bag slots, which ore and cloth can never go into, so a hunter
// with an empty quiver looked like it had room while its bags were full.
uint32 FreeGeneralBagSlots(Player* bot)
{
    uint32 freeSlots = 0;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++freeSlots;
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = bot->GetBagByPos(bagSlot))
        {
            ItemTemplate const* proto = bag->GetTemplate();
            if (proto && proto->Class == ITEM_CLASS_CONTAINER && proto->SubClass != ITEM_SUBCLASS_CONTAINER)
                continue; // herb/mining/enchanting... bag: special items only
            if (proto && proto->Class == ITEM_CLASS_QUIVER)
                continue;
            freeSlots += bag->GetFreeSlots();
        }
    return freeSlots;
}

// Entry -> total count of every Trade Goods stack in the bags (bank excluded).
std::map<uint32, uint32> CarriedTradeGoods(Player* bot)
{
    std::map<uint32, uint32> carried;
    auto add = [&](Item* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (proto && proto->Class == ITEM_CLASS_TRADE_GOODS)
            carried[proto->ItemId] += item->GetCount();
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        add(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = bot->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                add(bag->GetItemByPos(slot));
    return carried;
}
}

void BotMgr::TellGatherRequester(GuildGatherOrder const& order, std::string const& text)
{
    if (!order.requesterLowGuid || FindBotSession(order.requesterLowGuid))
        return;
    if (Player* requester = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(order.requesterLowGuid)))
        if (requester->GetSession())
            ChatHandler(requester->GetSession()).SendSysMessage(text);
}

bool BotMgr::GetGatherOrderItem(ObjectGuid const& guid, uint32* itemEntry) const
{
    auto it = _guildGatherOrders.find(guid);
    if (it == _guildGatherOrders.end())
        return false;
    if (itemEntry)
        *itemEntry = it->second.itemEntry;
    return true;
}

void BotMgr::NoteGatherLoot(Player* bot, std::vector<std::pair<uint32, uint32>> const& looted, uint32 orderItemsStored,
    bool leftBehind)
{
    auto it = bot ? _guildGatherOrders.find(bot->GetGUID()) : _guildGatherOrders.end();
    if (it == _guildGatherOrders.end())
        return;
    GuildGatherOrder& order = it->second;
    ++order.lootsOpened;
    if (orderItemsStored)
        ++order.lootsWithItem;
    if (leftBehind)
    {
        ++order.lootsLeftBehind;
        order.bagsFull = true;          // the update loop makes room and re-checks
        order.lastRoomCheckMs = 0;
    }
    std::string text;
    for (auto const& [entry, count] : looted)
    {
        if (text.size() > 120)
        {
            text += ", ...";
            break;
        }
        text += (text.empty() ? "" : ", ") + Acore::StringFormat("{} x{}", entry, count);
    }
    order.lastLoot = Acore::StringFormat("{} -> {} of the ordered item stored{}", text.empty() ? std::string("empty") : text,
        orderItemsStored, leftBehind ? ", SOME LEFT BEHIND (bags full)" : "");
    LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: order #{} bot '{}' looted: {}", order.orderNo, bot->GetName(),
        order.lastLoot);
}

bool BotMgr::MakeRoomForGatherOrder(Player* bot)
{
    auto it = bot ? _guildGatherOrders.find(bot->GetGUID()) : _guildGatherOrders.end();
    if (it == _guildGatherOrders.end())
        return false;
    GuildGatherOrder& order = it->second;
    uint32 const itemEntry = order.itemEntry;
    auto roomy = [&]() { return CanTakeOneMore(bot, itemEntry) && FreeGeneralBagSlots(bot) >= GATHER_ORDER_MIN_FREE_SLOTS; };
    if (roomy())
        return true;

    // 1. Junk: greys and outgrown gear -- sold when a vendor is in reach, destroyed otherwise.
    //    IsClutterItem never takes Trade Goods, so the order's own materials are safe here.
    BotAI::PrepareBagsForTask(bot);
    if (roomy())
        return true;

    // 2. Carried order items go to the guild bank now and count as banked progress (the update
    //    loop's deposit does the same; doing it here frees their slots before the next node).
    uint32 const carried = std::min(bot->GetItemCount(itemEntry, false), order.remainingCount);
    if (carried && !order.bankBlocked)
    {
        uint32 const deposited = GuildDepositItem(bot->GetGUID().GetCounter(), itemEntry, carried, nullptr);
        if (deposited)
        {
            order.gatheredCount += deposited;
            order.remainingCount = order.remainingCount > deposited ? order.remainingCount - deposited : 0;
            order.heldCount = std::min(bot->GetItemCount(itemEntry, false), order.remainingCount);
            order.lastProgressMs = getMSTime();
            SaveGuildGatherOrder(it->first, order);
        }
        else
            order.bankBlocked = true;
        if (roomy())
            return true;
    }

    // 3. Other materials (Rough Stone next to Copper Ore, old leveling cloth...) to the guild bank.
    bool const canMail = order.requesterLowGuid && !FindBotSession(order.requesterLowGuid);
    std::map<uint32, uint32> other = CarriedTradeGoods(bot);
    other.erase(itemEntry);
    for (auto otherItr = other.begin(); otherItr != other.end();)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(otherItr->first);
        if (bot->HasQuestForItem(otherItr->first, 0, true) || BotAI::IsProfessionTool(proto) || BotEconomy::IsReservedReagent(bot, otherItr->first))
            otherItr = other.erase(otherItr);
        else
            ++otherItr;
    }
    for (auto const& [entry, count] : other)
    {
        if (order.mailToRequester)
            break;
        GuildDepositItem(bot->GetGUID().GetCounter(), entry, 0, nullptr);
        if (roomy())
            return true;
    }

    // 4. Guild bank full: mail the ordered items carried so far to the requester (counted as
    //    banked progress), then the other materials, a few entries per pass.
    if (canMail && order.remainingCount)
    {
        uint32 const held = std::min(bot->GetItemCount(itemEntry, false), order.remainingCount);
        // Keep the order item while a stack of it still has room: mailing it only helps when it is
        // what fills the bags (a stack that is full, or no slot for a new one).
        if (held && !CanTakeOneMore(bot, itemEntry))
        {
            uint32 const mailed = MailGatherItems(bot, order.requesterLowGuid, itemEntry, held);
            if (mailed)
            {
                order.gatheredCount += mailed;
                order.remainingCount = order.remainingCount > mailed ? order.remainingCount - mailed : 0;
                order.heldCount = std::min(bot->GetItemCount(itemEntry, false), order.remainingCount);
                order.lastProgressMs = getMSTime();
                SaveGuildGatherOrder(it->first, order);
                TellGatherRequester(order, Acore::StringFormat(
                    "|cffffcc00Guild order #{}:|r {}'s bags were full and the guild bank too, so {}x {} went to your mailbox ({}/{}).",
                    order.orderNo, bot->GetName(), mailed, GatherItemName(itemEntry), GatherOrderProgress(order), order.targetCount));
            }
        }
        if (roomy())
            return true;
    }
    if (canMail)
    {
        uint32 sent = 0;
        for (auto const& [entry, count] : other)
        {
            uint32 const left = bot->GetItemCount(entry, false);
            if (!left)
                continue;
            if (MailGatherItems(bot, order.requesterLowGuid, entry, left))
                ++sent;
            if (roomy() || sent >= 3)
                break;
        }
        if (sent)
            TellGatherRequester(order, Acore::StringFormat(
                "|cffffcc00Guild order #{}:|r the guild bank is full, so {} mailed you {} stack type(s) of other materials to keep bag space for {}.",
                order.orderNo, bot->GetName(), sent, GatherItemName(itemEntry)));
    }
    // "Room" for the order means the item fits; the 4-slot margin is best effort.
    return CanTakeOneMore(bot, itemEntry) && FreeGeneralBagSlots(bot) > 0;
}

void BotMgr::DescribeGatherOrders(ChatHandler* handler) const
{
    if (!handler)
        return;
    if (_guildGatherOrders.empty() && _gatherQueue.empty())
    {
        handler->PSendSysMessage("Gather orders: none active, none queued.");
        return;
    }
    handler->PSendSysMessage("Gather orders: {} active, {} queued.", _guildGatherOrders.size(), _gatherQueue.size());
    std::unordered_set<uint32> guildsShown;
    for (auto const& entry : _guildGatherOrders)
    {
        Player* bot = FindBotPlayer(entry.first.GetCounter());
        Guild* guild = bot ? bot->GetGuild() : nullptr;
        if (!guild || !guildsShown.insert(guild->GetId()).second)
            continue;
        uint32 freeSlots = 0;
        for (uint8 tabId = 0; tabId < guild->GetPurchasedTabsSize(); ++tabId)
            for (uint8 slotId = 0; slotId < GUILD_BANK_MAX_SLOTS; ++slotId)
                if (!guild->GetBankItem(tabId, slotId))
                    ++freeSlots;
        handler->PSendSysMessage("Guild '{}' bank: {} tab(s), {} free slot(s){}.", guild->GetName(), guild->GetPurchasedTabsSize(), freeSlots,
            freeSlots ? "" : " -- FULL: gathered items are mailed to the requester instead");
    }
    uint32 const nowMs = getMSTime();
    for (auto const& [guid, order] : _guildGatherOrders)
    {
        Player* bot = FindBotPlayer(guid.GetCounter());
        std::string const item = GatherItemName(order.itemEntry);
        if (!bot || !bot->IsInWorld())
        {
            handler->PSendSysMessage("- bot {} (offline/loading): {} {}/{} banked.", guid.GetCounter(), item, order.gatheredCount, order.targetCount);
            continue;
        }
        GatherSourceInfo const& info = const_cast<BotMgr*>(this)->GetGatherSourceInfo(order.itemEntry);
        char const* kind = info.IsHunt() ? "hunt" : info.nodeSourced ? "node" : info.fishSourced ? "fish" : "other";
        std::string where = order.hasTargetLocation
            ? (bot->GetMapId() == order.targetMapId
                ? Acore::StringFormat("{:.0f} yd from its spot", bot->GetExactDist2d(order.targetX, order.targetY))
                : Acore::StringFormat("spot is on map {}, bot on map {}", order.targetMapId, bot->GetMapId()))
            : std::string("no spot");
        std::string skill = "-";
        if (info.nodeSourced && info.nodeSkillId)
            skill = Acore::StringFormat("skill {} = {} (needs {})", info.nodeSkillId,
                bot->HasSkill(info.nodeSkillId) ? bot->GetSkillValue(info.nodeSkillId) : 0, info.nodeRequiredSkill);
        else if (info.IsHunt())
            skill = Acore::StringFormat("Skinning = {}", bot->HasSkill(SKILL_SKINNING) ? bot->GetSkillValue(SKILL_SKINNING) : 0);
        uint32 const idleSec = order.lastProgressMs ? getMSTimeDiff(order.lastProgressMs, nowMs) / IN_MILLISECONDS : 0;
        handler->PSendSysMessage("- {} (lvl {}): {} [{}] {}/{} (banked {}, carrying {}){} | {} | {}",
            bot->GetName(), bot->GetLevel(), item, kind, GatherOrderProgress(order), order.targetCount, order.gatheredCount,
            order.heldCount, order.bankBlocked ? " | GUILD BANK FULL" : "", order.status.empty() ? "starting" : order.status, where);
        handler->PSendSysMessage("    {} | free bag slots {} | spot tries {}/{} | no progress for {}s{}{}",
            skill, bot->GetFreeInventorySpace(), order.stalledRetries, GUILD_GATHER_MAX_STALLED_RETRIES, idleSec,
            bot->GetGroup() ? " | IN A GROUP" : "", bot->IsInCombat() ? " | in combat" : "");
        // Raw counts, straight from the bags: what the order counter sees.
        uint32 const inBags = bot->GetItemCount(order.itemEntry, false);
        uint32 const withBank = bot->GetItemCount(order.itemEntry, true);
        handler->PSendSysMessage("    order #{} | item {} in bags: {} (bot's own bank +{}) | counted carrying {} | room for 1 more: {} | "
            "free normal bag slots {}{}",
            order.orderNo, order.itemEntry, inBags, withBank - inBags, order.heldCount, CanTakeOneMore(bot, order.itemEntry) ? "yes" : "NO",
            FreeGeneralBagSlots(bot), order.bagsFull ? " | BAGS FULL - making room" : "");
        std::string goods;
        uint32 shown = 0;
        for (auto const& [entry, count] : CarriedTradeGoods(bot))
        {
            if (++shown > 12)
            {
                goods += " ...";
                break;
            }
            goods += Acore::StringFormat(" {}x{}", entry, count);
        }
        handler->PSendSysMessage("    trade goods in bags (id x count):{}", goods.empty() ? std::string(" none") : goods);
        if (info.nodeSourced)
            handler->PSendSysMessage("    {} node type(s) yield it, best chance per node {:.1f}%{}", info.nodeGoEntries.size(),
                info.nodeBestChance, info.nodeBestChance > 0.0f && info.nodeBestChance < 10.0f ? " (RARE: expect many nodes per item)" : "");
        handler->PSendSysMessage("    loot windows emptied: {} | with the item: {} | left items behind: {} | last: {}", order.lootsOpened,
            order.lootsWithItem, order.lootsLeftBehind, order.lastLoot.empty() ? std::string("-") : order.lastLoot);
    }
    uint32 position = 0;
    for (QueuedGatherOrder const& q : _gatherQueue)
        handler->PSendSysMessage("- queued #{}: {}x {} (waiting for a free, ungrouped guild bot)", ++position, q.count, GatherItemName(q.itemEntry));
}

bool BotMgr::CanManageGatherOrder(Player* commander, uint32 guildId, ObjectGuid::LowType requesterLowGuid) const
{
    if (commander && requesterLowGuid && commander->GetGUID().GetCounter() == requesterLowGuid)
        return true;
    if (!commander || !guildId || commander->GetGuildId() != guildId)
        return false;
    Guild* guild = sGuildMgr->GetGuildById(guildId);
    Guild::Member const* member = guild ? guild->GetMember(commander->GetGUID()) : nullptr;
    return member && member->GetRankId() <= 1; // guild master or officer
}

std::string BotMgr::CancelGatherOrder(Player* commander, ObjectGuid::LowType botLowGuid)
{
    ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(botLowGuid);
    auto it = _guildGatherOrders.find(guid);
    if (it == _guildGatherOrders.end())
        return CancelCraftOrder(commander, botLowGuid);
    Player* bot = FindBotPlayer(botLowGuid);
    uint32 const guildId = bot ? bot->GetGuildId() : sCharacterCache->GetCharacterGuildIdByGuid(guid);
    if (!CanManageGatherOrder(commander, guildId, it->second.requesterLowGuid))
        return "denied";

    GuildGatherOrder const order = it->second;
    _guildGatherOrders.erase(it);
    DeleteGuildGatherOrder(guid);
    std::string name = bot ? bot->GetName() : std::string("the bot");
    ChatHandler(commander->GetSession()).PSendSysMessage("|cff33ff99Guild order cancelled:|r {} stops gathering {} ({}/{} delivered).",
        name, GatherItemName(order.itemEntry), order.gatheredCount, order.targetCount);
    LOG_INFO("module.coa-playerbots", "BotMgr: '{}' cancelled the gather order of bot {} ({}x item {}).",
        commander->GetName(), botLowGuid, order.targetCount, order.itemEntry);
    // The bot's next idle tick finds no order and drops back to its own open-world life.
    return "ok";
}

std::string BotMgr::CancelCraftOrder(Player* commander, ObjectGuid::LowType botLowGuid)
{
    ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(botLowGuid);
    auto it = _craftOrders.find(guid);
    if (it == _craftOrders.end())
        return "none";
    Player* bot = FindBotPlayer(botLowGuid);
    uint32 const guildId = bot ? bot->GetGuildId() : sCharacterCache->GetCharacterGuildIdByGuid(guid);
    if (!CanManageGatherOrder(commander, guildId, it->second.requesterGuid.GetCounter()))
        return "denied";
    CraftOrderState const order = it->second;
    _craftOrders.erase(it);
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(order.itemEntry);
    ChatHandler(commander->GetSession()).PSendSysMessage("|cff33ff99Crafting order cancelled:|r {} stops crafting {} ({}/{} made).",
        bot ? bot->GetName() : std::string("the bot"), proto ? proto->Name1 : std::to_string(order.itemEntry),
        order.orderedCount - std::min(order.orderedCount, order.remainingCount), order.orderedCount);
    LOG_INFO("module.coa-playerbots", "BotMgr: '{}' cancelled the crafting order of bot {} (item {}).", commander->GetName(), botLowGuid, order.itemEntry);
    return "ok";
}

std::string BotMgr::PrioritizeGatherOrder(Player* commander, ObjectGuid::LowType botLowGuid)
{
    ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(botLowGuid);
    auto it = _guildGatherOrders.find(guid);
    if (it == _guildGatherOrders.end())
        return "none";
    Player* bot = FindBotPlayer(botLowGuid);
    if (!bot || !bot->IsInWorld())
        return "offline";
    if (!CanManageGatherOrder(commander, bot->GetGuildId(), it->second.requesterLowGuid))
        return "denied";
    if (bot->IsBeingTeleported())
        return "busy";
    RetryGuildGatherLocation(guid, true);
    if (!_guildGatherOrders.count(guid))
        return "failed"; // no source left -- RetryGuildGatherLocation already sent ORDERFAIL
    ChatHandler(commander->GetSession()).PSendSysMessage("|cff33ff99Guild order:|r {} is heading straight to a fresh spot for {}.",
        bot->GetName(), GatherItemName(it->second.itemEntry));
    return "ok";
}

std::string BotMgr::CancelQueuedGatherOrder(Player* commander, uint32 queueId)
{
    auto it = std::find_if(_gatherQueue.begin(), _gatherQueue.end(), [queueId](QueuedGatherOrder const& q) { return q.queueId == queueId; });
    if (it == _gatherQueue.end())
        return "none";
    if (!CanManageGatherOrder(commander, it->guildId, it->requesterLowGuid))
        return "denied";
    ChatHandler(commander->GetSession()).PSendSysMessage("|cff33ff99Guild order cancelled:|r queued {}x {} removed.",
        it->count, GatherItemName(it->itemEntry));
    _gatherQueue.erase(it);
    SaveGatherQueue();
    return "ok";
}

std::string BotMgr::TopQueuedGatherOrder(Player* commander, uint32 queueId)
{
    auto it = std::find_if(_gatherQueue.begin(), _gatherQueue.end(), [queueId](QueuedGatherOrder const& q) { return q.queueId == queueId; });
    if (it == _gatherQueue.end())
        return "none";
    if (!CanManageGatherOrder(commander, it->guildId, it->requesterLowGuid))
        return "denied";
    std::rotate(_gatherQueue.begin(), it, it + 1); // move to the front, everything else keeps its order
    SaveGatherQueue();
    return "ok";
}

void BotMgr::LoadGuildGatherOrders()
{
    // Ensure database table exists
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS mod_coa_bot_guild_gather_orders ("
        "  bot_guid INT UNSIGNED NOT NULL PRIMARY KEY,"
        "  item_entry INT UNSIGNED NOT NULL,"
        "  target_count INT UNSIGNED NOT NULL,"
        "  gathered_count INT UNSIGNED NOT NULL,"
        "  remaining_count INT UNSIGNED NOT NULL,"
        "  target_map INT UNSIGNED NOT NULL,"
        "  target_x FLOAT NOT NULL,"
        "  target_y FLOAT NOT NULL,"
        "  target_z FLOAT NOT NULL,"
        "  has_target_location TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "  requester_guid INT UNSIGNED NOT NULL DEFAULT 0"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;"
    );
    // Tables created before requester_guid existed: add it once (same change as the module's
    // data/sql/db-characters/updates file, done here too so it works whether or not the core's
    // DB updater applies module SQL on this server).
    if (!CharacterDatabase.Query("SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() "
            "AND TABLE_NAME = 'mod_coa_bot_guild_gather_orders' AND COLUMN_NAME = 'requester_guid'"))
        CharacterDatabase.DirectExecute("ALTER TABLE mod_coa_bot_guild_gather_orders ADD COLUMN requester_guid INT UNSIGNED NOT NULL DEFAULT 0");
    if (!CharacterDatabase.Query("SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() "
            "AND TABLE_NAME = 'mod_coa_bot_guild_gather_orders' AND COLUMN_NAME = 'deliver_mail'"))
        CharacterDatabase.DirectExecute("ALTER TABLE mod_coa_bot_guild_gather_orders ADD COLUMN deliver_mail TINYINT UNSIGNED NOT NULL DEFAULT 0");

    LoadGatherQueue();

    QueryResult result = CharacterDatabase.Query(
        "SELECT bot_guid, item_entry, target_count, gathered_count, remaining_count, target_map, target_x, target_y, target_z, has_target_location, requester_guid, "
        "deliver_mail FROM mod_coa_bot_guild_gather_orders"
    );

    if (!result)
        return;

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();
        uint32 lowGuid = fields[0].Get<uint32>();
        ObjectGuid botGuid = ObjectGuid::Create<HighGuid::Player>(lowGuid);

        GuildGatherOrder order;
        order.itemEntry = fields[1].Get<uint32>();
        order.targetCount = fields[2].Get<uint32>();
        order.gatheredCount = fields[3].Get<uint32>();
        order.remainingCount = fields[4].Get<uint32>();
        order.targetMapId = fields[5].Get<uint32>();
        order.targetX = fields[6].Get<float>();
        order.targetY = fields[7].Get<float>();
        order.targetZ = fields[8].Get<float>();
        order.hasTargetLocation = fields[9].Get<uint8>() != 0;
        order.requesterLowGuid = fields[10].Get<uint32>();
        order.mailToRequester = fields[11].Get<uint8>() != 0;
        order.bankBlocked = order.mailToRequester;
        order.orderNo = _nextGatherOrderNo++;
        // Carried items are only known once the bot is online; the first update loop pass
        // counts them, so a restart re-announces "first progress" only for new items.
        order.startProgress = std::min(order.targetCount, order.gatheredCount);

        _guildGatherOrders[botGuid] = order;
        ++count;
    } while (result->NextRow());

    LOG_INFO("module.coa-playerbots", "BotMgr: Loaded {} guild gather orders from database.", count);
}

void BotMgr::SaveGuildGatherOrder(ObjectGuid const& guid, GuildGatherOrder const& order)
{
    CharacterDatabase.Execute(
        "REPLACE INTO mod_coa_bot_guild_gather_orders "
        "(bot_guid, item_entry, target_count, gathered_count, remaining_count, target_map, target_x, target_y, target_z, has_target_location, requester_guid, deliver_mail) "
        "VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {})",
        guid.GetCounter(), order.itemEntry, order.targetCount, order.gatheredCount, order.remainingCount,
        order.targetMapId, order.targetX, order.targetY, order.targetZ, order.hasTargetLocation ? 1 : 0, order.requesterLowGuid,
        order.mailToRequester ? 1 : 0
    );
}

void BotMgr::DeleteGuildGatherOrder(ObjectGuid const& guid)
{
    CharacterDatabase.Execute("DELETE FROM mod_coa_bot_guild_gather_orders WHERE bot_guid = {}", guid.GetCounter());
}

void BotMgr::LoadGatherQueue()
{
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS mod_coa_bot_guild_gather_queue ("
        "  queue_id INT UNSIGNED NOT NULL PRIMARY KEY,"
        "  position INT UNSIGNED NOT NULL,"
        "  guild_id INT UNSIGNED NOT NULL,"
        "  requester_guid INT UNSIGNED NOT NULL DEFAULT 0,"
        "  item_entry INT UNSIGNED NOT NULL,"
        "  item_count INT UNSIGNED NOT NULL"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;"
    );
    _gatherQueue.clear();
    QueryResult result = CharacterDatabase.Query(
        "SELECT queue_id, guild_id, requester_guid, item_entry, item_count FROM mod_coa_bot_guild_gather_queue ORDER BY position");
    if (!result)
        return;
    do
    {
        Field* fields = result->Fetch();
        QueuedGatherOrder q;
        q.queueId = fields[0].Get<uint32>();
        q.guildId = fields[1].Get<uint32>();
        q.requesterLowGuid = fields[2].Get<uint32>();
        q.itemEntry = fields[3].Get<uint32>();
        q.count = fields[4].Get<uint32>();
        _gatherQueue.push_back(q);
        _nextGatherQueueId = std::max(_nextGatherQueueId, q.queueId + 1);
    } while (result->NextRow());
    LOG_INFO("module.coa-playerbots", "BotMgr: Loaded {} queued guild gather orders from database.", uint32(_gatherQueue.size()));
}

void BotMgr::SaveGatherQueue()
{
    // Small (a handful of rows at most): rewrite it whole so position always matches the vector.
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    trans->Append("DELETE FROM mod_coa_bot_guild_gather_queue");
    uint32 position = 0;
    for (QueuedGatherOrder const& q : _gatherQueue)
        trans->Append("INSERT INTO mod_coa_bot_guild_gather_queue (queue_id, position, guild_id, requester_guid, item_entry, item_count) "
            "VALUES ({}, {}, {}, {}, {}, {})", q.queueId, position++, q.guildId, q.requesterLowGuid, q.itemEntry, q.count);
    CharacterDatabase.CommitTransaction(trans);
}

void BotMgr::CraftOrder(ObjectGuid::LowType requesterCharLowGuid, uint32 itemEntry, uint32 count, ChatHandler* handler)
{
    Player* requester = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(requesterCharLowGuid));
    if (!requester)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: no online player with guid {} found.", requesterCharLowGuid);
        return;
    }

    uint32 guildId = requester->GetGuildId();
    if (!guildId)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: '{}' is not in a guild -- crafting orders only look at guild-mate bots.", requester->GetName());
        return;
    }

    auto const& index = CraftingRecipeIndex();
    auto recipeItr = index.find(itemEntry);
    if (recipeItr == index.end())
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: no known recipe produces item {}.", itemEntry);
        return;
    }

    // Prefer a guild-mate bot that already has enough reagents to start right away; fall back
    // to the first one found that at least knows the recipe (it'll wait on reagents instead).
    Player* crafter = nullptr;
    uint32 chosenSpell = 0;
    for (Player* bot : GetOnlineBots())
    {
        if (bot->GetGuildId() != guildId)
            continue;
        for (uint32 spellId : recipeItr->second)
        {
            if (!bot->HasSpell(spellId))
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info)
                continue;
            bool ready = CrafterHasReagentsFor(bot, info);
            if (!crafter || ready)
            {
                crafter = bot;
                chosenSpell = spellId;
            }
            if (ready)
                break;
        }
        if (crafter && CrafterHasReagentsFor(crafter, sSpellMgr->GetSpellInfo(chosenSpell)))
            break;
    }

    if (!crafter)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: no online guild-mate bot knows a recipe for item {}.", itemEntry);
        return;
    }

    CraftOrderState& order = _craftOrders[crafter->GetGUID()];
    order = CraftOrderState();
    order.requesterGuid = requester->GetGUID();
    order.spellId = chosenSpell;
    order.itemEntry = itemEntry;
    order.remainingCount = count;
    order.orderedCount = count;
    order.placedAtMs = std::max<uint32>(1, GameTime::GetGameTimeMS().count());
    order.nextSourcingAtMs = order.placedAtMs;
    order.reagentBudgetLeftCopper = BotEconomy::CraftReagentBudgetCopper();

    if (handler)
        handler->PSendSysMessage("BotMgr: craft order placed -- bot '{}' will craft {}x item {} for '{}' (spell {}).",
            crafter->GetName(), count, itemEntry, requester->GetName(), chosenSpell);
    LOG_INFO("module.coa-playerbots", "BotMgr: craft order placed -- bot '{}' crafting {}x item {} for '{}' (spell {}).",
        crafter->GetName(), count, itemEntry, requester->GetName(), chosenSpell);
}

std::vector<std::string> BotMgr::GetGuildRosterInfo(Player* commander) const
{
    std::vector<std::string> lines;
    uint32 guildId = commander ? commander->GetGuildId() : 0;
    if (!guildId)
        return lines;

    for (Player* bot : GetOnlineBots())
    {
        if (bot->GetGuildId() != guildId)
            continue;

        // Professions travel on their own ROSTERPROF line(s) as "<skillId>=<value>" pairs: the
        // old "Name=value" list inside ROSTER pushed the line to ~290 bytes with every profession
        // and an active order, past the client's ~255-byte addon-whisper limit.
        std::vector<std::string> professionPairs;
        for (ProfessionSkillEntry const& entry : PROFESSION_SKILLS)
        {
            if (!bot->HasSkill(entry.skillId))
                continue;
            professionPairs.push_back(std::to_string(entry.skillId) + "=" + std::to_string(bot->GetSkillValue(entry.skillId)));
        }

        // "item N" was the raw entry id -- confirmed live this reads as meaningless noise to a
        // player ("gathering 20x item 2450") since nothing on the client side ever resolves it.
        // Real item names contain no ":" (the wire delimiter), so this is safe to inline as-is.
        auto itemName = [](uint32 itemEntry) -> std::string
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
            return proto ? proto->Name1 : ("item " + std::to_string(itemEntry));
        };

        std::string task = "idle";
        uint32 taskItemEntry = 0; // 0 == idle, no tooltip -- see docs/addon-protocol.md's ROSTER row
        // Appended Task Board fields (docs/addon-protocol.md): gather progress and whether the
        // asking player may Cancel/Top this row's gather order. 0:0:0 for any other task.
        uint32 orderGathered = 0, orderTarget = 0;
        bool canManage = false;
        if (auto craftItr = _craftOrders.find(bot->GetGUID()); craftItr != _craftOrders.end())
        {
            taskItemEntry = craftItr->second.itemEntry;
            task = "crafting " + std::to_string(craftItr->second.remainingCount) + "x " + itemName(taskItemEntry);
            if (SpellInfo const* recipe = sSpellMgr->GetSpellInfo(craftItr->second.spellId); recipe && !CrafterHasReagentsFor(bot, recipe))
                task += " - needs " + BotEconomy::DescribeReagents(BotEconomy::MissingReagents(bot, recipe, 1));
            orderTarget = craftItr->second.orderedCount;
            orderGathered = orderTarget - std::min(orderTarget, craftItr->second.remainingCount);
            canManage = CanManageGatherOrder(commander, guildId, craftItr->second.requesterGuid.GetCounter());
        }
        else if (auto gatherItr = _guildGatherOrders.find(bot->GetGUID()); gatherItr != _guildGatherOrders.end())
        {
            taskItemEntry = gatherItr->second.itemEntry;
            uint32 target = gatherItr->second.targetCount ? gatherItr->second.targetCount : 1;
            // Banked + carried: a bot that is gathering shows it right away, bank full or not.
            uint32 gathered = GatherOrderProgress(gatherItr->second);
            task = "gathering " + itemName(taskItemEntry) + " " + std::to_string(gathered) + "/" + std::to_string(target);
            if (gatherItr->second.bagsFull)
                task += " - bags full - making room";
            if (!gatherItr->second.status.empty())
                task += " - " + gatherItr->second.status;
            orderGathered = gathered;
            orderTarget = gatherItr->second.targetCount;
            canManage = CanManageGatherOrder(commander, guildId, gatherItr->second.requesterLowGuid);
        }
        else if (std::string goldTask = BotEconomy::GoldOrderTask(bot); !goldTask.empty())
            task = goldTask;

        // Trailing itemEntry field (added for the task board's item tooltip, see
        // docs/addon-protocol.md) -- task's item *name* was already resolved server-side above,
        // but a real GameTooltip needs the id, not just the name, to show icon/stats.
        //   ROSTER:<guid>:<name>:<class>:<level>:<task>:<professions, now always empty>:<itemEntry>:<gathered>:<target>:<canManage>
        //   ROSTERPROF:<guid>:<skillId>=<value>,...   (more ROSTERPROF lines if one would not fit; the
        //                                              client merges every line for that guid)
        // Field order is unchanged so 2.2.x clients still read everything but the professions. The
        // task is the only free text left on the line, so it gets whatever room the fixed fields leave.
        std::string const guidStr = std::to_string(bot->GetGUID().GetCounter());
        std::string const name = CoaBotWireText(bot->GetName(), 24, ":|");
        std::string const tail = "::" + std::to_string(taskItemEntry) + ":" + std::to_string(orderGathered) + ":" +
            std::to_string(orderTarget) + ":" + (canManage ? "1" : "0");
        std::string const head = "ROSTER:" + guidStr + ":" + name + ":" + std::to_string(uint32(bot->getClass())) + ":" +
            std::to_string(bot->GetLevel()) + ":";
        std::size_t const room = CoaBotWireRoom(head + tail);
        lines.push_back(head + CoaBotWireText(task, std::min<std::size_t>(room, 120), ":|") + tail);

        std::string const profHead = "ROSTERPROF:" + guidStr + ":";
        std::string profBody;
        for (std::string const& pair : professionPairs)
        {
            if (!profBody.empty() && profBody.size() + 1 + pair.size() > CoaBotWireRoom(profHead))
            {
                lines.push_back(profHead + profBody);
                profBody.clear();
            }
            if (!profBody.empty())
                profBody += ",";
            profBody += pair;
        }
        lines.push_back(profHead + profBody); // an empty list is sent too: "knows no professions"
    }

    // Queued gather orders (all eligible bots were busy when they were placed), front first:
    // "QUEUED:<queueId>:<itemEntry>:<count>:<position>:<canManage 0/1>:<requesterName>:<itemName>".
    uint32 position = 0;
    for (QueuedGatherOrder const& q : _gatherQueue)
    {
        if (q.guildId != guildId)
            continue;
        std::string requesterName;
        if (!q.requesterLowGuid || !sCharacterCache->GetCharacterNameByGuid(ObjectGuid::Create<HighGuid::Player>(q.requesterLowGuid), requesterName))
            requesterName = "?";
        std::string const queuedHead = "QUEUED:" + std::to_string(q.queueId) + ":" + std::to_string(q.itemEntry) + ":" + std::to_string(q.count) + ":" +
            std::to_string(++position) + ":" + (CanManageGatherOrder(commander, guildId, q.requesterLowGuid) ? "1" : "0") + ":" +
            CoaBotWireText(requesterName, 24, ":|") + ":";
        lines.push_back(queuedHead + CoaBotWireText(GatherItemName(q.itemEntry), CoaBotWireRoom(queuedHead), "|"));
    }
    return lines;
}

namespace
{
// Shared chunking helper for both catalog replies below -- packs by actual byte length rather
// than a fixed item count, so a reply body never risks exceeding WoW's real ~255-byte chat
// length cap no matter how long a particular item's name turns out to be (GetRecipeCatalog in
// particular draws from the full item_template, not just Trade Goods -- confirmed some item
// names on this realm run past 100 characters, so a fixed "N items per chunk" count sized only
// against gatherable materials' shorter names would not have been safe there).
constexpr std::size_t CATALOG_CHUNK_BUDGET_BYTES = 200;

void AppendCatalogChunks(std::vector<std::string>& lines, std::string const& prefix, std::vector<GatherCategoryItem> const& items)
{
    std::string body;
    for (GatherCategoryItem const& item : items)
    {
        // Names are cut to 100 bytes and lose any ":", "|" or "," so one entry can never break the
        // list or, on its own, outgrow the chunk budget.
        std::string entryStr = std::to_string(item.entry) + "," + CoaBotWireText(item.name, 100, ":|,");
        if (!body.empty() && body.size() + 1 + entryStr.size() > CATALOG_CHUNK_BUDGET_BYTES)
        {
            lines.push_back(prefix + body);
            body.clear();
        }
        if (!body.empty())
            body += "|";
        body += entryStr;
    }
    if (!body.empty())
        lines.push_back(prefix + body);
}

std::string RecipeProfessionName(uint32 spellId)
{
    auto bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
    for (auto itr = bounds.first; itr != bounds.second; ++itr)
    {
        uint32 skillLine = itr->second->SkillLine;
        for (ProfessionSkillEntry const& profession : PROFESSION_SKILLS)
            if (profession.skillId == skillLine)
                return profession.name;
    }
    return {};
}
}

std::vector<std::string> BotMgr::GetGatherCatalog() const
{
    std::vector<std::string> lines;
    for (auto const& [category, items] : GatherableCatalog())
        AppendCatalogChunks(lines, "GCAT:" + category + ":", items);
    return lines;
}

std::vector<std::string> BotMgr::GetRecipeCatalog(Player* commander) const
{
    std::vector<std::string> lines;
    uint32 guildId = commander ? commander->GetGuildId() : 0;
    if (!guildId)
        return lines;

    // Same "knows a recipe" definition CraftOrder itself uses (Player::HasSpell on a real
    // SPELL_EFFECT_CREATE_ITEM spell) -- only offer the addon items a guild-mate bot can
    // *actually* craft right now, per the user's explicit ask, not every recipe that exists.
    std::unordered_map<std::string, std::vector<GatherCategoryItem>> knownByProfession;
    std::unordered_set<uint32> seen;
    for (auto const& [itemEntry, spellIds] : CraftingRecipeIndex())
    {
        if (seen.count(itemEntry))
            continue;
        bool knownByGuildBot = false;
        std::string professionName;
        for (Player* bot : GetOnlineBots())
        {
            if (bot->GetGuildId() != guildId)
                continue;
            for (uint32 spellId : spellIds)
            {
                if (bot->HasSpell(spellId))
                {
                    knownByGuildBot = true;
                    std::string resolvedProfession = RecipeProfessionName(spellId);
                    if (!resolvedProfession.empty())
                        professionName = resolvedProfession;
                    if (!professionName.empty())
                        break;
                }
            }
            if (!professionName.empty())
                break;
        }
        if (knownByGuildBot)
        {
            if (professionName.empty())
                professionName = "Other";
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
            knownByProfession[professionName].push_back(
                { itemEntry, proto ? proto->Name1 : ("item " + std::to_string(itemEntry)) });
            seen.insert(itemEntry);
        }
    }

    auto appendProfession = [&](std::string const& professionName)
    {
        auto itr = knownByProfession.find(professionName);
        if (itr == knownByProfession.end())
            return;
        std::vector<GatherCategoryItem>& items = itr->second;
        std::sort(items.begin(), items.end(),
            [](GatherCategoryItem const& a, GatherCategoryItem const& b) { return a.name < b.name; });
        AppendCatalogChunks(lines, "RCAT:" + professionName + ":", items);
    };
    for (ProfessionSkillEntry const& profession : PROFESSION_SKILLS)
        appendProfession(profession.name);
    appendProfession("Other");
    return lines;
}

void BotMgr::DumpRecipeCoverage(ChatHandler* handler) const
{
    if (!handler)
        return;

    std::vector<Player*> bots = GetOnlineBots();

    std::unordered_map<uint32, std::unordered_set<uint32>> itemsBySkillLine;
    std::unordered_set<uint32> itemsUnresolvedSkillLine;

    for (auto const& [itemEntry, spellIds] : CraftingRecipeIndex())
    {
        uint32 resolvedSkillLine = 0;
        bool anyBotKnows = false;
        for (uint32 spellId : spellIds)
        {
            for (Player* bot : bots)
            {
                if (bot->HasSpell(spellId))
                {
                    anyBotKnows = true;
                    break;
                }
            }
            if (anyBotKnows)
            {
                auto bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
                if (bounds.first != bounds.second)
                    resolvedSkillLine = bounds.first->second->SkillLine;
                break;
            }
        }
        if (!anyBotKnows)
            continue;
        if (resolvedSkillLine)
            itemsBySkillLine[resolvedSkillLine].insert(itemEntry);
        else
            itemsUnresolvedSkillLine.insert(itemEntry);
    }

    handler->PSendSysMessage("BotMgr: recipe coverage across {} online bot(s) (live sSpellMgr/HasSpell data, not the stale spell_dbc SQL export):", uint32(bots.size()));
    for (ProfessionSkillEntry const& entry : PROFESSION_SKILLS)
    {
        auto itr = itemsBySkillLine.find(entry.skillId);
        uint32 count = itr != itemsBySkillLine.end() ? uint32(itr->second.size()) : 0;
        handler->PSendSysMessage("  {}: {} distinct craftable item(s)", entry.name, count);
    }
    handler->PSendSysMessage("  (custom/unresolved skill line): {} distinct craftable item(s)", uint32(itemsUnresolvedSkillLine.size()));
}

static std::string CraftCastFailureText(SpellCastResult result)
{
    switch (result)
    {
        case SPELL_FAILED_TOTEMS:
        case SPELL_FAILED_TOTEM_CATEGORY:
            return "it does not carry the tool the recipe needs";
        case SPELL_FAILED_REQUIRES_SPELL_FOCUS:
            return "it is not standing at the anvil, forge or fire the recipe needs";
        case SPELL_FAILED_REAGENTS:
            return "it is missing reagents";
        case SPELL_FAILED_TOO_MANY_OF_ITEM:
            return "its bags have no room for the result";
        case SPELL_FAILED_MIN_SKILL:
        case SPELL_FAILED_LOW_CASTLEVEL:
            return "its skill is too low";
        default:
            return Acore::StringFormat("the cast failed (result {})", uint32(result));
    }
}

bool BotMgr::NoteCraftFailure(Player* crafter, CraftOrderState& order, std::string const& why)
{
    constexpr uint32 RETRY_DELAY_MS = 5 * 1000;
    order.nextCastAtMs = GameTime::GetGameTimeMS().count() + RETRY_DELAY_MS;
    ++order.failedCasts;
    if (!BotEconomyPolicy::GiveUpAfterFailures(order.failedCasts, BotEconomy::CraftMaxCastFailures()))
        return true;
    if (Player* requester = ObjectAccessor::FindPlayer(order.requesterGuid))
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(order.itemEntry);
        ChatHandler(requester->GetSession()).PSendSysMessage("|cffff6060Crafting order:|r {} gave up on {}x {} after {} failed tries: {}.",
            crafter->GetName(), order.remainingCount, proto ? proto->Name1 : std::to_string(order.itemEntry), order.failedCasts, why);
    }
    LOG_INFO("module.coa-playerbots", "BotMgr: craft order for bot '{}' (spell {}) given up after {} failed casts: {}.", crafter->GetName(),
        order.spellId, order.failedCasts, why);
    return false;
}

// A crafter short of reagents fetches them once a minute (guild bank, vendors, auction house -- whichever
// CoaBots.Crafting.* allows), tells the requester once what is still missing, and gives the order up after
// CoaBots.Crafting.OrderTimeoutMin. Returns false when the order is given up.
bool BotMgr::WaitForCraftReagents(Player* crafter, SpellInfo const* info, CraftOrderState& order)
{
    constexpr uint32 SOURCING_INTERVAL_MS = 60 * 1000;
    uint32 const now = GameTime::GetGameTimeMS().count();
    if (!order.placedAtMs)
        order.placedAtMs = std::max<uint32>(1, now);
    if (int32(now - order.nextSourcingAtMs) < 0)
        return true;
    order.nextSourcingAtMs = now + SOURCING_INTERVAL_MS;

    BotEconomy::CollectMailFrom(crafter, order.requesterGuid);
    if (CrafterHasReagentsFor(crafter, info))
        return true;
    std::vector<BotEconomy::ReagentShortfall> const missing =
        BotEconomy::SourceReagents(crafter, info, order.remainingCount, order.reagentBudgetLeftCopper);
    if (missing.empty())
        return true;

    Player* requester = ObjectAccessor::FindPlayer(order.requesterGuid);
    std::string const itemName = [&]() -> std::string
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(order.itemEntry);
        return proto ? proto->Name1 : std::to_string(order.itemEntry);
    }();
    uint32 const timeout = BotEconomy::CraftOrderTimeoutMs();
    if (timeout && now - order.placedAtMs > timeout)
    {
        if (requester)
            ChatHandler(requester->GetSession()).PSendSysMessage(
                "|cffff6060Crafting order:|r {} gave up on {}x {} -- still missing {}.", crafter->GetName(), order.remainingCount, itemName,
                BotEconomy::DescribeReagents(missing));
        LOG_INFO("module.coa-playerbots", "BotMgr: craft order for bot '{}' (spell {}) given up after {} min without reagents.",
            crafter->GetName(), order.spellId, timeout / 60000);
        return false;
    }
    if (!order.toldMissing && requester)
    {
        order.toldMissing = true;
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffffcc00Crafting order:|r {} needs {} to craft {}. Mail them to {} and it starts.", crafter->GetName(),
            BotEconomy::DescribeReagents(missing), itemName, crafter->GetName());
    }
    return true;
}

// Called from Update() every tick, same cadence as the guildgather order drain. Casts are
// naturally throttled to "at most one per bot per tick" just by this loop's own shape -- no
// separate cooldown tracking needed since tradeskill casts aren't on the GCD.
void BotMgr::ProcessCraftOrders()
{
    if (_craftOrders.empty())
        return;

    for (auto itr = _craftOrders.begin(); itr != _craftOrders.end(); )
    {
        Player* crafter = FindBotPlayer(itr->first.GetCounter());
        if (!crafter || CoaTraining::Owns(crafter->GetGUID()) || !crafter->IsInWorld())
        {
            ++itr;
            continue;
        }

        SpellInfo const* info = sSpellMgr->GetSpellInfo(itr->second.spellId);
        if (!info)
        {
            itr = _craftOrders.erase(itr);
            continue;
        }

        uint32 itemEntry = itr->second.itemEntry;

        // A cast was already fired on an earlier tick -- don't start another one on top of it
        // (some tradeskill recipes have a real cast time, not just instant ones; recasting
        // mid-cast would interrupt and restart it forever, never letting it finish). Wait for
        // it to actually finish, then check whether it produced anything.
        if (itr->second.awaitingCastResult)
        {
            if (crafter->IsNonMeleeSpellCast(false))
            {
                ++itr; // still casting -- check again next tick
                continue;
            }

            itr->second.awaitingCastResult = false;
            uint32 const nowCount = crafter->GetItemCount(itemEntry, false);
            uint32 producedThisCast = nowCount > itr->second.itemCountBeforeCast ? nowCount - itr->second.itemCountBeforeCast : 0;
            if (producedThisCast)
                itr->second.failedCasts = 0;
            if (!producedThisCast)
            {
                LOG_INFO("module.coa-playerbots", "BotMgr: craft order cast for bot '{}' (spell {}) finished but produced nothing -- will retry.",
                    crafter->GetName(), itr->second.spellId);
                if (!NoteCraftFailure(crafter, itr->second, "the cast finished without making anything"))
                    itr = _craftOrders.erase(itr);
                else
                    ++itr;
                continue;
            }

            itr->second.remainingCount = (producedThisCast >= itr->second.remainingCount) ? 0 : itr->second.remainingCount - producedThisCast;
            if (itr->second.remainingCount > 0)
            {
                ++itr;
                continue;
            }
            // falls through to the mail-and-complete block below
        }
        else
        {
            if (!CrafterHasReagentsFor(crafter, info))
            {
                if (!WaitForCraftReagents(crafter, info, itr->second))
                {
                    itr = _craftOrders.erase(itr);
                    continue;
                }
                ++itr; // still waiting on reagents -- retry next tick
                continue;
            }

            if (int32(GameTime::GetGameTimeMS().count() - itr->second.nextCastAtMs) < 0)
            {
                ++itr;
                continue;
            }
            itr->second.itemCountBeforeCast = crafter->GetItemCount(itemEntry, false);
            SpellCastResult result = crafter->CastSpell(crafter, itr->second.spellId, false);
            if (result != SPELL_CAST_OK)
            {
                LOG_INFO("module.coa-playerbots", "BotMgr: craft order cast failed for bot '{}' (spell {}, result {}) -- will retry.",
                    crafter->GetName(), itr->second.spellId, uint32(result));
                if (!NoteCraftFailure(crafter, itr->second, CraftCastFailureText(result)))
                    itr = _craftOrders.erase(itr);
                else
                    ++itr;
                continue;
            }

            itr->second.awaitingCastResult = true;
            ++itr; // check back once the cast (instant or not) has actually resolved
            continue;
        }

        // remainingCount == 0 here -- order complete: mail every matching stack currently in
        // the crafter's bags (the items this order itself just produced -- nothing else grants
        // this same itemEntry to a bot mid-order) to the requester.
        ObjectGuid::LowType requesterLowGuid = itr->second.requesterGuid.GetCounter();
        uint32 mailed = 0;

        auto mailIfMatch = [&](Item* item)
        {
            if (item && item->GetEntry() == itemEntry)
            {
                mailed += item->GetCount();
                MailCraftedItem(crafter, requesterLowGuid, item, "Crafting order complete");
            }
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            mailIfMatch(crafter->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            if (Bag* pBag = crafter->GetBagByPos(bag))
                for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
                    mailIfMatch(pBag->GetItemByPos(j));

        LOG_INFO("module.coa-playerbots", "BotMgr: craft order complete -- bot '{}' mailed {}x item {} to guid {}.",
            crafter->GetName(), mailed, itemEntry, requesterLowGuid);
        itr = _craftOrders.erase(itr);
    }
}

void BotMgr::Invite(ObjectGuid::LowType charLowGuid, std::string const& targetName, ChatHandler* handler)
{
    WorldSession* session = FindBotSession(charLowGuid);
    if (!session)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: no active bot session for guid {} (spawn it first).", charLowGuid);
        return;
    }

    Player* bot = session->GetPlayer();
    if (!bot)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: bot session for guid {} has no Player yet (login still pending?).", charLowGuid);
        return;
    }

    // CMSG_GROUP_INVITE's real body: a null-terminated target name, then a padding uint32
    // (unused by the handler) -- see WorldSession::HandleGroupInviteOpcode.
    WorldPacket packet;
    packet << targetName;
    packet << uint32(0);
    session->HandleGroupInviteOpcode(packet);

    if (handler)
        handler->PSendSysMessage("BotMgr: '{}' invited '{}' -- check .botcmd acceptinvite on the target if it's another bot.", bot->GetName(), targetName);
}

void BotMgr::Kill(ObjectGuid::LowType charLowGuid, ChatHandler* handler)
{
    WorldSession* session = FindBotSession(charLowGuid);
    Player* bot = session ? session->GetPlayer() : nullptr;
    if (!bot)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: no active bot session for guid {} (spawn it first).", charLowGuid);
        return;
    }

    if (!bot->IsAlive())
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: bot '{}' is already dead.", bot->GetName());
        return;
    }

    Unit::Kill(bot, bot);

    if (handler)
        handler->PSendSysMessage("BotMgr: bot '{}' killed.", bot->GetName());
}

void BotMgr::DespawnBot(ObjectGuid::LowType charLowGuid, ChatHandler* handler)
{
    auto itr = std::find_if(_botSessions.begin(), _botSessions.end(), [charLowGuid](WorldSession* session)
    {
        Player* bot = session->GetPlayer();
        return bot && bot->GetGUID().GetCounter() == charLowGuid;
    });

    if (itr == _botSessions.end())
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: no active bot session for guid {}.", charLowGuid);
        return;
    }

    WorldSession* session = *itr;
    std::string name = session->GetPlayer() ? session->GetPlayer()->GetName() : "?";

    // Same shape as the failure-path cleanup already used in SpawnBot: LogoutPlayer
    // saves+removes the Player from world, but does not delete the WorldSession
    // itself -- that's still on us, same as any other owner of a WorldSession.
    ObjectGuid botGuid = session->GetPlayer() ? session->GetPlayer()->GetGUID() : ObjectGuid::Empty;

    session->LogoutPlayer(true);
    delete session;
    _botSessions.erase(itr);
    _botSessionByLowGuid.erase(charLowGuid);
    _aiWaitMs.erase(botGuid);
    CoaBotScheduling::Forget(botGuid);
    // Also drop it from the pending-teleport-ack queue if it's there -- otherwise
    // the next Update() tick would call FinishPendingTeleport on a freed session.
    _pendingTeleportAck.erase(std::remove(_pendingTeleportAck.begin(), _pendingTeleportAck.end(), session), _pendingTeleportAck.end());
    BotAI::Forget(botGuid);

    LOG_INFO("module.coa-playerbots", "BotMgr: despawned bot '{}' (guid {}).", name, charLowGuid);
    if (handler)
        handler->PSendSysMessage("BotMgr: bot '{}' despawned.", name);
}

uint32 BotMgr::LogoutAllBots()
{
    _pendingAutoLoginQueue.clear();
    uint32 saved = 0;
    // The sessions themselves are left alone: a login still in flight holds a pointer to its session, and the
    // process is about to end anyway.
    for (WorldSession* session : _botSessions)
    {
        if (!session->GetPlayer())
            continue;
        session->LogoutPlayer(true);
        ++saved;
    }
    _aiWaitMs.clear();
    CoaBotScheduling::Clear();
    LOG_INFO("module.coa-playerbots", "BotMgr: saved and logged out {} companion(s) for shutdown.", saved);
    return saved;
}

uint32 BotMgr::DespawnAllBots()
{
    _pendingAutoLoginQueue.clear();
    std::vector<ObjectGuid::LowType> onlineGuids;
    onlineGuids.reserve(_botSessions.size());
    for (WorldSession* session : _botSessions)
        if (Player* bot = session->GetPlayer())
            onlineGuids.push_back(bot->GetGUID().GetCounter());
    for (ObjectGuid::LowType guid : onlineGuids)
        DespawnBot(guid, nullptr);
    return uint32(onlineGuids.size());
}

void BotMgr::PurgeCustomRaceBots(ChatHandler* handler)
{
    std::string accountPrefix = sConfigMgr->GetOption<std::string>("CoaBots.RandomSpawn.AccountPrefix", "CoaBotHost");
    QueryResult accounts = LoginDatabase.Query("SELECT id FROM account WHERE id > 2 AND username LIKE '{}%'", accountPrefix);
    if (!accounts)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: deleted 0 custom race bot(s).");
        return;
    }
    std::ostringstream accountIdList;
    do
    {
        accountIdList << (*accounts)[0].Get<uint32>() << ",";
    } while (accounts->NextRow());
    std::string idList = accountIdList.str();
    idList.pop_back();

    QueryResult chars = CharacterDatabase.Query(
        "SELECT guid, account FROM characters WHERE account IN ({}) AND race > 11", idList);
    uint32 deleted = 0;
    if (chars)
    {
        do
        {
            Field* fields = chars->Fetch();
            ObjectGuid::LowType guid = fields[0].Get<uint32>();
            uint32 accountId = fields[1].Get<uint32>();
            if (FindBotSession(guid))
                DespawnBot(guid, nullptr);
            Player::DeleteFromDB(guid, accountId, true, true);
            ++deleted;
        } while (chars->NextRow());
    }
    LOG_INFO("module.coa-playerbots", "BotMgr: deleted {} custom race bot(s).", deleted);
    if (handler)
        handler->PSendSysMessage("BotMgr: deleted {} custom race bot(s).", deleted);
}

void BotMgr::PurgeAllBots(ChatHandler* handler)
{
    // Despawn every online bot first -- every session in _botSessions is one of ours (BotMgr
    // never tracks a real client's session here), and DeleteFromDB below would otherwise be
    // deleting a character row still logged in.
    std::vector<ObjectGuid::LowType> onlineGuids;
    onlineGuids.reserve(_botSessions.size());
    for (WorldSession* session : _botSessions)
        if (Player* bot = session->GetPlayer())
            onlineGuids.push_back(bot->GetGUID().GetCounter());
    for (ObjectGuid::LowType guid : onlineGuids)
        DespawnBot(guid, nullptr);

    // Same account lookup QueueAllBotsForAutoLogin already uses to find every bot-hosting
    // account, so this catches bots that were never logged in this session too, not just the
    // ones just despawned above.
    std::string accountPrefix = sConfigMgr->GetOption<std::string>("CoaBots.RandomSpawn.AccountPrefix", "CoaBotHost");
    QueryResult accounts = LoginDatabase.Query("SELECT id FROM account WHERE id > 2 AND username LIKE '{}%'", accountPrefix);
    if (!accounts)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: no bot-hosting accounts found, nothing to purge.");
        return;
    }

    std::vector<uint32> accountIds;
    std::ostringstream accountIdList;
    do
    {
        uint32 accountId = (*accounts)[0].Get<uint32>();
        accountIds.push_back(accountId);
        accountIdList << accountId << ",";
    } while (accounts->NextRow());
    std::string idList = accountIdList.str();
    idList.pop_back();

    QueryResult chars = CharacterDatabase.Query(
        "SELECT guid, account FROM characters WHERE account IN ({})", idList);
    uint32 deleted = 0;
    if (chars)
    {
        do
        {
            Field* fields = chars->Fetch();
            ObjectGuid::LowType guid = fields[0].Get<uint32>();
            uint32 accountId = fields[1].Get<uint32>();
            // deleteFinally=true: a hard delete, not the recycle-bin soft delete a real player's
            // ".character erase" leaves behind -- there's no reason to keep a purged bot's row
            // around, and leaving it would just block CharacterCreate from reusing its name.
            Player::DeleteFromDB(guid, accountId, true, true);
            ++deleted;
        } while (chars->NextRow());
    }

    // Use the core account lifecycle, including bans, mute records and script hooks.
    // Retain shared accounts when another realm still owns characters.
    uint32 deletedAccounts = 0;
    for (uint32 accountId : accountIds)
    {
        QueryResult otherRealm = LoginDatabase.Query(
            "SELECT 1 FROM realmcharacters WHERE acctid = {} AND realmid <> {} AND numchars > 0 LIMIT 1",
            accountId, realm.Id.Realm);
        if (!otherRealm && AccountMgr::DeleteAccount(accountId) == AOR_OK)
            ++deletedAccounts;
    }

    LOG_INFO("module.coa-playerbots", "BotMgr: purged {} bot character(s) and {} bot-hosting account(s).",
        deleted, deletedAccounts);
    BotSpawn::ResetAccountLookupCache();
    if (handler)
        handler->PSendSysMessage("BotMgr: purged {} bot character(s) and {} bot account(s). Use .botcmd spawnrandom or "
            ".botcmd spawnleveled to create a fresh population.", deleted, deletedAccounts);
}

void BotMgr::ListAuras(ObjectGuid::LowType charLowGuid, ChatHandler* handler)
{
    Player* target = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(charLowGuid));
    if (!target)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: no online player with guid {} found.", charLowGuid);
        return;
    }

    if (handler)
    {
        handler->PSendSysMessage("BotMgr: auras on '{}' (guid {}), health {}/{} (mana {}/{}):",
            target->GetName(), charLowGuid,
            target->GetHealth(), target->GetMaxHealth(),
            target->GetPower(POWER_MANA), target->GetMaxPower(POWER_MANA));
        handler->PSendSysMessage("  Intellect={:.2f} Spirit={:.2f} manaRegenFlat={:.4f} manaRegenInterruptedFlat={:.4f} inCombat={}",
            target->GetStat(STAT_INTELLECT), target->GetStat(STAT_SPIRIT),
            target->GetFloatValue(static_cast<uint16>(UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER) + AsUnderlyingType(POWER_MANA)),
            target->GetFloatValue(static_cast<uint16>(UNIT_FIELD_POWER_REGEN_INTERRUPTED_FLAT_MODIFIER) + AsUnderlyingType(POWER_MANA)),
            target->IsInCombat());
        handler->PSendSysMessage("  meleeCrit={:.4f}% offhandCrit={:.4f}% rangedCrit={:.4f}% expertise={} offhandExpertise={} ratingMultCritMelee={:.4f}",
            target->GetFloatValue(PLAYER_CRIT_PERCENTAGE),
            target->GetFloatValue(PLAYER_OFFHAND_CRIT_PERCENTAGE),
            target->GetFloatValue(PLAYER_RANGED_CRIT_PERCENTAGE),
            target->GetUInt32Value(PLAYER_EXPERTISE),
            target->GetUInt32Value(PLAYER_OFFHAND_EXPERTISE),
            target->GetRatingMultiplier(CR_CRIT_MELEE));
        Item* rangedWeapon = target->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
        handler->PSendSysMessage("  rangedWeaponEquipped={} mainWeaponEquipped={}",
            rangedWeapon ? "yes" : "no",
            target->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND) ? "yes" : "no");
    }

    uint32 count = 0;
    for (auto const& [spellId, application] : target->GetAppliedAuras())
    {
        Aura* aura = application->GetBase();
        SpellInfo const* spellInfo = aura->GetSpellInfo();
        bool preventsRegen = aura->HasEffectType(SPELL_AURA_PREVENT_REGENERATE_POWER);

        if (handler)
            handler->PSendSysMessage("  [{}] '{}' stacks={} duration={}ms{}",
                spellId,
                spellInfo->SpellName[handler->GetSessionDbcLocale()],
                aura->GetStackAmount(),
                aura->GetDuration(),
                preventsRegen ? " <-- PREVENT_REGENERATE_POWER" : "");
        ++count;
    }

    if (count == 0 && handler)
        handler->PSendSysMessage("  (no auras)");

    LOG_INFO("module.coa-playerbots", "BotMgr: ListAuras dumped {} auras for '{}' (guid {}).", count, target->GetName(), charLowGuid);
}

void BotMgr::RunChatCommand(ObjectGuid::LowType charLowGuid, std::string const& command, ChatHandler* handler)
{
    Player* target = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(charLowGuid));
    if (!target)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: no online player with guid {} found.", charLowGuid);
        return;
    }

    ChatHandler asPlayer(target->GetSession());
    bool result = asPlayer.ParseCommands(command);

    if (handler)
        handler->PSendSysMessage("BotMgr: ran '{}' as '{}', ParseCommands returned {}.", command, target->GetName(), result);
}

void BotMgr::HasSpells(ObjectGuid::LowType charLowGuid, std::vector<uint32> const& spellIds, ChatHandler* handler)
{
    Player* target = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(charLowGuid));
    if (!target)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: no online player with guid {} found.", charLowGuid);
        return;
    }

    if (!handler)
        return;

    for (uint32 spellId : spellIds)
        handler->PSendSysMessage("  HasSpell({}) = {}", spellId, target->HasSpell(spellId));
}

void BotMgr::AttackNearestHostile(ObjectGuid::LowType charLowGuid, float range, ChatHandler* handler)
{
    WorldSession* session = FindBotSession(charLowGuid);
    Player* bot = session ? session->GetPlayer() : nullptr;
    if (!bot)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: no active bot session for guid {}.", charLowGuid);
        return;
    }

    if (range <= 0.0f)
        range = 30.0f;

    // Same "nearest hostile via a grid searcher" shape as Creature::SelectNearestTarget, using
    // a local Unit-compatible reimplementation of Acore::NearestHostileUnitCheck (that helper's
    // own check type only accepts a Creature const*, a Player can't use it directly). This
    // used to use AnyUnfriendlyUnitInObjectRangeCheck + UnitLastSearcher instead, which finds
    // whichever hostile the grid traversal happens to visit *last* -- not the nearest one --
    // so a bot could "attack nearest" and end up charging a target on the far side of its
    // search radius while a real nearest enemy stood right next to it.
    Unit* target = nullptr;
    NearestHostileUnitInObjectRangeCheck check(bot, range);
    Acore::UnitLastSearcher<NearestHostileUnitInObjectRangeCheck> searcher(bot, target, check);
    Cell::VisitObjects(bot, searcher, range);

    if (!target)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: no hostile unit found within {} yards of '{}'.", range, bot->GetName());
        return;
    }

    LOG_INFO("module.coa-playerbots", "BotMgr: pre-Attack diagnostics: botAlive={} targetAlive={} sameMap={} samePhase={} (botPhase={} targetPhase={}) botMounted={} priorVictim={}.",
        bot->IsAlive(), target->IsAlive(), bot->IsInMap(target), bot->InSamePhase(target),
        bot->GetPhaseMask(), target->GetPhaseMask(), bot->IsMounted(),
        bot->GetVictim() ? bot->GetVictim()->GetName() : "<null>");
    bool attacked = bot->Attack(target, true);
    LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: Attack() on '{}' returned {}; GetVictim() is now {}; attackers count {}.",
        target->GetName(), attacked, bot->GetVictim() ? bot->GetVictim()->GetName() : "<null>", bot->getAttackers().size());
    if (handler)
        handler->PSendSysMessage("BotMgr: bot '{}' is now attacking '{}' (Attack() returned {}).", bot->GetName(), target->GetName(), attacked);
}

void BotMgr::SetRole(ObjectGuid::LowType charLowGuid, std::string const& roleName, ChatHandler* handler)
{
    WorldSession* session = FindBotSession(charLowGuid);
    Player* bot = session ? session->GetPlayer() : nullptr;
    if (!bot)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: no active bot session for guid {}.", charLowGuid);
        return;
    }

    std::string normalized = roleName;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);

    if (normalized == "auto")
    {
        BotAI::ClearRoleOverride(bot->GetGUID());
        uint32 activeSpec = bot->GetPlayerSetting("core.ascension_active_spec", 0).value;
        BotRole autoRole = BotAI::GetRoleForClassSpec(bot->getClass(), activeSpec);
        char const* specName = BotAI::GetSpecName(bot->getClass(), activeSpec);
        char const* roleStr = RoleToString(autoRole);
        LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr::SetRole: bot '{}' role reset to auto (detected {} from spec {} '{}').",
            bot->GetName(), roleStr, activeSpec, specName ? specName : "unknown");
        if (handler)
            handler->PSendSysMessage("BotMgr: bot '{}' role reset to auto (detected {} from spec {} '{}').",
                bot->GetName(), roleStr, activeSpec, specName ? specName : "unknown");
        return;
    }

    BotRole role;
    if (normalized == "dps")
        role = BotRole::Dps;
    else if (normalized == "tank")
        role = BotRole::Tank;
    else if (normalized == "healer")
        role = BotRole::Healer;
    else if (normalized == "support")
        role = BotRole::Support;
    else
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: unknown role '{}' -- expected dps, tank, healer, support, or auto.", roleName);
        return;
    }

    // A role pick must actually correspond to a spec this class has -- most classes have no
    // Tank spec at all, several have no Healer spec either (see ClassSpecRoles.cpp's table).
    // Picking one of those roles anyway would just leave the bot's spec/talents mismatched
    // with what BotAI::Update now thinks its role is, silently breaking its rotation. If the
    // bot's current spec already satisfies the requested role, FindSpecForRole returns it
    // unchanged and no spec switch happens at all.
    uint32 currentSpec = bot->GetPlayerSetting("core.ascension_active_spec", 0).value;
    uint32 targetSpec = BotAI::FindSpecForRole(bot->getClass(), role, currentSpec);
    if (!targetSpec)
    {
        LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr::SetRole: bot '{}' (class {}) has no {} spec available -- role change refused.",
            bot->GetName(), uint32(bot->getClass()), normalized);
        if (handler)
            handler->PSendSysMessage("BotMgr: bot '{}' has no {} spec available for its class -- role change refused.",
                bot->GetName(), normalized);
        return;
    }

    if (targetSpec != currentSpec)
        LearnSpecialization(charLowGuid, targetSpec, handler);

    BotAI::SetRole(bot->GetGUID(), role);
    LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr::SetRole: bot '{}' role manually set to {} (guid {}).",
        bot->GetName(), normalized, charLowGuid);
    if (handler)
        handler->PSendSysMessage("BotMgr: bot '{}' role manually set to {}.", bot->GetName(), normalized);
}

void BotMgr::CheckRole(ObjectGuid::LowType charLowGuid, ChatHandler* handler)
{
    WorldSession* session = FindBotSession(charLowGuid);
    Player* bot = session ? session->GetPlayer() : nullptr;
    if (!bot)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: no active bot session for guid {}.", charLowGuid);
        return;
    }

    BotRole role = BotAI::GetRole(bot->GetGUID());
    char const* roleStr = RoleToString(role);
    uint32 activeSpec = bot->GetPlayerSetting("core.ascension_active_spec", 0).value;
    char const* specName = BotAI::GetSpecName(bot->getClass(), activeSpec);

    if (handler)
        handler->PSendSysMessage("BotMgr: bot '{}' effective role is {} (spec {} '{}').",
            bot->GetName(), roleStr, activeSpec, specName ? specName : "unknown");

    BotAI::ReportSpellbookRoleSignals(bot, handler);
}

void BotMgr::LearnSpecialization(ObjectGuid::LowType charLowGuid, uint32 specId, ChatHandler* handler)
{
    WorldSession* session = FindBotSession(charLowGuid);
    Player* bot = session ? session->GetPlayer() : nullptr;
    if (!bot)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: no active bot session for guid {}.", charLowGuid);
        return;
    }

    // Real spec switch -- AscensionClassService::SwitchSpecialization() itself already
    // strips every talent spell belonging to whichever spec was previously active (both
    // paid and automatic) and updates the in-memory active-spec map GetActiveSpecialization()
    // reads, not just the "core.ascension_active_spec" PlayerSetting a raw write here used to
    // leave as the only record of the change. See docs/core-patches.md's "Patch 3".
    // specId == 0 ("shared tree only, no spec chosen yet") is deliberately NOT passed to
    // SwitchSpecialization -- the real function rejects 0 outright (it means "no spec"),
    // and shared-tree (SpecId == 0) entries below don't need an active spec at all to be set.
    if (specId != 0)
    {
        if (!AscensionClassServiceBridge::SwitchSpecialization(bot, specId))
        {
            if (handler)
                handler->PSendSysMessage("BotMgr: specialization {} is not valid for bot '{}' (class {}).",
                    specId, bot->GetName(), uint32(bot->getClass()));
            LOG_ERROR(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: SwitchSpecialization rejected spec {} for bot '{}' (class {}).",
                specId, bot->GetName(), uint32(bot->getClass()));
            return;
        }
    }
    else
    {
        // specId == 0 has no real SwitchSpecialization equivalent (the real function
        // rejects 0 outright -- it means "no spec"), so drop any spec-specific talent this
        // bot currently knows by hand here, the same way SwitchSpecialization would for a
        // real respec, and leave the shared tree (handled by SetTalentRank below, which
        // doesn't require an active spec for SpecId == 0 entries) untouched.
        for (AscensionCompatData::CoATalentEntry const& entry : AscensionCompatData::CoATalentEntries)
        {
            if (entry.ClassId != bot->getClass() || entry.SpecId == 0)
                continue;
            for (uint32 spellId : entry.SpellIds)
                if (spellId && bot->HasSpell(spellId))
                    bot->removeSpell(spellId, SPEC_MASK_ALL, false);
        }
    }

    uint32 learned = 0;
    uint32 skipped = 0;
    if (BotTalentBuilds::GetBuild(bot->getClass(), specId))
    {
        BotTalentBuilds::ApplyBuildForLevel(bot, bot->GetLevel());
    }
    else
    {
        for (AscensionCompatData::CoATalentEntry const& entry : AscensionCompatData::CoATalentEntries)
        {
            if (entry.ClassId != bot->getClass())
                continue;
            if (entry.SpecId != 0 && entry.SpecId != specId)
                continue;
            // AECost==0 && TECost==0 entries are "automatic" -- mod-ascension-compat's own
            // SynchronizeProgression grants those itself now that the spec switch above has
            // gone through the real path. We only need to reach the paid ones here.
            if (entry.AECost == 0 && entry.TECost == 0)
                continue;
            if (entry.RequiredLevel > bot->GetLevel() || !entry.SpellCount)
                continue;

            // Set to the entry's highest rank through the real budget-checked path instead
            // of learnSpell()-ing it directly -- a failure here (most commonly: the
            // class/spec talent-point budget is already spent at this level) is expected
            // and just means this bot can't afford every paid entry in the spec yet,
            // exactly like a real player at the same level.
            std::string error;
            if (AscensionClassServiceBridge::SetTalentRank(bot, entry, entry.SpellCount, error))
                ++learned;
            else
                ++skipped;
        }
    }

    char const* specName = BotAI::GetSpecName(bot->getClass(), specId);
    BotRole autoRole = BotAI::GetRoleForClassSpec(bot->getClass(), specId);
    char const* roleStr = RoleToString(autoRole);

    BotAI::SpecStrategyRegistry::ForgetBot(bot->GetGUID());
    BotAI::ActionEvaluator::ClearThrottles(bot->GetGUID());

    if (handler)
        handler->PSendSysMessage("BotMgr: bot '{}' spent {} talent(s) ({} skipped, budget or other limit) for specialization {} '{}' (detected role: {}).",
            bot->GetName(), learned, skipped, specId, specName ? specName : "unknown", roleStr);
    LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: bot '{}' (class {}) spent {} talent(s), skipped {}, for spec {} '{}' (role: {}).",
        bot->GetName(), uint32(bot->getClass()), learned, skipped, specId, specName ? specName : "unknown", roleStr);
}

void BotMgr::QuickFillGroup(Player* commander, ChatHandler* handler)
{
    constexpr uint32 TARGET_TOTAL = 5;
    constexpr uint32 TARGET_TANKS = 1;
    constexpr uint32 TARGET_HEALERS = 1;
    constexpr uint32 TARGET_DPS = 3;

    if (!commander)
        return;

    Group* group = commander->GetGroup();

    // Tally roles already covered by bots currently in the group -- real (non-bot) members
    // are counted toward the group's total size but not toward any specific role bucket:
    // there's no reliable way to infer a real player's role from here, and guessing wrong
    // would under-invite a bucket that's actually still empty.
    uint32 haveTanks = 0, haveHealers = 0, haveDps = 0;
    uint32 currentSize = 1; // just commander, if solo
    if (group)
    {
        currentSize = group->GetMembersCount();
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member)
                continue;
            if (!sBotMgr->FindBotPlayer(member->GetGUID().GetCounter()))
                continue; // real player -- see comment above

            switch (BotAI::GetRole(member->GetGUID()))
            {
                case BotRole::Tank:   ++haveTanks;   break;
                case BotRole::Healer: ++haveHealers; break;
                default:              ++haveDps;     break;
            }
        }
    }

    uint32 slotsLeft = (TARGET_TOTAL > currentSize) ? TARGET_TOTAL - currentSize : 0;
    if (!slotsLeft)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: group is already full ({} members).", currentSize);
        return;
    }

    uint32 needTanks = (haveTanks < TARGET_TANKS) ? TARGET_TANKS - haveTanks : 0;
    uint32 needHealers = (haveHealers < TARGET_HEALERS) ? TARGET_HEALERS - haveHealers : 0;
    uint32 needDps = (haveDps < TARGET_DPS) ? TARGET_DPS - haveDps : 0;

    // Candidate pool: any online bot not already in a group (this one or another), same
    // faction as the commander, still alive. Scored (not filtered) by guild membership and
    // level/gear closeness so the best-fit candidate wins per role, not just the first found.
    std::vector<Player*> pool;
    for (Player* bot : GetOnlineBots())
    {
        if (bot == commander)
            continue; // commander may itself be a tracked bot session (RA/.botcmd testing) -- never a candidate for its own group
        if (!bot->IsInWorld() || !bot->IsAlive())
            continue;
        if (bot->GetTeamId() != commander->GetTeamId())
            continue;
        if (bot->GetGroup())
        {
            if (group && bot->GetGroup() == group)
                continue;
            // Probe only: do not leave yet. A bot we will not pick must stay in its group.
            // CanTake is the same guards as TryRelease without the leave -- check by a dry call
            // is unsafe, so just skip unsafely-grouped bots below via the same conditions.
            if (commander->IsInCombat() || bot->IsInCombat() || bot->InBattleground())
                continue;
            if (Map const* map = bot->GetMap(); map && (map->IsDungeon() || map->IsRaid() || map->IsBattlegroundOrArena()))
                continue;
        }
        pool.push_back(bot);
    }

    bool commanderInGuild = commander->GetGuildId() != 0;
    float commanderIlvl = commander->GetAverageItemLevel();
    auto isGuildmate = [&](Player* bot) { return commanderInGuild && bot->GetGuildId() == commander->GetGuildId(); };
    auto betterCandidate = [&](Player* a, Player* b)
    {
        bool aFree = !a->GetGroup(), bFree = !b->GetGroup();
        if (aFree != bFree)
            return aFree; // ungrouped bots first; steal only when no free bot fits
        bool aGuild = isGuildmate(a), bGuild = isGuildmate(b);
        if (aGuild != bGuild)
            return aGuild; // guildmates always win, regardless of level/ilvl fit
        int32 aLevelDiff = std::abs(int32(a->GetLevel()) - int32(commander->GetLevel()));
        int32 bLevelDiff = std::abs(int32(b->GetLevel()) - int32(commander->GetLevel()));
        if (aLevelDiff != bLevelDiff)
            return aLevelDiff < bLevelDiff;
        return std::abs(a->GetAverageItemLevel() - commanderIlvl) < std::abs(b->GetAverageItemLevel() - commanderIlvl);
    };

    auto pickForRole = [&](BotRole role, uint32 count) -> std::vector<Player*>
    {
        std::vector<Player*> matches;
        for (Player* bot : pool)
            if (BotAI::GetRole(bot->GetGUID()) == role)
                matches.push_back(bot);
        std::sort(matches.begin(), matches.end(), betterCandidate);
        if (matches.size() > count)
            matches.resize(count);
        return matches;
    };

    // Fallback for Tank/Healer specifically: a random population's specs mostly land on Dps
    // (real players mostly play Dps -- see AGENTS.md's Quick Fill entry), so "zero bots
    // currently *are* a tank/healer" is the common case, not a reason to leave the slot empty
    // when a class capable of the role is sitting right there. Matches on class capability
    // (BotAI::FindSpecForRole) rather than current spec.
    auto pickCapableForRole = [&](BotRole role, uint32 count) -> std::vector<Player*>
    {
        std::vector<Player*> matches;
        for (Player* bot : pool)
            if (BotAI::FindSpecForRole(bot->getClass(), role) != 0)
                matches.push_back(bot);
        std::sort(matches.begin(), matches.end(), betterCandidate);
        if (matches.size() > count)
            matches.resize(count);
        return matches;
    };

    std::vector<Player*> selected;
    auto takeUpTo = [&](BotRole role, uint32& need)
    {
        if (!need || !slotsLeft)
            return;
        uint32 want = std::min(need, slotsLeft);
        std::vector<Player*> picked = pickForRole(role, want);
        for (Player* bot : picked)
        {
            selected.push_back(bot);
            // Remove from pool so a later role bucket can't also claim this bot.
            pool.erase(std::remove(pool.begin(), pool.end(), bot), pool.end());
        }
        need -= uint32(picked.size());
        slotsLeft -= uint32(picked.size());

        if (!need || !slotsLeft || role == BotRole::Dps)
            return; // every class is Dps-capable -- a Dps shortfall means the pool is just empty

        want = std::min(need, slotsLeft);
        std::vector<Player*> converted = pickCapableForRole(role, want);
        for (Player* bot : converted)
        {
            uint32 currentSpec = bot->GetPlayerSetting("core.ascension_active_spec", 0).value;
            uint32 targetSpec = BotAI::FindSpecForRole(bot->getClass(), role, currentSpec);
            if (targetSpec != currentSpec)
                LearnSpecialization(bot->GetGUID().GetCounter(), targetSpec, nullptr);
            BotAI::SetRole(bot->GetGUID(), role);
            LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr::QuickFillGroup: switched '{}' to {} (spec {}) to fill an empty slot.",
                bot->GetName(), RoleToString(role), targetSpec);
            selected.push_back(bot);
            pool.erase(std::remove(pool.begin(), pool.end(), bot), pool.end());
        }
        need -= uint32(converted.size());
        slotsLeft -= uint32(converted.size());
    };

    takeUpTo(BotRole::Tank, needTanks);
    takeUpTo(BotRole::Healer, needHealers);
    takeUpTo(BotRole::Dps, needDps);

    for (Player* bot : selected)
    {
        if (bot->GetGroup() && !TryReleaseBotFromGroup(commander, bot))
            continue;
        WorldPacket packet;
        packet << bot->GetName();
        packet << uint32(0);
        commander->GetSession()->HandleGroupInviteOpcode(packet);
        LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr::QuickFillGroup: '{}' invited bot '{}' (role {}).",
            commander->GetName(), bot->GetName(), RoleToString(BotAI::GetRole(bot->GetGUID())));
    }

    if (handler)
    {
        if (selected.empty())
            handler->PSendSysMessage("BotMgr: quick-fill found no eligible bots for the roles still needed.");
        else
            handler->PSendSysMessage("BotMgr: quick-fill invited {} bot(s).", uint32(selected.size()));
    }
}

void BotMgr::InviteBotForRole(Player* commander, BotRole role, ChatHandler* handler)
{
    if (!commander)
        return;

    if (Group* group = commander->GetGroup(); group && group->IsFull())
    {
        if (handler)
            handler->SendSysMessage("BotMgr: your group is full.");
        return;
    }

    std::vector<Player*> pool;
    for (Player* bot : GetOnlineBots())
    {
        if (bot == commander || !bot->IsInWorld() || !bot->IsAlive())
            continue;
        if (bot->GetTeamId() != commander->GetTeamId())
            continue;
        if (bot->GetGroup())
        {
            if (Group* selfGroup = commander->GetGroup(); selfGroup && bot->GetGroup() == selfGroup)
                continue;
            if (commander->IsInCombat() || bot->IsInCombat() || bot->InBattleground())
                continue;
            if (Map const* map = bot->GetMap(); map && (map->IsDungeon() || map->IsRaid() || map->IsBattlegroundOrArena()))
                continue;
        }
        pool.push_back(bot);
    }

    bool const commanderInGuild = commander->GetGuildId() != 0;
    float const commanderIlvl = commander->GetAverageItemLevel();
    auto isGuildmate = [&](Player* bot) { return commanderInGuild && bot->GetGuildId() == commander->GetGuildId(); };
    auto better = [&](Player* a, Player* b)
    {
        bool aFree = !a->GetGroup(), bFree = !b->GetGroup();
        if (aFree != bFree)
            return aFree;
        bool aGuild = isGuildmate(a), bGuild = isGuildmate(b);
        if (aGuild != bGuild)
            return aGuild;
        int32 aLevelDiff = std::abs(int32(a->GetLevel()) - int32(commander->GetLevel()));
        int32 bLevelDiff = std::abs(int32(b->GetLevel()) - int32(commander->GetLevel()));
        if (aLevelDiff != bLevelDiff)
            return aLevelDiff < bLevelDiff;
        return std::abs(a->GetAverageItemLevel() - commanderIlvl) < std::abs(b->GetAverageItemLevel() - commanderIlvl);
    };

    // First choice: a bot that already plays the role. Second: a bot whose class can (Dps: every class can).
    Player* chosen = nullptr;
    for (Player* bot : pool)
        if (BotAI::GetRole(bot->GetGUID()) == role && (!chosen || better(bot, chosen)))
            chosen = bot;
    bool converted = false;
    if (!chosen && role != BotRole::Dps)
    {
        for (Player* bot : pool)
            if (BotAI::FindSpecForRole(bot->getClass(), role) != 0 && (!chosen || better(bot, chosen)))
                chosen = bot;
        converted = chosen != nullptr;
    }

    if (!chosen)
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: no free {} bot of your faction is online.", RoleToString(role));
        return;
    }

    if (converted)
    {
        uint32 currentSpec = chosen->GetPlayerSetting("core.ascension_active_spec", 0).value;
        uint32 targetSpec = BotAI::FindSpecForRole(chosen->getClass(), role, currentSpec);
        if (targetSpec != currentSpec)
            LearnSpecialization(chosen->GetGUID().GetCounter(), targetSpec, nullptr);
        BotAI::SetRole(chosen->GetGUID(), role);
    }

    if (chosen->GetGroup() && !TryReleaseBotFromGroup(commander, chosen))
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: '{}' cannot leave its group right now.", chosen->GetName());
        return;
    }
    WorldPacket packet;
    packet << chosen->GetName();
    packet << uint32(0);
    commander->GetSession()->HandleGroupInviteOpcode(packet);
    if (handler)
        handler->PSendSysMessage("BotMgr: invited {} ({}).", chosen->GetName(), RoleToString(role));
}

std::vector<std::string> BotMgr::GetBotList(Player* commander, BotListQuery const& query) const
{
    std::vector<std::string> lines;
    if (!commander)
        return lines;

    auto lower = [](std::string s)
    {
        for (char& ch : s)
            ch = char(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    };
    std::string const nameFilter = lower(query.name);
    uint32 const guildId = commander->GetGuildId();

    std::vector<Player*> matches;
    for (Player* bot : GetOnlineBots())
    {
        if (bot == commander || bot->GetGroup() || !bot->IsInWorld() || bot->GetTeamId() != commander->GetTeamId())
            continue;
        if (query.classId && bot->getClass() != query.classId)
            continue;
        if (bot->GetLevel() < query.minLevel || bot->GetLevel() > query.maxLevel)
            continue;
        if (query.guildOnly && (!guildId || bot->GetGuildId() != guildId))
            continue;
        if (query.role >= 0 && BotAI::GetRole(bot->GetGUID()) != BotRole(query.role))
            continue;
        if (!nameFilter.empty() && lower(bot->GetName()).find(nameFilter) == std::string::npos)
            continue;
        matches.push_back(bot);
    }

    // Guildmates first, then the highest level, then by name.
    std::sort(matches.begin(), matches.end(), [&](Player* a, Player* b)
    {
        bool aGuild = guildId && a->GetGuildId() == guildId, bGuild = guildId && b->GetGuildId() == guildId;
        if (aGuild != bGuild)
            return aGuild;
        if (a->GetLevel() != b->GetLevel())
            return a->GetLevel() > b->GetLevel();
        return a->GetName() < b->GetName();
    });

    constexpr size_t PAGE_SIZE = 24;
    size_t const pages = std::max<size_t>(1, (matches.size() + PAGE_SIZE - 1) / PAGE_SIZE);
    size_t const page = std::min<size_t>(query.page, pages - 1);
    lines.push_back("BOTPAGE:" + std::to_string(page) + ":" + std::to_string(matches.size()) + ":" + std::to_string(pages));

    constexpr size_t BUDGET = 200;
    std::string body;
    auto flush = [&]()
    {
        if (!body.empty())
            lines.push_back("BOTS:" + body);
        body.clear();
    };
    for (size_t i = page * PAGE_SIZE; i < matches.size() && i < (page + 1) * PAGE_SIZE; ++i)
    {
        Player* bot = matches[i];
        std::string name;
        for (char ch : bot->GetName())
            name.push_back(ch == ':' || ch == '|' || ch == ',' ? ' ' : ch);
        std::string className;
        if (ChrClassesEntry const* classEntry = sChrClassesStore.LookupEntry(bot->getClass()))
            for (char ch : std::string(classEntry->name[0] ? classEntry->name[0] : ""))
                className.push_back(ch == ':' || ch == '|' || ch == ',' ? ' ' : ch);
        std::string const piece = std::to_string(bot->GetGUID().GetCounter()) + "," + name + "," + std::to_string(uint32(bot->getClass())) + "," +
            std::to_string(uint32(bot->GetLevel())) + "," + RoleToString(BotAI::GetRole(bot->GetGUID())) + "," + ((guildId && bot->GetGuildId() == guildId) ? "1" : "0") + "," + className;
        if (!body.empty() && body.size() + 1 + piece.size() > BUDGET)
            flush();
        if (!body.empty())
            body += "|";
        body += piece;
    }
    flush();
    return lines;
}

bool BotMgr::TryReleaseBotFromGroup(Player* inviter, Player* bot)
{
    if (!inviter || !bot || bot == inviter)
        return false;
    if (FindBotPlayer(bot->GetGUID().GetCounter()) != bot)
        return false;
    Group* group = bot->GetGroup();
    if (!group)
        return true;
    if (inviter->GetGroup() && group == inviter->GetGroup())
        return false;
    if (inviter->IsInCombat() || bot->IsInCombat())
        return false;
    if (bot->InBattleground())
        return false;
    if (Map const* map = bot->GetMap())
        if (map->IsDungeon() || map->IsRaid() || map->IsBattlegroundOrArena())
            return false;
    if (group->isBGGroup())
        return false;

    LOG_INFO("module.coa-playerbots", "BotMgr: bot '{}' leaving group so '{}' can invite it.",
        bot->GetName(), inviter->GetName());
    bot->RemoveFromGroup(GROUP_REMOVEMETHOD_LEAVE);
    return !bot->GetGroup();
}

void BotMgr::InviteBotToGroup(Player* commander, ObjectGuid::LowType botGuid, ChatHandler* handler)
{
    Player* bot = commander ? FindBotPlayer(botGuid) : nullptr;
    if (commander && !bot && botGuid && botGuid != commander->GetGUID().GetCounter())
    {
        // Not logged in as a companion (AltBots can think an alt is still out after it logged off).
        // Log it in first, then invite, but only for the commander's own characters or for a GM.
        ObjectGuid charGuid = ObjectGuid::Create<HighGuid::Player>(botGuid);
        uint32 accountId = sCharacterCache->GetCharacterAccountIdByGuid(charGuid);
        bool allowed = accountId && (accountId == commander->GetSession()->GetAccountId() ||
            commander->GetSession()->GetSecurity() >= SEC_GAMEMASTER);
        if (allowed && !ObjectAccessor::FindPlayer(charGuid))
        {
            if (handler)
                handler->SendSysMessage("BotMgr: that character was offline; logging it in, then inviting.");
            ObjectGuid commanderGuid = commander->GetGUID();
            SpawnBot(botGuid, nullptr, [commanderGuid, botGuid](Player* /*ready*/)
            {
                if (Player* leader = ObjectAccessor::FindPlayer(commanderGuid))
                {
                    ChatHandler leaderHandler(leader->GetSession());
                    sBotMgr->InviteBotToGroup(leader, botGuid, &leaderHandler);
                }
            });
            return;
        }
    }
    if (!bot || bot == commander || bot->GetTeamId() != commander->GetTeamId())
    {
        if (handler)
            handler->SendSysMessage("BotMgr: that bot is not available any more.");
        return;
    }
    if (bot->GetGroup() && commander->GetGroup() && bot->GetGroup() == commander->GetGroup())
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: '{}' is already in your group.", bot->GetName());
        return;
    }
    if (bot->GetGroup() && !TryReleaseBotFromGroup(commander, bot))
    {
        if (handler)
            handler->PSendSysMessage("BotMgr: '{}' is in combat, a battleground, or an instance, so it cannot leave its group yet.", bot->GetName());
        return;
    }
    if (Group* group = commander->GetGroup(); group && group->IsFull())
    {
        if (handler)
            handler->SendSysMessage("BotMgr: your group is full.");
        return;
    }
    WorldPacket packet;
    packet << bot->GetName();
    packet << uint32(0);
    commander->GetSession()->HandleGroupInviteOpcode(packet);
}

void BotMgr::InviteBotToGuild(Player* commander, ObjectGuid::LowType botGuid, ChatHandler* handler)
{
    Player* bot = commander ? FindBotPlayer(botGuid) : nullptr;
    Guild* guild = commander ? commander->GetGuild() : nullptr;
    if (!bot || !guild || bot->GetGuildId() || bot->GetGuildIdInvited() || bot->GetTeamId() != commander->GetTeamId())
    {
        if (handler)
            handler->SendSysMessage("BotMgr: that bot cannot be invited to your guild.");
        return;
    }
    // The guild's own check of the commander's invite right runs inside; a bot joins as soon as it is invited.
    guild->HandleInviteMember(commander->GetSession(), bot->GetName());
    if (bot->GetGuildIdInvited())
        if (WorldSession* session = FindBotSession(botGuid))
            DoAcceptGuildInvite(session);
}

void BotMgr::SetAutoDungeonMode(ObjectGuid leaderGuid, bool enabled)
{
    if (enabled)
        _autoDungeonLeaders.insert(leaderGuid);
    else
    {
        _autoDungeonLeaders.erase(leaderGuid);
        _clearedBosses.erase(leaderGuid);
    }
}

bool BotMgr::IsAutoDungeonModeEnabled(ObjectGuid leaderGuid) const
{
    return _autoDungeonLeaders.count(leaderGuid) != 0;
}

void BotMgr::MarkBossCleared(ObjectGuid leaderGuid, uint32 bossEntry)
{
    _clearedBosses[leaderGuid].insert(bossEntry);
}

void BotMgr::PostKillEvent(KillEvent const& event)
{
    std::lock_guard<std::mutex> guard(_killEventLock);
    // A bot-less server never drains this, so keep it bounded.
    if (_killEvents.size() < 4096)
        _killEvents.push_back(event);
}

void BotMgr::ProcessKillEvents()
{
    std::vector<KillEvent> events;
    {
        std::lock_guard<std::mutex> guard(_killEventLock);
        if (_killEvents.empty())
            return;
        events.swap(_killEvents);
    }

    for (KillEvent const& event : events)
    {
        if (event.bossEntry)
            MarkBossCleared(event.bossLeader, event.bossEntry);

        if (_botSessions.empty())
            continue;

        if (event.victim.IsPlayer())
        {
            WorldEncounterDirector::OnPvPKill(event.killer, event.victim);
            continue;
        }

        if (Player* killer = ObjectAccessor::FindPlayer(event.killer))
            BotAI::EnqueuePendingLoot(killer, event.victim);
    }
}

bool BotMgr::IsBossCleared(ObjectGuid leaderGuid, uint32 bossEntry) const
{
    auto itr = _clearedBosses.find(leaderGuid);
    if (itr != _clearedBosses.end())
        return itr->second.count(bossEntry) != 0;
    return false;
}

void BotMgr::ClearBosses(ObjectGuid leaderGuid)
{
    _clearedBosses.erase(leaderGuid);
}

void BotMgr::SetGroupFormation(ObjectGuid leaderGuid, BotGroupFormation formation)
{
    _groupFormations[leaderGuid] = formation;
}

BotGroupFormation BotMgr::GetGroupFormation(ObjectGuid leaderGuid) const
{
    auto itr = _groupFormations.find(leaderGuid);
    if (itr != _groupFormations.end())
        return itr->second;
    return BotGroupFormation::RoleBased;
}

void BotMgr::TeleportBotsToPlayer(Player* commander, ChatHandler* handler)
{
    if (!commander)
        return;

    if (commander->IsInCombat())
    {
        if (handler)
            handler->SendSysMessage("BotMgr: cannot teleport bots to you while you are in combat.");
        return;
    }

    Group* group = commander->GetGroup();
    if (!group)
    {
        if (handler)
            handler->SendSysMessage("BotMgr: you are not in a group.");
        return;
    }

    constexpr float ALREADY_HERE_DIST = 5.0f;
    uint32 teleported = 0;
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (!member || member == commander)
            continue;
        if (!FindBotPlayer(member->GetGUID().GetCounter()))
            continue;

        if (member->IsInCombat())
            continue;

        if (member->GetMap() == commander->GetMap() && member->GetDistance(commander) < ALREADY_HERE_DIST)
            continue;

        member->TeleportTo(commander->GetWorldLocation());
        _pendingTeleportAck.push_back(member->GetSession());
        ++teleported;
    }

    LOG_INFO("module.coa-playerbots", "BotMgr::TeleportBotsToPlayer: '{}' brought {} bot(s) to their location.",
        commander->GetName(), teleported);

    if (handler)
    {
        if (teleported)
            handler->PSendSysMessage("BotMgr: teleporting {} bot(s) to you.", teleported);
        else
            handler->SendSysMessage("BotMgr: no bots needed to be teleported.");
    }
}

namespace
{
struct GearTypeSkill
{
    uint32 subclass;
    uint32 skill;
};

constexpr GearTypeSkill ARMOR_SKILLS[4] = {
    { ITEM_SUBCLASS_ARMOR_CLOTH,   SKILL_CLOTH },
    { ITEM_SUBCLASS_ARMOR_LEATHER, SKILL_LEATHER },
    { ITEM_SUBCLASS_ARMOR_MAIL,    SKILL_MAIL },
    { ITEM_SUBCLASS_ARMOR_PLATE,   SKILL_PLATE_MAIL },
};

constexpr GearTypeSkill WEAPON_SKILLS[6] = {
    { ITEM_SUBCLASS_WEAPON_SWORD, SKILL_SWORDS },
    { ITEM_SUBCLASS_WEAPON_MACE,  SKILL_MACES },
    { ITEM_SUBCLASS_WEAPON_AXE,   SKILL_AXES },
    { ITEM_SUBCLASS_WEAPON_FIST,  SKILL_FIST_WEAPONS },
    { ITEM_SUBCLASS_WEAPON_DAGGER,SKILL_DAGGERS },
    { ITEM_SUBCLASS_WEAPON_STAFF, SKILL_STAVES },
};
}

uint32 BotMgr::GetGearPreference(Player* bot, bool weapon) const
{
    if (!bot)
        return 0;
    char const* key = weapon ? "coa.gear_pref_weapon" : "coa.gear_pref_armor";
    return bot->GetPlayerSetting(key, 0).value;
}

void BotMgr::SetGearPreference(Player* bot, bool weapon, uint32 subclass, bool automatic)
{
    if (!bot)
        return;
    char const* key = weapon ? "coa.gear_pref_weapon" : "coa.gear_pref_armor";
    bot->UpdatePlayerSetting(key, 0, automatic ? 0 : (weapon ? subclass + 1 : subclass));
}

std::vector<uint32> BotMgr::GetLegalArmorSubclasses(Player* bot) const
{
    std::vector<uint32> legal;
    if (!bot)
        return legal;
    for (GearTypeSkill const& type : ARMOR_SKILLS)
        if (bot->GetSkillValue(type.skill) > 0)
            legal.push_back(type.subclass);
    return legal;
}

std::vector<uint32> BotMgr::GetLegalWeaponSubclasses(Player* bot) const
{
    std::vector<uint32> legal;
    if (!bot)
        return legal;
    for (GearTypeSkill const& type : WEAPON_SKILLS)
        if (bot->GetSkillValue(type.skill) > 0)
            legal.push_back(type.subclass);
    return legal;
}

bool BotMgr::MatchesGearPreference(Player* bot, uint32 itemEntry) const
{
    if (!bot)
        return true;

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
    if (!proto)
        return true;

    bool isBasicArmor = proto->Class == ITEM_CLASS_ARMOR &&
        (proto->SubClass == ITEM_SUBCLASS_ARMOR_CLOTH || proto->SubClass == ITEM_SUBCLASS_ARMOR_LEATHER ||
         proto->SubClass == ITEM_SUBCLASS_ARMOR_MAIL || proto->SubClass == ITEM_SUBCLASS_ARMOR_PLATE);
    bool isBasicWeapon = proto->Class == ITEM_CLASS_WEAPON &&
        (proto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD || proto->SubClass == ITEM_SUBCLASS_WEAPON_MACE ||
         proto->SubClass == ITEM_SUBCLASS_WEAPON_AXE || proto->SubClass == ITEM_SUBCLASS_WEAPON_FIST ||
         proto->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER || proto->SubClass == ITEM_SUBCLASS_WEAPON_STAFF);
    if (!isBasicArmor && !isBasicWeapon)
        return true;

    uint16 dest = 0;
    InventoryResult const equip = bot->CanEquipNewItem(NULL_SLOT, dest, itemEntry, true);
    bool const levelOnly = equip == EQUIP_ERR_CANT_EQUIP_LEVEL_I && BotGear::ProficientIgnoringLevel(bot, proto);
    if (equip != EQUIP_ERR_OK && !levelOnly)
        return false;

    if (isBasicArmor)
    {
        uint32 pref = GetGearPreference(bot, false);
        if (pref == 0)
            return true;
        return proto->SubClass == pref;
    }

    uint32 pref = GetGearPreference(bot, true);
    if (pref == 0)
        return true;
    return proto->SubClass + 1 == pref;
}

std::vector<std::string> BotMgr::GetEquippedGearInfo(Player* bot) const
{
    std::vector<std::string> lines;
    if (!bot)
        return lines;

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD)
            continue;

        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;

        ItemTemplate const* proto = item->GetTemplate();
        std::string name = proto ? proto->Name1 : ("item " + std::to_string(item->GetEntry()));
        lines.push_back("GEAR:" + std::to_string(bot->GetGUID().GetCounter()) + ":" + std::to_string(uint32(slot)) +
            ":" + std::to_string(item->GetEntry()) + ":" + CoaBotWireText(name, 100, ":|") + ":" +
            std::to_string(proto ? proto->ItemLevel : 0));
    }
    return lines;
}

void BotMgr::RestoreGroupBotsOnLogin(Player* player)
{
    if (!BotsEnabled() || !player || FindBotPlayer(player->GetGUID().GetCounter()))
        return; // only for a real player's own login -- a bot's login already got here via whichever real player's own restore pass spawned it

    Group* group = player->GetGroup();
    if (!group)
        return;

    // GetMemberSlots() (not the online-only GroupReference list) so offline bots -- the
    // whole point of this -- are actually seen.
    for (Group::MemberSlot const& slot : group->GetMemberSlots())
    {
        if (slot.guid == player->GetGUID())
            continue;
        if (ObjectAccessor::FindPlayer(slot.guid))
            continue; // already online (bot or real player) -- Player::_LoadGroup already reattached it if it's a bot

        uint32 accountId = sCharacterCache->GetCharacterAccountIdByGuid(slot.guid);
        if (!IsBotAccountId(accountId))
            continue; // never auto-log-in a real player's other character just because it's grouped with us

        LOG_INFO("module.coa-playerbots", "BotMgr: auto-respawning bot '{}' to restore '{}''s pre-restart group.",
            slot.name, player->GetName());
        SpawnBot(slot.guid.GetCounter(), nullptr);
    }
}

void BotMgr::GearUpBot(Player* bot, ChatHandler* handler)
{
    if (!bot)
        return;

    BotSpawn::RepairGear(bot);
    if (handler)
        handler->PSendSysMessage("Bot gear: repaired '{}' using level-appropriate equipment.", bot->GetName());
}

void BotMgr::DoRollVote(WorldSession* session, Roll* roll, uint8 rollType)
{
    // HandleLootRoll reads itemGUID/itemSlot/rollType then calls Group::CountRollVote,
    // which does the real vote bookkeeping and broadcasts the update via SendLootRoll
    // (already null-socket-tolerant). Same "build the real packet, call the real
    // handler" pattern as DoAcceptInvite/login.
    WorldPacket packet;
    packet << roll->itemGUID;
    packet << uint32(roll->itemSlot);
    packet << rollType;
    session->HandleLootRoll(packet);
}

uint32 BotMgr::QueueAllBotsForAutoLogin(uint32 maxCountOverride)
{
    std::string accountPrefix = sConfigMgr->GetOption<std::string>("CoaBots.RandomSpawn.AccountPrefix", "CoaBotHost");
    uint32 maxBots = maxCountOverride > 0
        ? maxCountOverride
        : sConfigMgr->GetOption<uint32>("CoaBots.AutoLogin.MaxCount", 800);

    QueryResult accounts = LoginDatabase.Query("SELECT id FROM account WHERE username LIKE '{}%'", accountPrefix);
    if (!accounts)
    {
        LOG_INFO("module.coa-playerbots", "BotMgr: auto-login found no bot-hosting accounts yet.");
        return 0;
    }

    std::ostringstream accountIds;
    do
    {
        accountIds << (*accounts)[0].Get<uint32>() << ",";
    } while (accounts->NextRow());
    std::string idList = accountIds.str();
    idList.pop_back();

    // Prioritize bots in guilds or with active gather orders, then other bots
    QueryResult chars = CharacterDatabase.Query(
        "SELECT c.guid FROM characters c "
        "LEFT JOIN guild_member gm ON c.guid = gm.guid "
        "LEFT JOIN mod_coa_bot_guild_gather_orders go ON c.guid = go.bot_guid "
        "WHERE c.account IN ({}){} "
        "ORDER BY (gm.guildid IS NOT NULL OR go.bot_guid IS NOT NULL) DESC, c.guid ASC",
        idList, CustomRacesOff() ? " AND c.race IN (1, 2, 3, 4, 5, 6, 7, 8, 10, 11)" : ""
    );
    if (!chars)
        return 0;

    std::unordered_set<ObjectGuid::LowType> alreadyQueued(
        _pendingAutoLoginQueue.begin(), _pendingAutoLoginQueue.end());

    uint32 queued = 0;
    do
    {
        if (maxBots > 0 && queued >= maxBots)
            break;

        ObjectGuid::LowType guid = (*chars)[0].Get<uint32>();
        if (!FindBotSession(guid) && !alreadyQueued.count(guid))
        {
            _pendingAutoLoginQueue.push_back(guid);
            alreadyQueued.insert(guid);
            ++queued;
        }
    } while (chars->NextRow());

    // ProcessPendingAutoLogin pops from the back, so reverse the queue so that
    // top-priority characters (guild companions, active gatherers) pop first.
    std::reverse(_pendingAutoLoginQueue.begin(), _pendingAutoLoginQueue.end());

    LOG_INFO("module.coa-playerbots", "BotMgr: queued {} bot(s) for gradual auto-login (cap: {}).", queued, maxBots);
    return queued;
}

// A player who opens a trade window with a bot gets a bot that takes part: it opens its side, offers nothing and
// accepts once the player has accepted, so items and gold can be handed to a bot. What it receives goes into its
// bags, and the gear-upgrade pass equips anything that is better than what it wears. A window nobody confirms is
// closed after two minutes so the bot does not stay "busy" for everyone else.
void BotMgr::UpdateTrades(uint32 diff)
{
    if (!_tradeEnabled && _tradeOffers.empty())
    {
        _tradeAgeMs.clear();
        return;
    }

    for (WorldSession* session : _botSessions)
    {
        Player* bot = session->GetPlayer();
        if (!bot)
            continue;

        TradeData* mine = bot->GetTradeData();
        if (!mine)
        {
            if (!_tradeAgeMs.empty())
                _tradeAgeMs.erase(bot->GetGUID());
            continue;
        }
        if (!_tradeEnabled && !_tradeOffers.count(bot->GetGUID()))
            continue;

        Player* other = mine->GetTrader();
        TradeData* theirs = other ? other->GetTradeData() : nullptr;
        if (!theirs)
            continue;

        auto [age, isNew] = _tradeAgeMs.try_emplace(bot->GetGUID(), 0u);
        if (isNew)
        {
            WorldPacket begin(CMSG_BEGIN_TRADE);
            session->HandleBeginTradeOpcode(begin);
            continue;
        }

        age->second += diff;
        if (theirs->IsAccepted() && !mine->IsAccepted())
        {
            WorldPacket accept(CMSG_ACCEPT_TRADE);
            session->HandleAcceptTradeOpcode(accept);
        }
        else if (age->second > 120000)
            bot->TradeCancel(true);
    }

    UpdateTradeOffers(diff);
}

namespace
{
    constexpr uint32 TRADE_OFFER_TIMEOUT_MS = 60000;
    constexpr float TRADE_OFFER_REACH_YARDS = 100.0f;

    void TellOrder(Player* commander, Player* bot, char const* kind, std::string const& token, std::string const& text)
    {
        if (!commander || !commander->GetSession())
            return;
        if (!text.empty())
            ChatHandler(commander->GetSession()).PSendSysMessage("|cff33ccffAltBots:|r {} {}", bot->GetName(), text);
        CoaBotSendAddonReply(commander, "ORDER:" + std::to_string(bot->GetGUID().GetCounter()) + ":" + kind + ":" + token);
    }

    template<class Fn>
    void ForEachBagItem(Player* bot, Fn&& fn)
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                fn(item, uint8(INVENTORY_SLOT_BAG_0), slot);
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = bot->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (Item* item = bag->GetItemByPos(uint8(slot)))
                        fn(item, bagSlot, uint8(slot));
    }

    bool IsBoundForOthers(Item const* item)
    {
        return item->IsSoulBound() && !item->IsBoundAccountWide();
    }
}

std::string BotMgr::SendItemToCommander(Player* bot, Player* commander, uint32 itemEntry, uint32 count, bool toGuildBank,
    uint32& moved)
{
    moved = 0;
    if (!bot || !commander)
        return "none";
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
    if (!proto || !bot->GetItemCount(itemEntry, false))
        return "none";
    if (BotAI::IsProfessionTool(proto))
        return "tool";

    uint32 free = 0;
    ForEachBagItem(bot, [&](Item* item, uint8, uint8)
    {
        if (item->GetEntry() == itemEntry && !IsBoundForOthers(item) && !item->IsInTrade())
            free += item->GetCount();
    });
    if (!free)
        return "bound";
    uint32 const wanted = count ? std::min(count, free) : free;

    if (toGuildBank)
    {
        if (!bot->GetGuild())
            return "noguild";
        moved = GuildDepositItem(bot->GetGUID().GetCounter(), itemEntry, wanted, nullptr);
        return moved ? "ok" : "bankfull";
    }

    moved = MailGatherItems(bot, commander->GetGUID().GetCounter(), itemEntry, wanted,
        bot->GetName() + " sent this to you on your order.");
    return moved ? "ok" : "none";
}

std::string BotMgr::OfferTrade(Player* bot, Player* commander, uint32 itemEntry, uint32 count)
{
    if (!bot || !commander)
        return "none";
    if (!bot->IsAlive())
        return "dead";
    if (bot->IsInCombat())
        return "combat";
    if (bot->GetTradeData() || commander->GetTradeData() || _tradeOffers.count(bot->GetGUID()))
        return "busy";
    if (bot->GetMap() != commander->GetMap() || bot->GetDistance(commander) > TRADE_OFFER_REACH_YARDS)
        return "toofar";
    if (!bot->GetItemCount(itemEntry, false))
        return "none";

    bool tradeable = false;
    ForEachBagItem(bot, [&](Item* item, uint8, uint8)
    {
        if (item->GetEntry() == itemEntry && item->CanBeTraded(false, true))
            tradeable = true;
    });
    if (!tradeable)
        return "bound";

    TradeOffer offer;
    offer.playerGuid = commander->GetGUID();
    offer.itemEntry = itemEntry;
    offer.count = count;
    offer.countBefore = bot->GetItemCount(itemEntry, false);
    _tradeOffers[bot->GetGUID()] = offer;
    return "ok";
}

namespace
{
    constexpr uint32 STANDING_BANK_INTERVAL_MS = 30 * IN_MILLISECONDS;
    constexpr uint32 STANDING_MAIL_INTERVAL_MS = 5 * MINUTE * IN_MILLISECONDS;
}

void BotMgr::SetStandingRule(Player* owner, uint32 itemEntry, bool toGuildBank, bool enabled)
{
    if (!owner || !owner->GetSession() || !itemEntry || !sObjectMgr->GetItemTemplate(itemEntry))
        return;
    StandingRules& rules = _standingRules[owner->GetGUID().GetCounter()];
    rules.accountId = owner->GetSession()->GetAccountId();
    if (enabled)
        rules.toGuildBankByItem[itemEntry] = toGuildBank;
    else
        rules.toGuildBankByItem.erase(itemEntry);
    if (rules.toGuildBankByItem.empty())
        _standingRules.erase(owner->GetGUID().GetCounter());
}

void BotMgr::ClearStandingRules(Player* owner)
{
    if (owner)
        _standingRules.erase(owner->GetGUID().GetCounter());
}

void BotMgr::UpdateStandingRules(uint32 diff)
{
    if (_standingRules.empty())
        return;
    _standingTimerMs += diff;
    if (_standingTimerMs < STANDING_BANK_INTERVAL_MS)
        return;
    _standingTimerMs = 0;
    uint32 const now = getMSTime();

    for (WorldSession* session : _botSessions)
    {
        Player* bot = session ? session->GetPlayer() : nullptr;
        if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->GetTradeData())
            continue;

        uint32 orderItem = 0;
        GetGatherOrderItem(bot->GetGUID(), &orderItem);
        for (auto const& [ownerLow, rules] : _standingRules)
        {
            if (rules.accountId != session->GetAccountId() || ownerLow == bot->GetGUID().GetCounter())
                continue;

            auto mailAt = _standingMailAtMs.find(bot->GetGUID());
            bool const mailDue = mailAt == _standingMailAtMs.end() || getMSTimeDiff(mailAt->second, now) >= STANDING_MAIL_INTERVAL_MS;
            bool mailed = false;
            for (auto const& [itemEntry, toGuildBank] : rules.toGuildBankByItem)
            {
                if (itemEntry == orderItem || !bot->GetItemCount(itemEntry, false))
                    continue;
                if (toGuildBank && !bot->GetGuild())
                    continue;
                if (!toGuildBank && !mailDue)
                    continue;
                uint32 moved = 0;
                if (toGuildBank)
                    SendItemToCommander(bot, bot, itemEntry, 0, true, moved);
                else if (!BotAI::IsProfessionTool(sObjectMgr->GetItemTemplate(itemEntry)))
                {
                    moved = MailGatherItems(bot, ownerLow, itemEntry, bot->GetItemCount(itemEntry, false),
                        bot->GetName() + " sent this on your standing mail rule.");
                    mailed = mailed || moved > 0;
                }
                if (moved)
                    LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: standing rule -- bot '{}' {} {}x item {} for owner {}.",
                        bot->GetName(), toGuildBank ? "banked" : "mailed", moved, itemEntry, ownerLow);
            }
            if (mailed || mailDue)
                _standingMailAtMs[bot->GetGUID()] = now;
        }
    }
}

void BotMgr::UpdateTradeOffers(uint32 diff)
{
    for (auto itr = _tradeOffers.begin(); itr != _tradeOffers.end();)
    {
        TradeOffer& offer = itr->second;
        Player* bot = FindBotPlayer(itr->first.GetCounter());
        Player* commander = ObjectAccessor::FindPlayer(offer.playerGuid);
        if (!bot || !commander || !bot->IsInWorld() || !commander->IsInWorld())
        {
            itr = _tradeOffers.erase(itr);
            continue;
        }

        offer.ageMs += diff;
        TradeData* mine = bot->GetTradeData();
        bool const withCommander = mine && mine->GetTrader() == commander;

        if (offer.placed)
        {
            if (!mine)
            {
                bool const traded = bot->GetItemCount(offer.itemEntry, false) < offer.countBefore;
                TellOrder(commander, bot, "trade", traded ? "ok" : "cancelled",
                    traded ? "handed the item over." : "the trade was cancelled.");
                itr = _tradeOffers.erase(itr);
                continue;
            }
            if (offer.ageMs > TRADE_OFFER_TIMEOUT_MS)
            {
                bot->TradeCancel(true);
                TellOrder(commander, bot, "trade", "timeout", "closed the trade after a minute without an accept.");
                itr = _tradeOffers.erase(itr);
                continue;
            }
            ++itr;
            continue;
        }

        if (offer.ageMs > TRADE_OFFER_TIMEOUT_MS || bot->IsInCombat() || !bot->IsAlive())
        {
            if (withCommander)
                bot->TradeCancel(true);
            TellOrder(commander, bot, "trade", "failed", "could not start the trade.");
            itr = _tradeOffers.erase(itr);
            continue;
        }

        if (!offer.initiated)
        {
            if (mine || commander->GetTradeData())
            {
                ++itr;
                continue;
            }
            if (bot->GetMap() != commander->GetMap() || bot->GetDistance(commander) > TRADE_OFFER_REACH_YARDS)
            {
                TellOrder(commander, bot, "trade", "toofar", "is too far away to trade.");
                itr = _tradeOffers.erase(itr);
                continue;
            }
            if (!bot->IsWithinDistInMap(commander, TRADE_DISTANCE - 2.0f, false))
            {
                if (!offer.nextMoveMs || offer.ageMs >= offer.nextMoveMs)
                {
                    offer.nextMoveMs = offer.ageMs + 2000;
                    BotMovement::MoveTo(bot, MoveOwner::Travel, commander->GetPositionX(), commander->GetPositionY(),
                        commander->GetPositionZ());
                }
                ++itr;
                continue;
            }
            WorldPacket initiate(CMSG_INITIATE_TRADE, 8);
            initiate << commander->GetGUID();
            bot->GetSession()->HandleInitiateTradeOpcode(initiate);
            if (!bot->GetTradeData())
            {
                TellOrder(commander, bot, "trade", "failed", "could not open a trade with you.");
                itr = _tradeOffers.erase(itr);
                continue;
            }
            offer.initiated = true;
            WorldPacket begin(CMSG_BEGIN_TRADE);
            bot->GetSession()->HandleBeginTradeOpcode(begin);
            ++itr;
            continue;
        }

        if (!withCommander)
        {
            TellOrder(commander, bot, "trade", "cancelled", "the trade was cancelled.");
            itr = _tradeOffers.erase(itr);
            continue;
        }

        uint8 tradeSlot = 0;
        uint32 offered = 0;
        ForEachBagItem(bot, [&](Item* item, uint8 bag, uint8 slot)
        {
            if (tradeSlot >= TRADE_SLOT_TRADED_COUNT || (offer.count && offered >= offer.count))
                return;
            if (item->GetEntry() != offer.itemEntry || !item->CanBeTraded(false, true) || mine->HasItem(item->GetGUID()))
                return;
            WorldPacket set(CMSG_SET_TRADE_ITEM, 3);
            set << tradeSlot << bag << slot;
            bot->GetSession()->HandleSetTradeItemOpcode(set);
            ++tradeSlot;
            offered += item->GetCount();
        });
        offer.placed = true;
        if (!bot->GetTradeData())
        {
            TellOrder(commander, bot, "trade", "failed", "could not put the item in the trade window.");
            itr = _tradeOffers.erase(itr);
            continue;
        }
        TellOrder(commander, bot, "trade", "open",
            Acore::StringFormat("put {}x {} in the trade window. Accept the trade to take it.", offered, GatherItemName(offer.itemEntry)));
        ++itr;
    }
}

void BotMgr::Update(uint32 diff)
{
    CoaBotPerf::Scope perfScope(CoaBotPerf::Metric::Manager);
    if (!BotsEnabled())
    {
        if (!_botSessions.empty() || !_pendingAutoLoginQueue.empty())
            DespawnAllBots();
        return;
    }

    // Readback inspects loaded persistent state before durable gear orders or other maintenance
    // can legitimately change it. Login query callbacks and map updates still run in the core.
    static bool const benchmarkReadback = sConfigMgr->GetOption<bool>("CoaBots.Benchmark.SuspendOnLogin", false);
    if (benchmarkReadback) return;

    static uint32 capCheckTimer = 0;
    capCheckTimer += diff;
    if (capCheckTimer >= 2000)
    {
        capCheckTimer = 0;
        for (WorldSession* session : _botSessions)
            if (ClampBotLevel(session->GetPlayer()))
                break;
    }
    ProcessKillEvents();
    {
        CoaBotPerf::Scope scope(CoaBotPerf::Metric::ManagerGear);
        BotGear::Update(diff);
    }

    // Must run even with zero bots currently online (e.g. right after a fresh restart, before
    // anything has spawned yet) -- otherwise a `.botcmd spawnrandom` issued at that point would
    // queue a batch that never starts draining until some unrelated bot happens to log in.
    {
        CoaBotPerf::Scope scope(CoaBotPerf::Metric::ManagerPopulation);
        BotSpawn::ProcessPendingRandomBotSpawns(diff);
        BotSpawn::ProcessPendingLeveledBotSpawns(diff);
        BotZoneProgression::ProcessPendingRelocations(diff);
    }
    {
        CoaBotPerf::Scope scope(CoaBotPerf::Metric::ManagerWorld);
        WorldBrain::GlobalUpdate(diff);
    }
    {
        CoaBotPerf::Scope scope(CoaBotPerf::Metric::ManagerEncounters);
        WorldEncounterDirector::Update(diff);
    }
    CoaBotPerf::Scope sessionMaintenance(CoaBotPerf::Metric::ManagerSessions);

    // See QueueAllBotsForAutoLogin -- gradual login for every known bot character, throttled the
    // same way SpawnRandomBots/SpawnLeveledBots throttle bulk creation (login itself is much
    // lighter -- SpawnBot just queues an async DB query and returns -- so this can run a faster
    // pace than character creation without repeating the earlier crash). Also must run with zero
    // bots online yet, same reasoning as the two calls above.
    if (!_pendingAutoLoginQueue.empty())
    {
        uint32 autoLoginBatchSize = sConfigMgr->GetOption<uint32>("CoaBots.AutoLogin.BatchSize", 10);
        uint32 autoLoginIntervalMs = sConfigMgr->GetOption<uint32>("CoaBots.AutoLogin.BatchIntervalMs", 500);

        if (_autoLoginThrottleMs > diff)
            _autoLoginThrottleMs -= diff;
        else
        {
            _autoLoginThrottleMs = autoLoginIntervalMs;
            uint32 batch = std::min<uint32>(uint32(_pendingAutoLoginQueue.size()), autoLoginBatchSize);
            for (uint32 i = 0; i < batch; ++i)
            {
                SpawnBot(_pendingAutoLoginQueue.back(), nullptr);
                _pendingAutoLoginQueue.pop_back();
            }
            if (_pendingAutoLoginQueue.empty())
                LOG_INFO("module.coa-playerbots", "BotMgr: auto-login queue drained.");
        }
    }

    if (_botSessions.empty())
        return;

    // Finish any teleport queued on a *previous* tick first, before this tick's
    // invite-check loop below has a chance to queue a fresh one -- that's what
    // gives it the one-tick separation from TeleportTo() it needs (see
    // FinishPendingTeleport's header comment).
    if (!_pendingTeleportAck.empty())
    {
        std::vector<WorldSession*> due;
        due.swap(_pendingTeleportAck);
        for (WorldSession* session : due)
            FinishPendingTeleport(session);
    }

    // Safety net for teleports nothing explicitly queued an ack for. A spell-driven teleport -- a
    // portal or teleport crystal the ambient layer uses, a hearthstone -- only starts when its cast
    // lands, possibly ticks after whatever triggered it, so the caller cannot queue the ack itself.
    // Without one a null-socket bot stays IsBeingTeleported forever (see FinishPendingTeleport).
    // Queued here it is finished next tick, keeping the one-tick separation that function needs.
    for (WorldSession* session : _botSessions)
    {
        Player* bot = session->GetPlayer();
        if (!bot || !(bot->IsBeingTeleportedNear() || bot->IsBeingTeleportedFar()))
            continue;
        if (std::find(_pendingTeleportAck.begin(), _pendingTeleportAck.end(), session) == _pendingTeleportAck.end())
            _pendingTeleportAck.push_back(session);
    }

    // See QueueBotGroupLeave/the new GroupScript hook -- a real player leaving/being removed
    // from a group with no other real player left in it means the remaining bots have no one
    // left to command, so send each one through the same real "leave party" call a client's own
    // button triggers (Player::RemoveFromGroup), letting them fall back to their normal
    // idle/solo behavior (TryGrindWhenSolo etc.) instead of standing around in a bot-only group.
    if (!_pendingGroupLeaves.empty())
    {
        std::vector<ObjectGuid> due;
        due.swap(_pendingGroupLeaves);
        for (ObjectGuid botGuid : due)
        {
            Player* bot = FindBotPlayer(botGuid.GetCounter());
            if (bot && !CoaTraining::Owns(bot->GetGUID()) && bot->GetGroup())
            {
                LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: bot '{}' leaving its group -- no real player left in it.", bot->GetName());
                bot->RemoveFromGroup(GROUP_REMOVEMETHOD_LEAVE);
            }
        }
    }

    // Auto-accept: checked every tick, not throttled. A bot with a real Player
    // and a pending group or guild invite accepts it immediately, same as a human would.
    for (WorldSession* session : _botSessions)
    {
        Player* bot = session->GetPlayer();
        if (!bot)
            continue;

        if (bot->GetGroupInvite())
        {
            LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: auto-accepting pending group invite for bot '{}'.", bot->GetName());
            DoAcceptInvite(session);
        }

        if (bot->GetGuildIdInvited() && !bot->GetGuildId())
        {
            LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: auto-accepting pending guild invite for bot '{}' (guild id {}).",
                bot->GetName(), bot->GetGuildIdInvited());
            DoAcceptGuildInvite(session);
        }
    }

    // Cross-map/instance follow: checked every tick, throttled implicitly by
    // TryFollowLeaderAcrossMaps' own early-out once map+instance already match. Keeps a bot
    // with the group leader through dungeon/raid/BG transfers a real client would otherwise
    // have to walk through a portal for.
    for (WorldSession* session : _botSessions)
        TryFollowLeaderAcrossMaps(session);

    // Ghost-stranded-off-corpse-map recovery: see TryReturnGhostToCorpseMap's comment.
    for (WorldSession* session : _botSessions)
        TryReturnGhostToCorpseMap(session);

    // Loot rolls: also checked every tick. Needs on armor and weapons that would replace what the
    // bot wears (BotAI::IsLootUpgrade, CoaBots.Loot.NeedOnUpgrades) when the roll offers Need.
    // Otherwise greeds on anything except the 4 basic armor types /
    // 6 basic melee weapon types this bot's class can't wear or doesn't prefer
    // (MatchesGearPreference, see docs/addon-protocol.md's GETGEAR/SETGEARPREF verbs) --
    // everything else (shields/jewelry/ranged weapons/relics/consumables/etc.) still always
    // Greeds, unchanged from before.
    for (WorldSession* session : _botSessions)
    {
        Player* bot = session->GetPlayer();
        if (!bot)
            continue;

        Group* group = bot->GetGroup();
        if (!group)
            continue;

        for (Roll* roll : group->GetRolls())
        {
            auto voteItr = roll->playerVote.find(bot->GetGUID());
            if (voteItr != roll->playerVote.end() && voteItr->second == NOT_EMITED_YET)
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(roll->itemid);
                bool const needOffered = roll->rollVoteMask & ROLL_FLAG_TYPE_NEED;
                bool const upgrade = _lootNeedOnUpgrades && needOffered && BotAI::IsLootUpgrade(bot, proto);
                BotGearPolicy::LootRoll const choice = BotGearPolicy::ChooseLootRoll(
                    _lootNeedOnUpgrades, needOffered, upgrade, MatchesGearPreference(bot, roll->itemid));
                char const* const label = choice == BotGearPolicy::LootRoll::Need ? "Need (upgrade)" :
                    choice == BotGearPolicy::LootRoll::Greed ? "Greed" : "Pass (wrong armor/weapon type)";
                LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()),
                    "BotMgr: bot '{}' rolling {} on item {} (slot {}).",
                    bot->GetName(), label, roll->itemid, roll->itemSlot);
                DoRollVote(session, roll, choice == BotGearPolicy::LootRoll::Need ? ROLL_NEED :
                    choice == BotGearPolicy::LootRoll::Greed ? ROLL_GREED : ROLL_PASS);
            }
        }
    }

    // Combat AI: chase/engage/cast for every active bot. See BotAI.cpp for why this is a
    // generic, class-agnostic "press known offensive spells" engine rather than a
    // hand-tuned per-class rotation.
    //
    // A bot does not need to think thirty times a second: a decision every 100 ms while it fights, follows a
    // group or sits in a battleground, and every 250 ms while it wanders alone, is indistinguishable in play and
    // costs a fraction of the world thread (a thousand bots made this loop most of the tick). The time a bot waited
    // is handed to the AI as its `diff`, so every timer inside it keeps running at the right speed, and the first
    // decision of each bot is offset by its guid so they do not all think on the same tick.
    BotAddonChat::ProcessPendingQueries();
    sessionMaintenance.Stop();
    _aiConfigAgeMs += diff;
    if (_aiConfigAgeMs >= 10000)
    {
        _aiConfigAgeMs = 0;
        _aiCombatIntervalMs = sConfigMgr->GetOption<uint32>("CoaBots.AI.CombatIntervalMs", 100);
        _aiIdleIntervalMs = sConfigMgr->GetOption<uint32>("CoaBots.AI.IdleIntervalMs", 250);
        _tradeEnabled = sConfigMgr->GetOption<bool>("CoaBots.Trade.Enable", true);
        _autoLootInGroup = sConfigMgr->GetOption<bool>("CoaBots.Loot.AutoLootInGroup", false);
        _lootNeedOnUpgrades = sConfigMgr->GetOption<bool>("CoaBots.Loot.NeedOnUpgrades", true);
        uint32 const levelsAhead = sConfigMgr->GetOption<uint32>("CoaBots.Loot.NeedLevelsAhead", 2);
        _lootNeedLevelsAhead = uint8(std::min<uint32>(levelsAhead, 10));
        _inventorySellWhenNearVendor = sConfigMgr->GetOption<bool>("CoaBots.Inventory.SellWhenNearVendor", true);
        _inventorySellOnLeaderVendor = sConfigMgr->GetOption<bool>("CoaBots.Inventory.SellOnLeaderVendor", true);
        _inventoryKeepQuality = sConfigMgr->GetOption<uint32>("CoaBots.Inventory.KeepQuality", 3);
        _questShareTurnIn = sConfigMgr->GetOption<bool>("CoaBots.Quest.ShareTurnIn", true);
        _questShareAccept = sConfigMgr->GetOption<bool>("CoaBots.Quest.ShareAccept", true);
        _questShareIgnoreEligibility = sConfigMgr->GetOption<bool>("CoaBots.Quest.ShareIgnoreEligibility", true);
        _questShareObjectLoot = sConfigMgr->GetOption<bool>("CoaBots.Quest.ShareObjectLoot", true);
        _gatherWhileFollowing = sConfigMgr->GetOption<bool>("CoaBots.Gather.WhileFollowing", true);
        float followRadius = sConfigMgr->GetOption<float>("CoaBots.Gather.FollowRadius", 20.0f);
        if (followRadius < 5.0f)
            followRadius = 5.0f;
        else if (followRadius > 40.0f)
            followRadius = 40.0f;
        _gatherFollowRadius = followRadius;
    }
    {
        CoaBotPerf::Scope scope(CoaBotPerf::Metric::ManagerTrades);
        UpdateTrades(diff);
        UpdateStandingRules(diff);
    }
    {
        CoaBotPerf::Scope scope(CoaBotPerf::Metric::Economy);
        BotEconomy::Update(diff, _botSessions);
    }
    CoaBotPerf::Gauge("sessions", _botSessions.size());
    uint32 const combatIntervalMs = _aiCombatIntervalMs;
    uint32 const idleIntervalMs = _aiIdleIntervalMs;
    if (CoaBotScheduling::Enabled())
        CoaBotScheduling::Update(_botSessions, [](Player* bot, uint32 elapsed) { BotAI::Update(bot, elapsed); });
    else
    {
        struct ReadyUpdate
        {
            ObjectGuid guid;
            uint32 elapsed, interval;
            bool background;
            CoaBotScheduling::WorkOrder workOrder;
        };
        std::vector<ReadyUpdate> ready;
        for (WorldSession* session : _botSessions)
        {
            Player* bot = session->GetPlayer();
            if (!bot)
                continue;

            uint32 const interval = (bot->IsInCombat() || bot->GetGroup() || bot->InBattleground()) ? combatIntervalMs : idleIntervalMs;
            auto [waiting, isNew] = _aiWaitMs.try_emplace(bot->GetGUID(), 0u);
            if (isNew && interval)
                waiting->second = uint32(bot->GetGUID().GetCounter() * 37u) % interval;
            waiting->second += diff;
            if (waiting->second < interval)
                continue;

            uint32 const elapsed = waiting->second;
            waiting->second = 0;
            if (CoaBotScheduling::BudgetsEnabled())
                ready.push_back({bot->GetGUID(), elapsed, interval, !bot->IsInCombat() && !CoaBotScheduling::Protected(bot),
                                 CoaBotScheduling::PendingWorkOrder(bot->GetGUID())});
            else BotAI::Update(bot, elapsed);
        }
        std::stable_sort(ready.begin(), ready.end(), [](ReadyUpdate const& a, ReadyUpdate const& b)
        {
            return std::tie(a.background, a.workOrder) < std::tie(b.background, b.workOrder);
        });
        for (auto const& update : ready)
            if (Player* bot = ObjectAccessor::FindPlayer(update.guid))
            {
                if (!update.background) CoaBotScheduling::CancelWork(bot->GetGUID());
                BotAI::Update(bot, update.elapsed);
                CoaBotScheduling::DeferWorkFor(bot->GetGUID(), update.interval);
            }
    }

    // Active guild gather orders: check if bot collected the requested item, deposit into guild bank
    if (!_guildGatherOrders.empty())
    {
        for (auto itr = _guildGatherOrders.begin(); itr != _guildGatherOrders.end(); )
        {
            Player* bot = FindBotPlayer(itr->first.GetCounter());
            if (!bot || !bot->IsInWorld())
            {
                // Bot might be offline, loading, or mid-teleport -- NEVER erase the persistent order!
                if (!bot)
                    itr->second.lastProgressMs = 0; // the no-progress clock restarts when it is back
                ++itr;
                continue;
            }

            if (!bot->GetGuild() && !itr->second.mailToRequester)
            {
                GuildGatherOrder const order = itr->second;
                DeleteGuildGatherOrder(itr->first);
                itr = _guildGatherOrders.erase(itr);
                NotifyGatherOrderFail(bot->GetGUID(), bot->GetName(), order, "leftguild");
                continue;
            }

            uint32 const nowMs = getMSTime() ? getMSTime() : 1;
            GuildGatherOrder& order = itr->second;
            uint32 const itemEntry = order.itemEntry;

            // A bot grouped with a real player follows that player (BotAI suspends its solo life,
            // order included), so the order would sit at 0 forever. Pause its clocks, say so on
            // the Task Board, and after GUILD_GATHER_GROUPED_HANDOFF_MS hand the rest of the order
            // back to the front of the queue for the next free bot.
            Group* group = bot->GetGroup();
            bool const groupedWithPlayer = group && !FindBotSession(group->GetLeaderGUID().GetCounter());
            if (groupedWithPlayer)
            {
                if (!order.groupedSinceMs)
                    order.groupedSinceMs = nowMs;
                order.lastProgressMs = nowMs;
                order.status = "paused - in a group (remove it from the group to resume)";
                // What it picks up while following still counts on the board.
                order.heldCount = std::min(bot->GetItemCount(itemEntry, false), order.remainingCount);
                if (!order.mailToRequester && getMSTimeDiff(order.groupedSinceMs, nowMs) >= GUILD_GATHER_GROUPED_HANDOFF_MS)
                {
                    uint32 const banked = GuildDepositItem(bot->GetGUID().GetCounter(), itemEntry, order.remainingCount, nullptr);
                    order.gatheredCount += banked;
                    uint32 const rest = order.remainingCount > banked ? order.remainingCount - banked : 0;
                    GuildGatherOrder const handed = order;
                    DeleteGuildGatherOrder(itr->first);
                    itr = _guildGatherOrders.erase(itr);
                    if (!rest)
                    {
                        NotifyGatherOrderDone(bot, handed);
                        continue;
                    }
                    QueuedGatherOrder queued;
                    queued.queueId = _nextGatherQueueId++;
                    queued.guildId = bot->GetGuildId();
                    queued.requesterLowGuid = handed.requesterLowGuid;
                    queued.itemEntry = itemEntry;
                    queued.count = rest;
                    _gatherQueue.insert(_gatherQueue.begin(), queued);
                    SaveGatherQueue();
                    LOG_INFO("module.coa-playerbots", "BotMgr: bot '{}' is grouped; the rest of its gather order ({}x item {}) went back to the queue.",
                        bot->GetName(), rest, itemEntry);
                    if (handed.requesterLowGuid && !FindBotSession(handed.requesterLowGuid))
                        if (Player* requester = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(handed.requesterLowGuid)))
                            if (requester->GetSession())
                                ChatHandler(requester->GetSession()).PSendSysMessage(
                                    "|cff33ff99Guild order:|r {} is in a group, so the remaining {}x {} goes to the next free guild bot.",
                                    bot->GetName(), rest, GatherItemName(itemEntry));
                    continue;
                }
                ++itr;
                continue;
            }
            if (order.groupedSinceMs)
            {
                order.groupedSinceMs = 0;
                order.lastProgressMs = nowMs;
                order.status = "back to work";
            }

            // No-progress watchdog, only counted while the bot is actually online, in the world and
            // free to work. Progress is anything gathered: banked *or* still carried.
            if (!order.lastProgressMs)
                order.lastProgressMs = nowMs;
            else if (getMSTimeDiff(order.lastProgressMs, nowMs) >= GUILD_GATHER_NO_PROGRESS_MS)
            {
                GuildGatherOrder const failed = order;
                DeleteGuildGatherOrder(itr->first);
                itr = _guildGatherOrders.erase(itr);
                NotifyGatherOrderFail(bot->GetGUID(), bot->GetName(), failed, "noprogress");
                continue;
            }

            // What it carries counts as progress right away. Progress used to move only when a
            // guild-bank deposit succeeded -- a full bank tab meant 0/N on the board for the whole
            // order however much the bot had actually gathered, until the 30-minute watchdog failed it.
            uint32 const remaining = order.remainingCount;
            uint32 const held = bot->GetItemCount(itemEntry, false);
            uint32 const heldCapped = std::min(held, remaining);
            if (heldCapped > order.heldCount)
            {
                order.stalledRetries = 0;
                order.lastProgressMs = nowMs;
            }
            order.heldCount = heldCapped;

            // First item counted for this order: tell the requester, so "is it working at all?"
            // has an answer in chat without opening the board.
            if (!order.firstProgressTold && GatherOrderProgress(order) > order.startProgress)
            {
                order.firstProgressTold = true;
                TellGatherRequester(order, Acore::StringFormat("|cff33ff99Guild order #{}:|r {} gathered {} {} ({}/{}).",
                    order.orderNo, bot->GetName(), GatherOrderProgress(order) - order.startProgress, GatherItemName(itemEntry),
                    GatherOrderProgress(order), order.targetCount));
                LOG_INFO("module.coa-playerbots", "BotMgr: order #{} ({}x item {}) by bot '{}': first progress, {}/{}.", order.orderNo,
                    order.targetCount, itemEntry, bot->GetName(), GatherOrderProgress(order), order.targetCount);
            }
            // Opened loot several times without ever getting the ordered item: say what came
            // instead, once (a rare drop, or a node type that only sometimes yields it).
            if (!order.wrongLootTold && order.lootsOpened >= 3 && !order.lootsWithItem && !order.lastLoot.empty())
            {
                order.wrongLootTold = true;
                TellGatherRequester(order, Acore::StringFormat(
                    "|cffffcc00Guild order #{}:|r {} looted {} time(s) but got no {} yet. Last loot: {}.", order.orderNo,
                    bot->GetName(), order.lootsOpened, GatherItemName(itemEntry), order.lastLoot));
            }

            // Bag space. Loot that does not fit stays in the node, so a bot with full bags is
            // seen gathering while nothing is ever counted. Checked on a timer, and right away
            // after TakeAllLoot reports something left behind (NoteGatherLoot clears the timer).
            if (!order.lastRoomCheckMs || getMSTimeDiff(order.lastRoomCheckMs, nowMs) >= GATHER_ROOM_CHECK_MS)
            {
                order.lastRoomCheckMs = nowMs;
                bool const needCheck = order.bagsFull || !CanTakeOneMore(bot, itemEntry) ||
                    FreeGeneralBagSlots(bot) < GATHER_ORDER_MIN_FREE_SLOTS;
                if (needCheck)
                {
                    bool const fits = MakeRoomForGatherOrder(bot);
                    if (!fits && !order.bagsFullTold)
                    {
                        order.bagsFullTold = true;
                        LOG_INFO("module.coa-playerbots", "BotMgr: order #{} bot '{}' has full bags and could not make room for item {}.",
                            order.orderNo, bot->GetName(), itemEntry);
                        TellGatherRequester(order, Acore::StringFormat(
                            "|cffff6060Guild order #{}:|r {}'s bags are full and nothing more could be banked or mailed. Free a guild bank slot so it can keep gathering {}.",
                            order.orderNo, bot->GetName(), GatherItemName(itemEntry)));
                    }
                    order.bagsFull = !fits;
                    if (order.remainingCount == 0)
                    {
                        GuildGatherOrder const done = order;
                        DeleteGuildGatherOrder(itr->first);
                        itr = _guildGatherOrders.erase(itr);
                        NotifyGatherOrderDone(bot, done);
                        continue;
                    }
                    // Counts moved (banked/mailed); the deposit below works from fresh ones next tick.
                    ++itr;
                    continue;
                }
                else
                    order.bagsFull = false;
            }

            if (order.mailToRequester)
            {
                if (heldCapped >= remaining && (!order.lastDepositTryMs || getMSTimeDiff(order.lastDepositTryMs, nowMs) >= GATHER_DEPOSIT_RETRY_MS))
                {
                    order.lastDepositTryMs = nowMs;
                    uint32 const mailed = MailGatherItems(bot, order.requesterLowGuid, itemEntry, remaining, OrderMailBody(bot));
                    if (mailed > 0)
                    {
                        GuildGatherOrder done = order;
                        done.gatheredCount += mailed;
                        DeleteGuildGatherOrder(itr->first);
                        itr = _guildGatherOrders.erase(itr);
                        NotifyGatherOrderDone(bot, done, mailed);
                        continue;
                    }
                }
            }
            else if (held > 0 && (!order.lastDepositTryMs || getMSTimeDiff(order.lastDepositTryMs, nowMs) >= GATHER_DEPOSIT_RETRY_MS ||
                (heldCapped >= remaining && !order.bankBlocked)))
            {
                order.lastDepositTryMs = nowMs;
                uint32 const deposited = GuildDepositItem(bot->GetGUID().GetCounter(), itemEntry, heldCapped, nullptr);
                if (deposited > 0)
                {
                    order.gatheredCount += deposited;
                    order.heldCount = order.heldCount > deposited ? order.heldCount - deposited : 0;
                    order.bankBlocked = false;
                    order.stalledRetries = 0;
                    order.lastProgressMs = nowMs;
                    if (deposited >= remaining)
                    {
                        LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: guildgather order completed for bot '{}' (item {}).",
                            bot->GetName(), itemEntry);
                        GuildGatherOrder const done = order;
                        DeleteGuildGatherOrder(itr->first);
                        itr = _guildGatherOrders.erase(itr);
                        NotifyGatherOrderDone(bot, done);
                        continue;
                    }
                    order.remainingCount -= deposited;
                    SaveGuildGatherOrder(itr->first, order);
                }
                else
                {
                    if (!order.bankBlocked)
                    {
                        LOG_INFO("module.coa-playerbots", "BotMgr: guild bank of '{}' has no room for item {} -- bot '{}' keeps gathering and mails it on completion.",
                            bot->GetGuildName(), itemEntry, bot->GetName());
                        if (order.requesterLowGuid && !FindBotSession(order.requesterLowGuid))
                            if (Player* requester = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(order.requesterLowGuid)))
                                if (requester->GetSession())
                                    ChatHandler(requester->GetSession()).PSendSysMessage(
                                        "|cffffcc00Guild order:|r the guild bank has no free slot for {} -- {} keeps gathering and will mail them to you when the order is done. Free a bank slot to have them banked instead.",
                                        GatherItemName(itemEntry), bot->GetName());
                    }
                    order.bankBlocked = true;

                    // Finished but the bank can't take it: mail it to whoever asked.
                    if (heldCapped >= remaining)
                    {
                        bool const canMail = order.requesterLowGuid && !FindBotSession(order.requesterLowGuid);
                        uint32 const mailed = canMail ? MailGatherItems(bot, order.requesterLowGuid, itemEntry, remaining) : 0;
                        // No player to mail to (a console/GM order, or a bot asked): the items stay
                        // in the bot's bags and the order is done.
                        if (mailed > 0 || !canMail)
                        {
                            GuildGatherOrder done = order;
                            done.gatheredCount += mailed ? mailed : heldCapped;
                            DeleteGuildGatherOrder(itr->first);
                            itr = _guildGatherOrders.erase(itr);
                            LOG_INFO("module.coa-playerbots", "BotMgr: gather order of bot '{}' (item {}) done; {} mailed to the requester (bank full).",
                                bot->GetName(), itemEntry, mailed);
                            NotifyGatherOrderDone(bot, done, mailed);
                            continue;
                        }
                    }
                }
            }
            ++itr;
        }
    }

    ProcessCraftOrders();

    _heartbeatTimer += diff;
    if (_heartbeatTimer < 10000)
        return;
    _heartbeatTimer = 0;

    // Queued gather orders start as soon as a suitable guild bot is free.
    ProcessGatherQueue();

    // Auto-deposit excess gold (> 500g) into guild bank for active bots
    for (WorldSession* session : _botSessions)
    {
        Player* bot = session->GetPlayer();
        if (!bot || !bot->IsInWorld() || !bot->GetGuild())
            continue;

        uint32 currentMoney = bot->GetMoney();
        constexpr uint32 goldLimit = 500 * GOLD;
        if (currentMoney > goldLimit)
        {
            uint32 excess = currentMoney - goldLimit;
            LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotMgr: bot '{}' auto-depositing excess gold ({} copper) to guild bank.",
                bot->GetName(), excess);
            GuildDepositMoney(bot->GetGUID().GetCounter(), excess, nullptr);
        }
    }

    for (WorldSession* session : _botSessions)
    {
        Player* bot = session->GetPlayer();
        LOG_DEBUG("module.coa-playerbots", "BotMgr heartbeat: account {} -> {}",
            session->GetAccountId(),
            bot ? (bot->IsInWorld() ? "in world" : "player exists, not in world") : "no player yet (login pending)");
    }

    CheckAllBotsMaxLevel();
}

void BotMgr::CheckAllBotsMaxLevel()
{
    uint8 maxLevel = uint8(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));

    bool anyBot = false;
    bool allMax = true;
    for (WorldSession* session : _botSessions)
    {
        Player* bot = session->GetPlayer();
        if (!bot || !bot->IsInWorld())
            continue;
        anyBot = true;
        if (bot->GetLevel() < maxLevel)
        {
            allMax = false;
            break;
        }
    }

    if (!anyBot || !allMax)
    {
        _allBotsMaxLevelNotified = false; // re-arm: a fresh notice is warranted next time this becomes true
        return;
    }

    if (_allBotsMaxLevelNotified)
        return;
    _allBotsMaxLevelNotified = true;

    LOG_INFO("module.coa-playerbots",
        "BotMgr: every active bot companion is already level {} -- no lower-level one left in the "
        "active roster to keep leveling. Spawning a fresh 'twink' isn't automated (no config-listed "
        "twink pool exists yet, and this project never creates characters without being asked) -- "
        "spawn an existing lower-level character yourself with `.botcmd spawnbot <guid>` if you want "
        "someone leveling in the world again.", uint32(maxLevel));
}

namespace
{
class coa_bot_restore_group_script : public PlayerScript
{
public:
    coa_bot_restore_group_script() : PlayerScript("coa_bot_restore_group_script",
        { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_CAN_GROUP_INVITE }) { }

    void OnPlayerLogin(Player* player) override
    {
        sBotMgr->RestoreGroupBotsOnLogin(player);
    }

    // HandleGroupInviteOpcode calls this *before* the ERR_ALREADY_IN_GROUP check, so a bot
    // that is already grouped can leave first and the rest of the invite proceeds normally.
    // The client /invite path, .botcmd invitebot, Quick Fill and INVITEBOT all go through that opcode.
    bool OnPlayerCanGroupInvite(Player* player, std::string& membername) override
    {
        if (!sBotMgr->BotsEnabled() || !player)
            return true;
        Player* target = ObjectAccessor::FindPlayerByName(membername, false);
        if (!target || !sBotMgr->FindBotPlayer(target->GetGUID().GetCounter()))
            return true;
        if (target->GetGroup() && target->GetGroup() != player->GetGroup())
            sBotMgr->TryReleaseBotFromGroup(player, target);
        return true;
    }
};
}

void AddSC_coa_bot_restore_group_script()
{
    new coa_bot_restore_group_script();
}

namespace
{
// Confirmed live: a real player leaving/being removed from a group that still has bots in it
// left the bots grouped together with a BOT promoted to leader (Group's normal
// leader-succession picks the next member regardless of bot/real status) -- they kept following
// whatever stale leader-position state they had instead of resuming their own idle/solo
// behavior, since nothing ever told them the group no longer has a real commander. Only acts
// when the departing member is real AND no real player is left afterward -- a group with two
// real players where one leaves should leave the bots alone, and a bot leaving/getting kicked/
// despawned on its own shouldn't cascade into disbanding everyone else's group.
class coa_bot_leaderless_group_script : public GroupScript
{
public:
    coa_bot_leaderless_group_script() : GroupScript("coa_bot_leaderless_group_script", { GROUPHOOK_ON_REMOVE_MEMBER, GROUPHOOK_ON_DISBAND }) { }

    // A disbanded group's shared combat snapshot (interrupt assignments) would otherwise wait for the idle sweep.
    void OnDisband(Group* group) override
    {
        if (group)
            BotAI::GroupDirector::ForgetGroup(group->GetGUID());
    }

    void OnRemoveMember(Group* group, ObjectGuid guid, RemoveMethod /*method*/, ObjectGuid /*kicker*/, char const* /*reason*/) override
    {
        if (!sBotMgr->BotsEnabled() || !group)
            return;
        if (BotMgr::IsBotAccountId(sCharacterCache->GetCharacterAccountIdByGuid(guid)))
            return; // a bot leaving on its own isn't a reason for the rest to bail too

        for (Group::MemberSlot const& slot : group->GetMemberSlots())
            if (!BotMgr::IsBotAccountId(sCharacterCache->GetCharacterAccountIdByGuid(slot.guid)))
                return; // another real player is still here -- bots still have someone to follow

        for (Group::MemberSlot const& slot : group->GetMemberSlots())
            sBotMgr->QueueBotGroupLeave(slot.guid);
    }
};
}

void AddSC_coa_bot_leaderless_group_script()
{
    new coa_bot_leaderless_group_script();
}
