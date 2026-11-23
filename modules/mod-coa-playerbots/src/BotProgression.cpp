#include "engine/BotDebugLog.h"
#include "BotProgression.h"
#include "BotProfessionSelect.h"
#include "BotAI.h"
#include "BotMgr.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "TemporarySummon.h"
#include "Trainer.h"
#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    // npc_ascension_training_book (mod-ascension-compat): the gossip a player uses to receive the
    // class abilities their level allows while CoA.AutoProgression is off.
    constexpr uint32 BOOK_OF_ASCENSION_CREATURE = 75115;
    constexpr uint32 BOOK_RESTORE_ABILITIES_ACTION = GOSSIP_ACTION_INFO_DEF + 1;

    // Book of Artisans, the "Personal Guide" profession trainer (creature_default_trainer 200001).
    constexpr uint32 BOOK_OF_ARTISANS_CREATURE = 57500;

    // Companion summon spells, as taught by the items 750750 (Book of Artisans) and 98457
    // (Book of Ascension) -- verified in item_template.spellid_2.
    constexpr uint32 SPELL_COMPANION_BOOK_OF_ARTISANS = 750750;
    constexpr uint32 SPELL_COMPANION_BOOK_OF_ASCENSION = 979602;

    constexpr uint32 SKILL_WOODCUTTING = 732;

    struct RaceMounts
    {
        uint8 race;
        uint32 ground60;
        uint32 ground100;
    };

    // Resolved from each mount item's spellid_2 in this realm's item_template rather than recalled,
    // which caught at least one id that differs from memory (Swift Gray Ram is 23239 here).
    constexpr std::array<RaceMounts, 10> RACE_MOUNTS =
    {{
        { RACE_HUMAN,         458,   23229 },
        { RACE_ORC,           6654,  23250 },
        { RACE_DWARF,         6777,  23239 },
        { RACE_NIGHTELF,      10793, 23221 },
        { RACE_UNDEAD_PLAYER, 17462, 17465 },
        { RACE_TAUREN,        18990, 23249 },
        { RACE_GNOME,         10873, 23225 },
        { RACE_TROLL,         8395,  23241 },
        { RACE_BLOODELF,      34795, 33660 },
        { RACE_DRAENEI,       34406, 35713 },
    }};

    constexpr uint32 FLYING_ALLIANCE_150 = 32235; // Golden Gryphon
    constexpr uint32 FLYING_ALLIANCE_280 = 32242; // Swift Blue Gryphon
    constexpr uint32 FLYING_HORDE_150 = 32243;    // Tawny Wind Rider
    constexpr uint32 FLYING_HORDE_280 = 32246;    // Swift Red Wind Rider

    // One tool per profession that needs one, chosen by the engine's TotemCategory rather than by
    // name -- several CoA tools are heirlooms with level 10 required, which only matters for
    // equipping, not for a tool that just has to be in the bags.
    constexpr std::array<std::pair<uint32, uint32>, 7> PROFESSION_TOOLS =
    {{
        { SKILL_MINING,        2901 },   // Mining Pick
        { SKILL_SKINNING,      7005 },   // Skinning Knife
        { SKILL_BLACKSMITHING, 5956 },   // Blacksmith Hammer
        { SKILL_ENGINEERING,   621922 }, // Arclight Spanner
        { SKILL_ENGINEERING,   10498 },  // Gyromatic Micro-Adjustor
        { SKILL_INSCRIPTION,   39505 },  // Virtuoso Inking Set
        { SKILL_WOODCUTTING,   6954 },   // Lumber Axe
    }};

    constexpr uint32 FISHING_POLE = 6256;

    // The riding spells mod-ascension-compat's InitializeRiding teaches every character when
    // CoA.MaxRidingFromStart is on: Apprentice, Journeyman, Expert, Artisan riding and
    // Cold Weather Flying.
    constexpr std::array<uint32, 5> RIDING_SPELLS = { 33388, 33391, 34090, 34091, 54197 };

    // Enchanting rods by the skill at which recipes start asking for them. A higher rod's
    // TotemCategory also satisfies the lower ones, so only the best one needs adding; older rods are
    // kept like every other tool (a bot never throws a profession tool away).
    constexpr std::array<std::pair<uint16, uint32>, 10> ENCHANTING_RODS =
    {{
        { 0,   6218 },  // Runed Copper Rod
        { 100, 6339 },  // Runed Silver Rod
        { 150, 11130 }, // Runed Golden Rod
        { 200, 11145 }, // Runed Truesilver Rod
        { 275, 16207 }, // Runed Arcanite Rod
        { 300, 22461 }, // Runed Fel Iron Rod
        { 335, 22462 }, // Runed Adamantite Rod
        { 350, 22463 }, // Runed Eternium Rod
        { 400, 44451 }, // Runed Cobalt Rod
        { 425, 44452 }, // Runed Titanium Rod
    }};

    constexpr std::array<uint32, 16> PROFESSION_SKILLS =
    {
        SKILL_ALCHEMY, SKILL_BLACKSMITHING, SKILL_ENCHANTING, SKILL_ENGINEERING, SKILL_LEATHERWORKING,
        SKILL_TAILORING, SKILL_HERBALISM, SKILL_MINING, SKILL_SKINNING, SKILL_JEWELCRAFTING, SKILL_INSCRIPTION,
        SKILL_COOKING, SKILL_FIRST_AID, SKILL_FISHING, SKILL_WOODCUTTING, 757 /* Woodworking */,
    };

    uint16 ProfessionCapFor(uint8 level)
    {
        return uint16(std::max<uint32>(1, std::min<uint32>(450, uint32(level) * 6)));
    }

    // CoA's own trades stop at Artisan: the client's SkillLineAbility.dbc chains four ranks for
    // Woodcutting (732) and Woodworking (757), so their ceiling is 300, not 450.
    uint16 ProfessionMaxSkill(uint32 skillId)
    {
        return (skillId == SKILL_WOODCUTTING || skillId == 757) ? 300 : 450;
    }

    uint16 ProfessionCapFor(uint32 skillId, uint8 level)
    {
        return std::min<uint16>(ProfessionCapFor(level), ProfessionMaxSkill(skillId));
    }

    // Rank step the client shows for a ceiling (1 Apprentice .. 6 Grand Master), as a trainer sets it.
    uint16 ProfessionStepFor(uint16 cap)
    {
        return uint16(std::max<uint32>(1, std::min<uint32>(6, (uint32(cap) + 74) / 75)));
    }

    bool HasItem(Player* bot, uint32 itemId)
    {
        return bot->HasItemCount(itemId, 1, true);
    }

    // Straight into the bags, never an equipment slot. Several CoA tools are real weapons (the Mining
    // Pick is a main-hand heirloom), and StoreNewItemInBestSlots equips into any empty slot first --
    // confirmed live: bots whose gear-up found no weapon ended up fighting with their Mining Pick.
    void GiveOnce(Player* bot, uint32 itemId, uint32 count = 1)
    {
        if (HasItem(bot, itemId) || !sObjectMgr->GetItemTemplate(itemId))
            return;

        ItemPosCountVec dest;
        if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, count) == EQUIP_ERR_OK)
            bot->StoreNewItem(dest, itemId, true);
    }

    struct Consumable
    {
        uint32 entry;
        uint32 requiredLevel;
        uint32 vendors;
    };

    struct ConsumableTable
    {
        std::vector<Consumable> food;
        std::vector<Consumable> drink;
    };

    // Vendor-sold food (spellcategory 11) and drink (59) from the world DB, loaded once. The drink
    // category also holds alcohol that restores next to no mana, so among items of one level the one
    // sold by the most vendors wins -- that is ordinary water and bread in practice.
    ConsumableTable const& Consumables()
    {
        static ConsumableTable table = []
        {
            ConsumableTable loaded;
            QueryResult result = WorldDatabase.Query(
                "SELECT it.entry, it.RequiredLevel, it.spellcategory_1, COUNT(*) FROM item_template it "
                "JOIN npc_vendor v ON v.item = it.entry WHERE it.class = 0 AND it.subclass = 5 "
                "AND it.spellcategory_1 IN (11, 59) GROUP BY it.entry, it.RequiredLevel, it.spellcategory_1");
            if (result)
            {
                do
                {
                    Field* fields = result->Fetch();
                    Consumable item{ fields[0].Get<uint32>(), fields[1].Get<uint32>(),
                        uint32(fields[3].Get<uint64>()) };
                    (fields[2].Get<uint32>() == 11 ? loaded.food : loaded.drink).push_back(item);
                } while (result->NextRow());
            }
            LOG_INFO("module.coa-playerbots", ">> Loaded {} vendor food and {} vendor drink items for bot supplies.",
                loaded.food.size(), loaded.drink.size());
            return loaded;
        }();
        return table;
    }

    uint32 PickConsumable(std::vector<Consumable> const& items, uint8 level)
    {
        Consumable const* best = nullptr;
        for (Consumable const& item : items)
        {
            if (!item.requiredLevel || item.requiredLevel > level)
                continue;
            if (!best || item.requiredLevel > best->requiredLevel ||
                (item.requiredLevel == best->requiredLevel && item.vendors > best->vendors))
                best = &item;
        }
        return best ? best->entry : 0;
    }
}

namespace BotProgression
{
    void LearnAbilitiesFromBook(Player* bot)
    {
        if (!bot || !bot->IsInWorld())
            return;

        TempSummon* book = bot->SummonCreature(BOOK_OF_ASCENSION_CREATURE, bot->GetPositionX() + 1.0f,
            bot->GetPositionY(), bot->GetPositionZ(), bot->GetOrientation(), TEMPSUMMON_TIMED_DESPAWN, 5000);
        if (!book)
        {
            LOG_WARN(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotProgression: could not summon a Book of Ascension for '{}'.",
                bot->GetName());
            return;
        }

        uint32 before = uint32(bot->GetSpellMap().size());
        sScriptMgr->OnGossipSelect(bot, book, GOSSIP_SENDER_MAIN, BOOK_RESTORE_ABILITIES_ACTION);
        book->DespawnOrUnsummon();

        uint32 after = uint32(bot->GetSpellMap().size());
        if (after > before)
            LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()),
                "BotProgression: '{}' learned {} spell(s) from its Book of Ascension at level {}.",
                bot->GetName(), after - before, bot->GetLevel());
    }

    uint32 LearnRecipesFromBook(Player* bot)
    {
        if (!bot)
            return 0;

        Trainer::Trainer* trainer = sObjectMgr->GetTrainer(BOOK_OF_ARTISANS_CREATURE);
        if (!trainer)
            return 0;

        // Recipes chain through ReqAbility, so a pass can unlock the next rank; repeat until a pass
        // teaches nothing. Bounded, since each pass either learns something or ends the loop.
        uint32 learned = 0;
        for (uint32 pass = 0; pass < 8; ++pass)
        {
            uint32 learnedThisPass = 0;
            for (Trainer::Spell const& spell : trainer->GetSpells())
            {
                if (!trainer->CanTeachSpell(bot, &spell))
                    continue;

                if (spell.IsCastable())
                    bot->CastSpell(bot, spell.SpellId, true);
                else
                    bot->learnSpell(spell.SpellId, false);
                ++learnedThisPass;
            }

            learned += learnedThisPass;
            if (!learnedThisPass)
                break;
        }

        if (learned)
            LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotProgression: '{}' learned {} recipe(s) from its Book of Artisans.",
                bot->GetName(), learned);
        return learned;
    }

    void GrantCompanions(Player* bot)
    {
        for (uint32 spellId : { SPELL_COMPANION_BOOK_OF_ARTISANS, SPELL_COMPANION_BOOK_OF_ASCENSION })
            if (!bot->HasSpell(spellId) && sSpellMgr->GetSpellInfo(spellId))
                bot->learnSpell(spellId, false);
    }

    void GrantMounts(Player* bot)
    {
        // Keep provisioning aligned with the core's riding setting, including config reloads.
        bool const maxRidingFromStart = sConfigMgr->GetOption<bool>("CoA.MaxRidingFromStart", true);
        if (maxRidingFromStart && bot->GetBaseSkillValue(SKILL_RIDING) < 300)
        {
            for (uint32 spellId : RIDING_SPELLS)
                if (!bot->HasSpell(spellId) && sSpellMgr->GetSpellInfo(spellId))
                    bot->learnSpell(spellId, false);
            bot->SetSkill(SKILL_RIDING, 4, 300, 300);
        }

        std::vector<uint32> mounts;
        for (RaceMounts const& entry : RACE_MOUNTS)
        {
            if (entry.race != bot->getRace())
                continue;
            mounts.push_back(entry.ground60);
            mounts.push_back(entry.ground100);
        }

        bool alliance = Player::TeamIdForRace(bot->getRace()) == TEAM_ALLIANCE;
        // CoA's custom races have no stock racial mount: give them their faction's horse or wolf,
        // or they could only ever fly and never follow a leader on a ground mount.
        if (mounts.empty())
        {
            mounts.push_back(alliance ? 458u : 6654u);
            mounts.push_back(alliance ? 23229u : 23250u);
        }
        mounts.push_back(alliance ? FLYING_ALLIANCE_150 : FLYING_HORDE_150);
        mounts.push_back(alliance ? FLYING_ALLIANCE_280 : FLYING_HORDE_280);

        for (uint32 spellId : mounts)
            if (!bot->HasSpell(spellId) && sSpellMgr->GetSpellInfo(spellId))
                bot->learnSpell(spellId, false);
    }

    // Learn/forget hooks fire on whichever thread changed the spellbook (a level-up happens on a map thread), so the counters are guarded.
    std::mutex s_spellbookMutex;
    std::unordered_map<ObjectGuid, uint32> s_spellbookRevision;

    void BumpSpellbookRevision(Player* player)
    {
        if (!player || !player->IsBot())
            return;
        std::lock_guard<std::mutex> lock(s_spellbookMutex);
        ++s_spellbookRevision[player->GetGUID()];
    }

    // The item that satisfies a spell's required TotemCategory, picked once per category: a plain tool (not a wearable) first, then the
    // lowest item level, then the lowest entry. 0 when the realm has no such item.
    uint32 ToolForTotemCategory(uint32 category)
    {
        static std::unordered_map<uint32, uint32> cache;
        auto found = cache.find(category);
        if (found != cache.end())
            return found->second;

        ItemTemplate const* best = nullptr;
        for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
        {
            if (proto.TotemCategory != category)
                continue;
            auto rank = [](ItemTemplate const& item) { return std::tuple<int, uint32, uint32>(item.InventoryType == INVTYPE_NON_EQUIP ? 0 : 1, item.ItemLevel, item.ItemId); };
            if (!best || rank(proto) < rank(*best))
                best = &proto;
        }
        return cache[category] = best ? best->ItemId : 0;
    }

    // Spells such as the Witch Doctor's totems and wards or the Guardian's standards need a tool in the bags (Spell.dbc TotemCategory); a bot
    // that was never given one answered every such cast with "missing totem category item", so those specs never used half their kit.
    void GrantSpellTools(Player* bot)
    {
        for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
        {
            if (!playerSpell || playerSpell->State == PLAYERSPELL_REMOVED || !playerSpell->Active)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || info->IsPassive())
                continue;
            for (uint32 category : info->TotemCategory)
            {
                if (!category || bot->HasItemTotemCategory(category))
                    continue;
                if (uint32 tool = ToolForTotemCategory(category))
                    GiveOnce(bot, tool);
            }
        }
    }

    uint32 SpellbookRevision(ObjectGuid botGuid)
    {
        std::lock_guard<std::mutex> lock(s_spellbookMutex);
        auto found = s_spellbookRevision.find(botGuid);
        return found != s_spellbookRevision.end() ? found->second : 0;
    }

    void ForgetSpellbook(ObjectGuid botGuid)
    {
        std::lock_guard<std::mutex> lock(s_spellbookMutex);
        s_spellbookRevision.erase(botGuid);
    }

    void GrantProfessionTools(Player* bot)
    {
        GrantSpellTools(bot);

        for (auto const& [skill, itemId] : PROFESSION_TOOLS)
            if (bot->HasSkill(skill))
                GiveOnce(bot, itemId);

        if (bot->HasSkill(SKILL_FISHING) && !HasItem(bot, FISHING_POLE))
            GiveOnce(bot, FISHING_POLE);

        if (!bot->HasSkill(SKILL_ENCHANTING))
            return;

        uint16 enchanting = bot->GetSkillValue(SKILL_ENCHANTING);
        uint32 wanted = 0;
        for (auto const& [minSkill, rod] : ENCHANTING_RODS)
            if (enchanting >= minSkill)
                wanted = rod;

        GiveOnce(bot, wanted);
    }

    uint32 PickFood(uint8 level)
    {
        return PickConsumable(Consumables().food, level);
    }

    uint32 PickDrink(uint8 level)
    {
        return PickConsumable(Consumables().drink, level);
    }

    namespace
    {
        // The ordinary healing and mana potion of each tier (required level, item). The world database holds dozens of look-alikes (injectors,
        // battleground and "contraband" versions); these are the plain ones every tier has.
        struct PotionTier { uint8 level; uint32 entry; };
        constexpr PotionTier HEALING_TIERS[] = { {1, 118}, {3, 858}, {12, 929}, {21, 1710}, {35, 3928}, {45, 13446}, {55, 22829} };
        constexpr PotionTier MANA_TIERS[] = { {5, 2455}, {14, 3385}, {20, 3827}, {31, 6149}, {41, 13443}, {49, 13444}, {55, 22832} };

        template <size_t N>
        uint32 PickPotionTier(PotionTier const (&tiers)[N], uint8 level)
        {
            uint32 best = 0;
            for (PotionTier const& tier : tiers)
                if (tier.level <= level && sObjectMgr->GetItemTemplate(tier.entry))
                    best = tier.entry;
            return best;
        }
    }

    uint32 PickHealingPotion(uint8 level)
    {
        return PickPotionTier(HEALING_TIERS, level);
    }

    uint32 PickManaPotion(uint8 level)
    {
        return PickPotionTier(MANA_TIERS, level);
    }

    void ProvisionPotions(Player* bot, uint32 count)
    {
        if (!bot || !count)
            return;
        auto give = [&](uint32 entry)
        {
            if (!entry || bot->GetItemCount(entry) >= count)
                return;
            bot->StoreNewItemInBestSlots(entry, count - bot->GetItemCount(entry));
        };
        give(PickHealingPotion(bot->GetLevel()));
        if (bot->getPowerType() == POWER_MANA)
            give(PickManaPotion(bot->GetLevel()));
    }

    void ProvisionFood(Player* bot, uint32 stackSize)
    {
        ConsumableTable const& table = Consumables();
        uint8 level = bot->GetLevel();
        for (uint32 itemId : { PickConsumable(table.food, level), PickConsumable(table.drink, level) })
            if (itemId && !HasItem(bot, itemId))
                bot->StoreNewItemInBestSlots(itemId, stackSize);
    }

    void LearnRankSpells(Player* bot, uint32 skillId, uint16 cap);

    void OnLevelUp(Player* bot, uint8 newLevel)
    {
        if (!bot)
            return;

        // Caps follow the level; values climb part of the way, so a bot's crafting and gathering
        // grow while it levels without jumping to the cap.
        for (uint32 skill : PROFESSION_SKILLS)
        {
            if (!bot->HasSkill(skill))
                continue;
            uint16 cap = ProfessionCapFor(skill, newLevel);
            uint16 value = bot->GetSkillValue(skill);
            uint16 grown = uint16(std::min<uint32>(cap, uint32(value) + 4));
            LearnRankSpells(bot, skill, cap);
            bot->SetSkill(uint16(skill), ProfessionStepFor(cap), grown, cap);
        }

        LearnAbilitiesFromBook(bot);
        LearnRecipesFromBook(bot);
        GrantProfessionTools(bot);
        ProvisionFood(bot, 20);
        ProvisionPotions(bot, 5);
    }

    // Trainer rank spells (Apprentice through Grand Master) for each stock profession. Used so
    // SetSkill caps are legal the same way a visit to a profession trainer would make them.
    // Every id below was checked against the CoA client's SkillLineAbility.dbc (each spell is an
    // ability of exactly that skill line). Woodcutting (732) and Woodworking (757) are CoA's own
    // trades with four ranks (Apprentice..Artisan), chained in that file by SupercededBySpell;
    // 13977859 and 1005008 are the spells mod-woodworking watches for the apprentice rank.
    struct ProfessionRankSpells
    {
        uint32 skillId;
        uint32 spells[6];
    };

    constexpr ProfessionRankSpells PROFESSION_RANK_SPELLS[] =
    {
        { 171, { 2259, 3101, 3464, 11611, 28596, 51304 } }, // Alchemy
        { 164, { 2018, 3100, 3538,  9785, 29844, 51300 } }, // Blacksmithing
        { 333, { 7411, 7412, 7413, 13920, 28029, 51313 } }, // Enchanting
        { 202, { 4036, 4037, 4038, 12656, 30350, 51306 } }, // Engineering
        { 165, { 2108, 3104, 3811, 10662, 32549, 51302 } }, // Leatherworking
        { 197, { 3908, 3909, 3910, 12180, 26790, 51309 } }, // Tailoring
        { 182, { 2366, 2368, 3570, 11993, 28695, 50300 } }, // Herbalism
        { 186, { 2575, 2576, 3564, 10248, 29354, 50310 } }, // Mining
        { 393, { 8613, 8617, 8618, 10768, 32678, 50305 } }, // Skinning
        { 755, { 25229, 25230, 28894, 28895, 28897, 51311 } }, // Jewelcrafting
        { 773, { 45357, 45358, 45359, 45360, 45361, 45363 } }, // Inscription
        { 185, { 2550, 3102, 3413, 18260, 33359, 51296 } }, // Cooking
        { 129, { 3273, 3274, 7924, 10846, 27028, 45542 } }, // First Aid
        { 356, { 7620, 7731, 7732, 18248, 33095, 51294 } }, // Fishing
        { 732, { 13977859, 13977882, 13977883, 13977884, 0, 0 } }, // Woodcutting (CoA)
        { 757, { 1005008, 1005009, 1005010, 1005011, 0, 0 } },     // Woodworking (CoA)
    };

    constexpr uint32 PROFESSION_RANK_CAPS[6] = { 75, 150, 225, 300, 375, 450 };

    constexpr ProfessionInfo PROFESSION_CATALOG[] =
    {
        { 171, "Alchemy",        false },
        { 164, "Blacksmithing",  false },
        { 333, "Enchanting",     false },
        { 202, "Engineering",    false },
        { 165, "Leatherworking", false },
        { 197, "Tailoring",      false },
        { 182, "Herbalism",      false },
        { 186, "Mining",         false },
        { 393, "Skinning",       false },
        { 755, "Jewelcrafting",  false },
        { 773, "Inscription",    false },
        { 732, "Woodcutting",    false },
        { 757, "Woodworking",    false },
        { 185, "Cooking",        true  },
        { 129, "First Aid",      true  },
        { 356, "Fishing",        true  },
    };

    ProfessionRankSpells const* RanksFor(uint32 skillId)
    {
        for (ProfessionRankSpells const& row : PROFESSION_RANK_SPELLS)
            if (row.skillId == skillId)
                return &row;
        return nullptr;
    }

    char const* ProfessionBusyToken(Player* bot)
    {
        if (!bot)
            return "invalid";
        if (!bot->IsAlive())
            return "dead";
        if (bot->IsInCombat())
            return "combat";
        if (bot->GetTrader())
            return "trade";
        if (bot->IsBeingTeleported())
            return "teleport";
        return nullptr;
    }

    void LearnRankSpells(Player* bot, uint32 skillId, uint16 cap)
    {
        ProfessionRankSpells const* ranks = RanksFor(skillId);
        if (!ranks)
            return;
        uint32 previousCap = 0;
        for (uint32 i = 0; i < 6; ++i)
        {
            uint32 spellId = ranks->spells[i];
            // A rank spell missing from this realm's Spell.dbc is skipped rather than learned blind.
            if (spellId && cap > previousCap && !bot->HasSpell(spellId) && sSpellMgr->GetSpellInfo(spellId))
                bot->learnSpell(spellId, false);
            previousCap = PROFESSION_RANK_CAPS[i];
        }
    }

    void ForgetRankSpells(Player* bot, uint32 skillId)
    {
        ProfessionRankSpells const* ranks = RanksFor(skillId);
        if (!ranks)
            return;
        for (uint32 i = 0; i < 6; ++i)
        {
            uint32 spellId = ranks->spells[i];
            if (spellId && bot->HasSpell(spellId))
                bot->removeSpell(spellId, SPEC_MASK_ALL, false);
        }
    }

    ProfessionInfo const* ProfessionCatalog(uint32& count)
    {
        count = uint32(sizeof(PROFESSION_CATALOG) / sizeof(PROFESSION_CATALOG[0]));
        return PROFESSION_CATALOG;
    }

    uint16 ProfessionCapForLevel(uint8 level)
    {
        return ProfessionCapFor(level);
    }

    char const* LearnProfession(Player* bot, uint32 skillId)
    {
        if (char const* busy = ProfessionBusyToken(bot))
            return busy;
        if (!skillId)
            return "invalid";
        bool known = false;
        for (ProfessionInfo const& info : PROFESSION_CATALOG)
            if (info.skillId == skillId)
                known = true;
        if (!known)
            return "unknown";

        uint16 cap = ProfessionCapFor(skillId, bot->GetLevel());
        uint32 configured = sConfigMgr->GetOption<uint32>("CoaBots.Prof.StartSkill", 0);
        uint16 value = configured == 0 ? cap : uint16(std::max<uint32>(1, std::min<uint32>(cap, configured)));
        if (bot->HasSkill(skillId))
        {
            uint16 current = bot->GetSkillValue(skillId);
            if (current > value)
                value = current;
        }
        if (value > cap)
            value = cap;
        if (!sSkillLineStore.LookupEntry(skillId))
            return "noskill"; // this realm's SkillLine.dbc has no such line; SetSkill would silently do nothing
        LearnRankSpells(bot, skillId, cap);
        bot->SetSkill(uint16(skillId), ProfessionStepFor(cap), value, cap);
        if (!bot->HasSkill(skillId))
            return "noskill";
        GrantProfessionTools(bot);
        LearnRecipesFromBook(bot);
        return "ok";
    }

    char const* UnlearnProfession(Player* bot, uint32 skillId)
    {
        if (char const* busy = ProfessionBusyToken(bot))
            return busy;
        if (!skillId)
            return "invalid";
        bool known = false;
        for (ProfessionInfo const& info : PROFESSION_CATALOG)
            if (info.skillId == skillId)
                known = true;
        if (!known)
            return "unknown";
        if (!bot->HasSkill(skillId))
            return "missing";
        ForgetRankSpells(bot, skillId);
        bot->SetSkill(uint16(skillId), 0, 0, 0);
        if (bot->HasSkill(skillId))
            return "invalid";
        return "ok";
    }

    uint16 ProfessionCapForSkill(uint32 skillId, uint8 level)
    {
        return ProfessionCapFor(skillId, level);
    }

    constexpr char const* PROF_SETTING = "coa.bot.prof";
    constexpr uint32 PROF_FLAG_ASSIGNED = 1u;
    constexpr uint32 PROF_FLAG_MANUAL = 2u;
    constexpr uint32 LEGACY_PRIMARY_THRESHOLD = 10;

    uint32 ReadProfessionFlags(Player* bot)
    {
        return bot ? bot->GetPlayerSetting(PROF_SETTING, 0).value : 0;
    }

    void WriteProfessionFlags(Player* bot, uint32 flags)
    {
        if (bot)
            bot->UpdatePlayerSetting(PROF_SETTING, 0, flags);
    }

    uint32 CountPrimaryProfessions(Player* bot)
    {
        if (!bot)
            return 0;
        uint32 count = 0;
        for (ProfessionInfo const& info : PROFESSION_CATALOG)
            if (!info.secondary && bot->HasSkill(info.skillId))
                ++count;
        return count;
    }

    bool ProfessionsAreManual(Player* bot)
    {
        return (ReadProfessionFlags(bot) & PROF_FLAG_MANUAL) != 0;
    }

    bool HasLegacyProfessionSet(Player* bot)
    {
        return CountPrimaryProfessions(bot) >= LEGACY_PRIMARY_THRESHOLD;
    }

    void MarkProfessionsManual(Player* bot)
    {
        if (!bot)
            return;
        WriteProfessionFlags(bot, ReadProfessionFlags(bot) | PROF_FLAG_MANUAL);
    }

    void ApplyProfessionSkill(Player* bot, uint32 skillId, bool atCap)
    {
        if (!bot || !skillId || !sSkillLineStore.LookupEntry(skillId))
            return;
        uint16 cap = ProfessionCapFor(skillId, bot->GetLevel());
        uint16 value = atCap
            ? cap
            : BotProfessionSelect::SkillValueFor(bot->GetGUID().GetCounter(), skillId, cap);
        if (bot->HasSkill(skillId))
        {
            uint16 current = bot->GetSkillValue(skillId);
            if (current > value)
                value = current;
        }
        if (value > cap)
            value = cap;
        LearnRankSpells(bot, skillId, cap);
        bot->SetSkill(uint16(skillId), ProfessionStepFor(cap), value, cap);
    }

    void ForceClearProfession(Player* bot, uint32 skillId)
    {
        if (!bot || !bot->HasSkill(skillId))
            return;
        ForgetRankSpells(bot, skillId);
        bot->SetSkill(uint16(skillId), 0, 0, 0);
    }

    void GrantProfessionAtCap(Player* bot, uint32 skillId)
    {
        ApplyProfessionSkill(bot, skillId, true);
        if (bot)
            GrantProfessionTools(bot);
    }

    char const* AssignVariedProfessions(Player* bot, bool force)
    {
        if (!bot)
            return "invalid";
        if (!force && ProfessionsAreManual(bot))
            return "manual";
        if (!force)
            if (char const* busy = ProfessionBusyToken(bot))
                return busy;

        bool const twoPrimary = sConfigMgr->GetOption<bool>("CoaBots.Professions.TwoPrimary", true);
        bool const grantSecondaries = sConfigMgr->GetOption<bool>("CoaBots.Professions.GrantSecondaries", true);

        uint8 const rawClassId = bot->getClass();
        uint8 const legacyClassId = uint8(GetLegacyClassForCustomClass(Classes(rawClassId)));
        BotProfessionSelect::Pick pick{};
        if (twoPrimary)
            pick = BotProfessionSelect::ChoosePrimaryPair(bot->GetGUID().GetCounter(), rawClassId, legacyClassId);

        for (ProfessionInfo const& info : PROFESSION_CATALOG)
        {
            if (info.secondary)
                continue;
            bool const keep = !twoPrimary || info.skillId == pick.skillA || info.skillId == pick.skillB;
            if (keep)
                ApplyProfessionSkill(bot, info.skillId, false);
            else
                ForceClearProfession(bot, info.skillId);
        }

        if (grantSecondaries)
            for (ProfessionInfo const& info : PROFESSION_CATALOG)
                if (info.secondary)
                    ApplyProfessionSkill(bot, info.skillId, false);

        GrantProfessionTools(bot);
        LearnRecipesFromBook(bot);

        uint32 flags = ReadProfessionFlags(bot) | PROF_FLAG_ASSIGNED;
        if (force)
            flags &= ~PROF_FLAG_MANUAL;
        WriteProfessionFlags(bot, flags);

        if (twoPrimary)
            LOG_INFO("module.coa-playerbots", "BotProgression: '{}' (guid {}, class {}) professions: {} + {}.",
                bot->GetName(), bot->GetGUID().GetCounter(), uint32(rawClassId),
                BotProfessionSelect::SkillName(pick.skillA), BotProfessionSelect::SkillName(pick.skillB));
        return "ok";
    }

    void MaybeRerollLegacyProfessions(Player* bot)
    {
        if (!bot || !bot->GetSession())
            return;
        if (!BotMgr::IsBotAccountId(bot->GetSession()->GetAccountId()))
            return;
        if (!sConfigMgr->GetOption<bool>("CoaBots.Professions.RerollLegacy", true))
            return;
        if (ProfessionsAreManual(bot))
            return;
        if (ReadProfessionFlags(bot) & PROF_FLAG_ASSIGNED)
            return;
        if (!HasLegacyProfessionSet(bot))
            return;
        char const* token = AssignVariedProfessions(bot, false);
        if (token && token[0] == 'o')
            LOG_INFO("module.coa-playerbots", "BotProgression: rerolled legacy professions for '{}'.", bot->GetName());
        else
            LOG_DEBUG("module.coa-playerbots", "BotProgression: deferred legacy profession reroll for '{}' ({})",
                bot->GetName(), token ? token : "invalid");
    }

    void AppendProfessionStats(std::vector<std::string>& lines)
    {
        std::vector<Player*> bots = sBotMgr->GetOnlineBots();
        uint32 catalogCount = 0;
        ProfessionInfo const* catalog = ProfessionCatalog(catalogCount);

        std::map<uint32, uint32> skillCounts;
        std::map<std::string, uint32> pairCounts;
        uint32 twoPrimary = 0;
        uint32 legacy = 0;
        uint32 manual = 0;
        uint32 other = 0;
        uint32 none = 0;

        for (Player* bot : bots)
        {
            if (!bot)
                continue;
            if (ProfessionsAreManual(bot))
                ++manual;
            if (HasLegacyProfessionSet(bot))
                ++legacy;

            std::vector<uint32> primaries;
            for (uint32 i = 0; i < catalogCount; ++i)
            {
                ProfessionInfo const& info = catalog[i];
                if (!bot->HasSkill(info.skillId))
                    continue;
                ++skillCounts[info.skillId];
                if (!info.secondary)
                    primaries.push_back(info.skillId);
            }

            if (primaries.empty())
                ++none;
            else if (primaries.size() == 2)
            {
                ++twoPrimary;
                uint32 first = primaries[0];
                uint32 second = primaries[1];
                if (second < first)
                    std::swap(first, second);
                std::string label = std::string(BotProfessionSelect::SkillName(first)) + " + " +
                    BotProfessionSelect::SkillName(second);
                ++pairCounts[label];
            }
            else
                ++other;
        }

        lines.push_back(Acore::StringFormat("BotMgr: profession stats for {} online bot(s): {} with two primaries, {} with the old all-profession set, {} other, {} with none, {} marked manual.",
            uint32(bots.size()), twoPrimary, legacy, other, none, manual));
        for (uint32 i = 0; i < catalogCount; ++i)
        {
            ProfessionInfo const& info = catalog[i];
            lines.push_back(Acore::StringFormat("  {}: {}", info.name, skillCounts[info.skillId]));
        }
        if (!pairCounts.empty())
        {
            lines.push_back("  pairings:");
            for (auto const& [label, count] : pairCounts)
                lines.push_back(Acore::StringFormat("    {}: {}", label, count));
        }
    }
}

namespace
{
class coa_bot_spellbook_script : public PlayerScript
{
public:
    coa_bot_spellbook_script() : PlayerScript("coa_bot_spellbook_script",
        { PLAYERHOOK_ON_LEARN_SPELL, PLAYERHOOK_ON_FORGOT_SPELL, PLAYERHOOK_ON_LOGIN }) { }

    void OnPlayerLearnSpell(Player* player, uint32 /*spellId*/) override { BotProgression::BumpSpellbookRevision(player); }
    void OnPlayerForgotSpell(Player* player, uint32 /*spellId*/) override { BotProgression::BumpSpellbookRevision(player); }
    void OnPlayerLogin(Player* player) override { BotProgression::MaybeRerollLegacyProfessions(player); }
};
}

void AddSC_coa_bot_spellbook_script()
{
    new coa_bot_spellbook_script();
}
