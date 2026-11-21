#include "BotMaintenance.h"
#include "BotAI.h"
#include "BotEconomy.h"
#include "BotGear.h"
#include "BotMgr.h"
#include "Bag.h"
#include "CellImpl.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    struct Settings
    {
        bool repairOnLeaderVendor = true;
        bool buyAmmoOnLeaderVendor = true;
        bool autoTravel = true;
        float shareRange = 80.0f;
        float travelSearchYards = 30000.0f;
        float nearbyServiceYards = 80.0f;
        uint32 repairItemPct = 30;
        uint32 repairAveragePct = 50;
        uint32 ammoLow = 200;
        uint32 ammoFill = 1000;
        bool cleanupEnable = true;
        uint32 freeSlotTarget = 4;
        uint32 tradeGoodsKeep = 20;
        bool ensureBags = true;
        bool buyBags = true;
        uint32 bagBuyReserveCopper = 10000;
    } _settings;

    constexpr uint32 HEARTHSTONE_ENTRY = 6948;

    std::string FormatCopperLocal(uint32 copper)
    {
        uint32 gold = copper / 10000;
        uint32 silver = (copper / 100) % 100;
        uint32 coppers = copper % 100;
        if (gold)
            return Acore::StringFormat("{}g {}s", gold, silver);
        if (silver)
            return Acore::StringFormat("{}s {}c", silver, coppers);
        return Acore::StringFormat("{}c", coppers);
    }

    void SendAltBots(Player* player, std::string const& text)
    {
        if (player && player->GetSession())
            ChatHandler(player->GetSession()).SendSysMessage("AltBots: " + text);
    }

    Player* GatherRequesterOf(Player* bot)
    {
        if (!bot || !sBotMgr)
            return nullptr;
        BotMgr::GuildGatherOrder const* order = sBotMgr->GetGuildGatherOrder(bot->GetGUID());
        if (!order || !order->requesterLowGuid)
            return nullptr;
        return ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(order->requesterLowGuid));
    }

    bool UsableAmmoProto(Player const* bot, ItemTemplate const* proto, uint32 subClass)
    {
        if (!bot || !proto)
            return false;
        if (proto->Class != ITEM_CLASS_PROJECTILE || proto->SubClass != subClass)
            return false;
        if (proto->RequiredLevel && bot->GetLevel() < proto->RequiredLevel)
            return false;
        return bot->CanUseItem(proto) == EQUIP_ERR_OK;
    }

    VendorItemData const* VendorItemsOf(uint32 vendorEntry)
    {
        return sObjectMgr->GetNpcVendorItemList(vendorEntry);
    }

    // Best (highest item level, then quality, then midpoint damage) unlimited gold-priced
    // ammo this vendor sells that the bot can use.
    ItemTemplate const* BestVendorAmmo(uint32 vendorEntry, Player const* bot, uint32 subClass)
    {
        VendorItemData const* items = VendorItemsOf(vendorEntry);
        if (!items || items->Empty())
            return nullptr;

        ItemTemplate const* best = nullptr;
        float bestDamage = -1.0f;
        for (uint8 slot = 0; slot < items->GetItemCount(); ++slot)
        {
            VendorItem const* vItem = items->GetItem(slot);
            if (!vItem || vItem->maxcount || vItem->ExtendedCost)
                continue;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(vItem->item);
            if (!proto || !vItem->IsGoldRequired(proto) || !proto->BuyPrice)
                continue;
            if (!UsableAmmoProto(bot, proto, subClass))
                continue;
            float damage = (proto->Damage[0].DamageMin + proto->Damage[0].DamageMax) * 0.5f;
            if (!best || proto->ItemLevel > best->ItemLevel ||
                (proto->ItemLevel == best->ItemLevel && proto->Quality > best->Quality) ||
                (proto->ItemLevel == best->ItemLevel && proto->Quality == best->Quality && damage > bestDamage))
            {
                best = proto;
                bestDamage = damage;
            }
        }
        return best;
    }

    void ForEachInventoryItem(Player const* bot, auto const& fn)
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            fn(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            if (Bag const* pBag = bot->GetBagByPos(bag))
                for (uint32 i = 0; i < pBag->GetBagSize(); ++i)
                    fn(pBag->GetItemByPos(i));
    }

    enum class RepairCostKind : uint8
    {
        None,
        MissingTable,
        Affordable,
    };

    struct RepairCost
    {
        RepairCostKind kind = RepairCostKind::None;
        uint32 copper = 0;
    };

    RepairCost ComputeRepairCost(Item const* item, float discountMod)
    {
        RepairCost result;
        if (!item)
            return result;
        uint32 maxDurability = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
        uint32 curDurability = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
        if (!maxDurability || curDurability >= maxDurability)
            return result;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto)
            return result;

        DurabilityCostsEntry const* dcost = sDurabilityCostsStore.LookupEntry(proto->ItemLevel);
        uint32 qualityModId = (proto->Quality + 1) * 2;
        DurabilityQualityEntry const* qualityMod = sDurabilityQualityStore.LookupEntry(qualityModId);
        if (!dcost || !qualityMod)
        {
            result.kind = RepairCostKind::MissingTable;
            return result;
        }

        uint32 multiplierId = ItemSubClassToDurabilityMultiplierId(proto->Class, proto->SubClass);
        if (multiplierId >= 29)
        {
            result.kind = RepairCostKind::MissingTable;
            return result;
        }

        uint32 lost = maxDurability - curDurability;
        uint32 costs = uint32(lost * dcost->multiplier[multiplierId] * double(qualityMod->quality_mod));
        costs = uint32(costs * discountMod * sWorld->getRate(RATE_REPAIRCOST));
        if (!costs)
            costs = 1;
        result.kind = RepairCostKind::Affordable;
        result.copper = costs;
        return result;
    }

    void VisitRepairPositions(Player* bot, auto const& fn)
    {
        for (uint8 i = EQUIPMENT_SLOT_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            fn(uint16((INVENTORY_SLOT_BAG_0 << 8) | i));
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            for (uint8 i = 0; i < MAX_BAG_SIZE; ++i)
                fn(uint16((bag << 8) | i));
    }

    struct DurabilitySnapshot
    {
        uint32 pieces = 0;
        uint32 curSum = 0;
        uint32 maxSum = 0;
        bool anyBelowItemPct = false;
        bool anyDamaged = false;
    };

    DurabilitySnapshot SnapshotDurability(Player const* bot)
    {
        DurabilitySnapshot snap;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item const* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;
            uint32 maxDurability = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
            if (!maxDurability)
                continue;
            uint32 curDurability = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
            ++snap.pieces;
            snap.curSum += curDurability;
            snap.maxSum += maxDurability;
            if (curDurability < maxDurability)
                snap.anyDamaged = true;
            if (curDurability * 100 < maxDurability * _settings.repairItemPct)
                snap.anyBelowItemPct = true;
        }
        return snap;
    }

    class ArmorerCheck
    {
    public:
        ArmorerCheck(Player* bot, float range) : _bot(bot), _range(range) { }
        bool operator()(Creature* creature)
        {
            if (!creature->IsAlive() || !creature->IsArmorer())
                return false;
            if (!_bot->IsWithinDistInMap(creature, _range))
                return false;
            _range = _bot->GetDistance(creature);
            return true;
        }
    private:
        Player* _bot;
        float _range;
    };

    class AmmoVendorCheck
    {
    public:
        AmmoVendorCheck(Player* bot, float range, uint32 subClass)
            : _bot(bot), _range(range), _subClass(subClass) { }
        bool operator()(Creature* creature)
        {
            if (!creature->IsAlive() || !creature->IsVendor())
                return false;
            if (creature->IsHostileTo(_bot) || _bot->IsHostileTo(creature))
                return false;
            if (!_bot->IsWithinDistInMap(creature, _range))
                return false;
            if (!BotMaintenance::VendorSellsUsableAmmo(creature->GetEntry(), _bot, _subClass))
                return false;
            _range = _bot->GetDistance(creature);
            return true;
        }
    private:
        Player* _bot;
        float _range;
        uint32 _subClass;
    };

    Creature* FindArmorer(Player* bot, float range)
    {
        Creature* found = nullptr;
        ArmorerCheck check(bot, range);
        Acore::CreatureLastSearcher<ArmorerCheck> searcher(bot, found, check);
        Cell::VisitObjects(bot, searcher, range);
        return found;
    }

    Creature* FindAmmoVendor(Player* bot, float range, uint32 subClass)
    {
        Creature* found = nullptr;
        AmmoVendorCheck check(bot, range, subClass);
        Acore::CreatureLastSearcher<AmmoVendorCheck> searcher(bot, found, check);
        Cell::VisitObjects(bot, searcher, range);
        return found;
    }

    void ReportRepair(Player* tellPlayer, Player* bot, BotMaintenance::RepairOutcome const& result)
    {
        if (!result.piecesRepaired)
            return;
        std::string text;
        if (result.copperSpent)
            text = Acore::StringFormat("{} repaired gear ({}).", bot->GetName(), FormatCopperLocal(result.copperSpent));
        else
            text = Acore::StringFormat("{} repaired gear.", bot->GetName());
        if (result.piecesSkipped)
            text += " Could not afford the rest.";
        if (tellPlayer)
            SendAltBots(tellPlayer, text);
        LOG_INFO("module.coa-playerbots", "BotMaintenance: {}", text);
    }

    void ReportAmmo(Player* tellPlayer, Player* bot, BotMaintenance::AmmoOutcome const& result)
    {
        std::string text;
        if (result.bought)
        {
            char const* name = result.itemName && result.itemName[0] ? result.itemName : "ammo";
            if (result.copperSpent)
                text = Acore::StringFormat("{} bought {} {} ({}).", bot->GetName(), result.bought, name,
                    FormatCopperLocal(result.copperSpent));
            else
                text = Acore::StringFormat("{} bought {} {}.", bot->GetName(), result.bought, name);
        }
        else if (result.fail && !strcmp(result.fail, "gold"))
            text = Acore::StringFormat("{} could not afford ammo.", bot->GetName());
        else if (result.fail && !strcmp(result.fail, "bags"))
            text = Acore::StringFormat("{} has no bag space for ammo.", bot->GetName());
        else
            return;
        if (tellPlayer)
            SendAltBots(tellPlayer, text);
        LOG_INFO("module.coa-playerbots", "BotMaintenance: {}", text);
    }

    class AnyVendorCheck
    {
    public:
        AnyVendorCheck(Player* bot, float range) : _bot(bot), _range(range) { }
        bool operator()(Creature* creature)
        {
            if (!creature->IsAlive() || !creature->IsVendor())
                return false;
            if (!_bot->IsWithinDistInMap(creature, _range))
                return false;
            _range = _bot->GetDistance(creature);
            return true;
        }
    private:
        Player* _bot;
        float _range;
    };

    Creature* FindAnyVendor(Player* bot, float range)
    {
        Creature* found = nullptr;
        AnyVendorCheck check(bot, range);
        Acore::CreatureLastSearcher<AnyVendorCheck> searcher(bot, found, check);
        Cell::VisitObjects(bot, searcher, range);
        return found;
    }

    bool CanActOnBags(Player const* bot)
    {
        return bot && bot->IsAlive() && bot->IsInWorld()
            && !bot->IsInCombat()
            && !bot->IsBeingTeleported()
            && !bot->GetTradeData()
            && bot->GetLootGUID().IsEmpty();
    }

    bool IsGeneralBagProto(ItemTemplate const* proto)
    {
        return proto && proto->Class == ITEM_CLASS_CONTAINER
            && proto->SubClass == ITEM_SUBCLASS_CONTAINER
            && proto->ContainerSlots > 0;
    }

    uint32 NeededSkillForBag(ItemTemplate const* proto)
    {
        if (!proto || proto->Class != ITEM_CLASS_CONTAINER)
            return 0;
        switch (proto->SubClass)
        {
            case ITEM_SUBCLASS_HERB_CONTAINER: return SKILL_HERBALISM;
            case ITEM_SUBCLASS_ENCHANTING_CONTAINER: return SKILL_ENCHANTING;
            case ITEM_SUBCLASS_ENGINEERING_CONTAINER: return SKILL_ENGINEERING;
            case ITEM_SUBCLASS_GEM_CONTAINER: return SKILL_JEWELCRAFTING;
            case ITEM_SUBCLASS_MINING_CONTAINER: return SKILL_MINING;
            case ITEM_SUBCLASS_LEATHERWORKING_CONTAINER: return SKILL_LEATHERWORKING;
            case ITEM_SUBCLASS_INSCRIPTION_CONTAINER: return SKILL_INSCRIPTION;
            default: return 0;
        }
    }

    bool BotCanUseSpecialBag(Player const* bot, ItemTemplate const* proto)
    {
        if (!bot || !proto)
            return false;
        if (proto->Class == ITEM_CLASS_QUIVER)
            return bot->getClass() == CLASS_HUNTER;
        if (proto->Class != ITEM_CLASS_CONTAINER)
            return false;
        if (proto->SubClass == ITEM_SUBCLASS_SOUL_CONTAINER)
            return bot->getClass() == CLASS_WARLOCK;
        uint32 skill = NeededSkillForBag(proto);
        return skill && bot->HasSkill(skill);
    }

    uint32 OrderItemOf(Player const* bot)
    {
        if (!bot || !sBotMgr)
            return 0;
        if (BotMgr::GuildGatherOrder const* order = sBotMgr->GetGuildGatherOrder(bot->GetGUID()))
            return order->itemEntry;
        return 0;
    }

    uint32 CountFreeGeneralSlotsLocal(Player const* bot)
    {
        if (!bot)
            return 0;
        uint32 freeSlots = 0;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                ++freeSlots;
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Bag const* bag = bot->GetBagByPos(bagSlot);
            if (!bag)
                continue;
            ItemTemplate const* proto = bag->GetTemplate();
            if (proto && proto->Class == ITEM_CLASS_CONTAINER && proto->SubClass != ITEM_SUBCLASS_CONTAINER)
                continue;
            if (proto && proto->Class == ITEM_CLASS_QUIVER)
                continue;
            freeSlots += bag->GetFreeSlots();
        }
        return freeSlots;
    }

    uint32 EquippedGeneralBagSlots(Player const* bot, uint8* outWeakSlot, uint32* outWeakSize, uint32* outFilled)
    {
        uint32 weakestSize = 0;
        uint8 weakestSlot = NULL_SLOT;
        uint32 filled = 0;
        uint32 totalSlots = INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START;
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Bag const* bag = bot->GetBagByPos(bagSlot);
            if (!bag)
                continue;
            ItemTemplate const* proto = bag->GetTemplate();
            if (!IsGeneralBagProto(proto))
                continue;
            ++filled;
            totalSlots += bag->GetBagSize();
            if (weakestSlot == NULL_SLOT || bag->GetBagSize() < weakestSize)
            {
                weakestSlot = bagSlot;
                weakestSize = bag->GetBagSize();
            }
        }
        if (outWeakSlot)
            *outWeakSlot = weakestSlot;
        if (outWeakSize)
            *outWeakSize = weakestSize;
        if (outFilled)
            *outFilled = filled;
        return totalSlots;
    }

    uint8 FirstEmptyBagSlot(Player const* bot)
    {
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot))
                return bagSlot;
        return NULL_SLOT;
    }

    // Verified against CoA item_template: these ids exist as general bags with the listed sizes.
    uint32 EnsureBagEntryForLevel(uint8 level)
    {
        if (level >= 60)
            return 21841; // Netherweave Bag, 16
        if (level >= 50)
            return 14046; // Runecloth Bag, 14
        if (level >= 35)
            return 10050; // Mageweave Bag, 12
        if (level >= 20)
            return 4497;  // Heavy Brown Bag, 10
        if (level >= 10)
            return 4498;  // Brown Leather Satchel, 8
        return 4496;      // Small Brown Pouch, 6
    }

    uint32 const kBagFallbacks[] = { 21841, 4500, 14046, 10050, 4497, 4498, 4496, 0 };

    ItemTemplate const* ValidGeneralBag(uint32 entry)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        return IsGeneralBagProto(proto) ? proto : nullptr;
    }

    ItemTemplate const* ResolveEnsureBag(uint8 level)
    {
        if (ItemTemplate const* proto = ValidGeneralBag(EnsureBagEntryForLevel(level)))
            return proto;
        for (uint32 entry : kBagFallbacks)
        {
            if (!entry)
                break;
            if (ItemTemplate const* proto = ValidGeneralBag(entry))
                return proto;
        }
        return nullptr;
    }

    bool StaleQuestItem(Player* bot, ItemTemplate const* proto)
    {
        if (!proto)
            return false;
        if (bot->HasQuestForItem(proto->ItemId))
            return false;
        if (proto->StartQuest)
        {
            QuestStatus status = bot->GetQuestStatus(proto->StartQuest);
            if (status == QUEST_STATUS_NONE || status == QUEST_STATUS_FAILED)
                return false;
            return true;
        }
        if (proto->Class == ITEM_CLASS_QUEST || proto->Bonding == BIND_QUEST_ITEM || proto->Bonding == BIND_QUEST_ITEM1)
            return true;
        return false;
    }


    // CoA custom gathering profession / tool. The axe itself is granted by a
    // world update rather than the base item_template dump.
    constexpr uint32 SKILL_WOODCUTTING_COA = 732;
    constexpr uint32 SKILL_WOODWORKING_COA = 757;
    constexpr uint32 LUMBER_AXE_ENTRY = 6954;

    constexpr uint32 TOOL_FAMILY_MINING      = 0x001;
    constexpr uint32 TOOL_FAMILY_SKINNING    = 0x002;
    constexpr uint32 TOOL_FAMILY_BLACKSMITH  = 0x004;
    constexpr uint32 TOOL_FAMILY_ENGINEERING = 0x008;
    constexpr uint32 TOOL_FAMILY_ENCHANTING  = 0x010;
    constexpr uint32 TOOL_FAMILY_INSCRIPTION = 0x020;
    constexpr uint32 TOOL_FAMILY_FISHING     = 0x040;
    constexpr uint32 TOOL_FAMILY_WOODCUTTING = 0x080;
    constexpr uint32 TOOL_FAMILY_ALCHEMY     = 0x100;
    constexpr uint32 TOOL_FAMILY_COOKING     = 0x200;
    constexpr uint32 TOOL_FAMILY_HERBALISM   = 0x400;
    constexpr uint32 TOOL_FAMILY_CLASS       = 0x800;

    uint32 OrderGatherSkill(Player const* bot)
    {
        if (!bot || !sBotMgr)
            return 0;
        BotMgr::GuildGatherOrder const* order = sBotMgr->GetGuildGatherOrder(bot->GetGUID());
        if (!order || !order->itemEntry)
            return 0;
        BotMgr::GatherSourceInfo const& info = sBotMgr->GetGatherSourceInfo(order->itemEntry);
        if (info.nodeSourced && info.nodeSkillId)
            return info.nodeSkillId;
        if (info.fishSourced)
            return SKILL_FISHING;
        if (!info.huntSkinEntries.empty())
            return SKILL_SKINNING;
        return 0;
    }

    uint32 ToolFamiliesOf(ItemTemplate const* proto)
    {
        if (!proto)
            return 0;
        uint32 families = 0;
        uint32 const cat = proto->TotemCategory;
        switch (cat)
        {
            case TC_EARTH_TOTEM:
            case TC_AIR_TOTEM:
            case TC_FIRE_TOTEM:
            case TC_WATER_TOTEM:
            case TC_MASTER_TOTEM:
                return TOOL_FAMILY_CLASS;
            case TC_MINING_PICK:
            case TC_MINING_PICK_OLD:
                families |= TOOL_FAMILY_MINING;
                break;
            case TC_SKINNING_KNIFE:
            case TC_SKINNING_SKIFE_OLD:
                families |= TOOL_FAMILY_SKINNING;
                break;
            case TC_BLACKSMITH_HAMMER:
            case TC_BLACKSMITH_HAMMER_OLD:
                families |= TOOL_FAMILY_BLACKSMITH;
                break;
            case TC_ARCLIGHT_SPANNER:
            case TC_GYROMATIC_MA:
                families |= TOOL_FAMILY_ENGINEERING;
                break;
            case TC_GNOMISH_ARMY_KNIFE:
                families |= TOOL_FAMILY_MINING | TOOL_FAMILY_SKINNING
                    | TOOL_FAMILY_BLACKSMITH | TOOL_FAMILY_ENGINEERING;
                break;
            case TC_HAMMER_PICK:
                families |= TOOL_FAMILY_MINING | TOOL_FAMILY_BLACKSMITH;
                break;
            case TC_BLADED_PICKAXE:
                families |= TOOL_FAMILY_MINING | TOOL_FAMILY_SKINNING;
                break;
            case TC_COPPER_ROD:
            case TC_SILVER_ROD:
            case TC_GOLDEN_ROD:
            case TC_TRUESILVER_ROD:
            case TC_ARCANITE_ROD:
            case TC_FEL_IRON_ROD:
            case TC_ADAMANTITE_ROD:
            case TC_ETERNIUM_ROD:
            case TC_RUNED_AZURITE_ROD:
            case TC_RUNED_COBALT_ROD:
            case TC_RUNED_TITANIUM_ROD:
                families |= TOOL_FAMILY_ENCHANTING;
                break;
            case TC_HOLLOW_QUILL:
            case TC_VIRTUOSO_INKING_SET:
                families |= TOOL_FAMILY_INSCRIPTION;
                break;
            case TC_PHILOSOPHERS_STONE:
                families |= TOOL_FAMILY_ALCHEMY;
                break;
            case TC_FLINT_AND_TINDER:
                families |= TOOL_FAMILY_COOKING;
                break;
            default:
                break;
        }

        if (proto->Class == ITEM_CLASS_WEAPON && proto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE)
            families |= TOOL_FAMILY_FISHING;

        if (proto->ItemId == LUMBER_AXE_ENTRY || proto->RequiredSkill == SKILL_WOODCUTTING_COA
            || proto->RequiredSkill == SKILL_WOODWORKING_COA)
            families |= TOOL_FAMILY_WOODCUTTING;

        if (proto->RequiredSkill == SKILL_HERBALISM
            && (cat || (proto->Class == ITEM_CLASS_WEAPON && proto->SubClass == ITEM_SUBCLASS_WEAPON_MISC)))
            families |= TOOL_FAMILY_HERBALISM;

        if (!families && BotAI::IsProfessionTool(proto))
            families = TOOL_FAMILY_CLASS;
        return families;
    }

    uint32 NeededToolFamilies(Player const* bot)
    {
        if (!bot)
            return 0;
        uint32 needed = 0;
        if (bot->HasSkill(SKILL_MINING))
            needed |= TOOL_FAMILY_MINING;
        if (bot->HasSkill(SKILL_SKINNING))
            needed |= TOOL_FAMILY_SKINNING;
        if (bot->HasSkill(SKILL_BLACKSMITHING))
            needed |= TOOL_FAMILY_BLACKSMITH;
        if (bot->HasSkill(SKILL_ENGINEERING))
            needed |= TOOL_FAMILY_ENGINEERING;
        if (bot->HasSkill(SKILL_ENCHANTING))
            needed |= TOOL_FAMILY_ENCHANTING;
        if (bot->HasSkill(SKILL_INSCRIPTION))
            needed |= TOOL_FAMILY_INSCRIPTION;
        if (bot->HasSkill(SKILL_FISHING))
            needed |= TOOL_FAMILY_FISHING;
        if (bot->HasSkill(SKILL_WOODCUTTING_COA) || bot->HasSkill(SKILL_WOODWORKING_COA))
            needed |= TOOL_FAMILY_WOODCUTTING;
        if (bot->HasSkill(SKILL_ALCHEMY))
            needed |= TOOL_FAMILY_ALCHEMY;
        if (bot->HasSkill(SKILL_COOKING))
            needed |= TOOL_FAMILY_COOKING;
        if (bot->HasSkill(SKILL_HERBALISM))
            needed |= TOOL_FAMILY_HERBALISM;

        uint32 const orderSkill = OrderGatherSkill(bot);
        if (orderSkill == SKILL_MINING)
            needed |= TOOL_FAMILY_MINING;
        else if (orderSkill == SKILL_SKINNING)
            needed |= TOOL_FAMILY_SKINNING;
        else if (orderSkill == SKILL_FISHING)
            needed |= TOOL_FAMILY_FISHING;
        else if (orderSkill == SKILL_HERBALISM)
            needed |= TOOL_FAMILY_HERBALISM;
        else if (orderSkill == SKILL_WOODCUTTING_COA || orderSkill == SKILL_WOODWORKING_COA)
            needed |= TOOL_FAMILY_WOODCUTTING;
        return needed;
    }

    uint32 ToolScore(Item const* item)
    {
        if (!item || !item->GetTemplate())
            return 0;
        ItemTemplate const* proto = item->GetTemplate();
        uint32 score = proto->Quality * 10000u + proto->ItemLevel * 100u + proto->RequiredSkillRank;
        if (proto->TotemCategory == TC_GNOMISH_ARMY_KNIFE)
            score += 50000u;
        switch (proto->TotemCategory)
        {
            case TC_COPPER_ROD: score += 1000u; break;
            case TC_SILVER_ROD: score += 2000u; break;
            case TC_GOLDEN_ROD: score += 3000u; break;
            case TC_TRUESILVER_ROD: score += 4000u; break;
            case TC_ARCANITE_ROD: score += 5000u; break;
            case TC_FEL_IRON_ROD: score += 6000u; break;
            case TC_ADAMANTITE_ROD: score += 7000u; break;
            case TC_ETERNIUM_ROD: score += 8000u; break;
            case TC_RUNED_AZURITE_ROD: score += 8500u; break;
            case TC_RUNED_COBALT_ROD: score += 9000u; break;
            case TC_RUNED_TITANIUM_ROD: score += 10000u; break;
            default: break;
        }
        // An already-equipped fishing pole or weapon-tool always wins its family.
        if (item->IsEquipped())
            score += 1000000u;
        return score;
    }

    struct ToolPlan
    {
        std::unordered_set<ObjectGuid> keep;
        std::unordered_set<ObjectGuid> sell;
    };

    ToolPlan ClassifyTools(Player* bot)
    {
        ToolPlan plan;
        if (!bot)
            return plan;
        uint32 const needed = NeededToolFamilies(bot);
        struct ToolItem
        {
            Item* item;
            uint32 families;
            uint32 score;
        };
        std::vector<ToolItem> tools;
        auto considerTool = [&](Item* item)
        {
            if (!item || !item->GetTemplate())
                return;
            uint32 families = ToolFamiliesOf(item->GetTemplate());
            if (!families)
                return;
            tools.push_back({ item, families, ToolScore(item) });
        };
        ForEachInventoryItem(bot, considerTool);
        // Include equipped tools so the bag copy of a worse pick or pole is sold.
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            considerTool(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));

        std::unordered_set<ObjectGuid> winners;
        static constexpr uint32 kFamilies[] = {
            TOOL_FAMILY_MINING, TOOL_FAMILY_SKINNING, TOOL_FAMILY_BLACKSMITH,
            TOOL_FAMILY_ENGINEERING, TOOL_FAMILY_ENCHANTING, TOOL_FAMILY_INSCRIPTION,
            TOOL_FAMILY_FISHING, TOOL_FAMILY_WOODCUTTING, TOOL_FAMILY_ALCHEMY,
            TOOL_FAMILY_COOKING, TOOL_FAMILY_HERBALISM
        };
        for (uint32 family : kFamilies)
        {
            if (!(needed & family))
                continue;
            ToolItem const* best = nullptr;
            for (ToolItem const& tool : tools)
            {
                if (!(tool.families & family))
                    continue;
                if (!best || tool.score > best->score)
                    best = &tool;
            }
            if (best)
                winners.insert(best->item->GetGUID());
        }

        for (ToolItem const& tool : tools)
        {
            ObjectGuid const guid = tool.item->GetGUID();
            // Class totems and similar stay. An equipped fishing pole is never sold.
            bool const classTool = (tool.families & TOOL_FAMILY_CLASS) != 0;
            bool const equippedPole = tool.item->IsEquipped()
                && tool.item->GetTemplate()->Class == ITEM_CLASS_WEAPON
                && tool.item->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE;
            if (classTool || equippedPole || winners.count(guid))
                plan.keep.insert(guid);
            else
                plan.sell.insert(guid);
        }
        return plan;
    }

    uint32 WeaponSkillFor(ItemTemplate const* proto)
    {
        if (!proto || proto->Class != ITEM_CLASS_WEAPON)
            return 0;
        switch (proto->SubClass)
        {
            case ITEM_SUBCLASS_WEAPON_AXE:          return SKILL_AXES;
            case ITEM_SUBCLASS_WEAPON_AXE2:         return SKILL_2H_AXES;
            case ITEM_SUBCLASS_WEAPON_BOW:          return SKILL_BOWS;
            case ITEM_SUBCLASS_WEAPON_GUN:          return SKILL_GUNS;
            case ITEM_SUBCLASS_WEAPON_MACE:         return SKILL_MACES;
            case ITEM_SUBCLASS_WEAPON_MACE2:        return SKILL_2H_MACES;
            case ITEM_SUBCLASS_WEAPON_POLEARM:      return SKILL_POLEARMS;
            case ITEM_SUBCLASS_WEAPON_SWORD:        return SKILL_SWORDS;
            case ITEM_SUBCLASS_WEAPON_SWORD2:       return SKILL_2H_SWORDS;
            case ITEM_SUBCLASS_WEAPON_STAFF:        return SKILL_STAVES;
            case ITEM_SUBCLASS_WEAPON_FIST:         return SKILL_FIST_WEAPONS;
            case ITEM_SUBCLASS_WEAPON_DAGGER:       return SKILL_DAGGERS;
            case ITEM_SUBCLASS_WEAPON_THROWN:       return SKILL_THROWN;
            case ITEM_SUBCLASS_WEAPON_SPEAR:        return SKILL_POLEARMS;
            case ITEM_SUBCLASS_WEAPON_CROSSBOW:     return SKILL_CROSSBOWS;
            case ITEM_SUBCLASS_WEAPON_WAND:         return SKILL_WANDS;
            default:                                return 0;
        }
    }

    bool ClassCanWear(Player const* bot, ItemTemplate const* proto)
    {
        if (!bot || !proto)
            return false;
        if (proto->AllowableClass && proto->AllowableClass != uint32(-1)
            && !(proto->AllowableClass & bot->getClassMask()))
            return false;
        if (proto->AllowableRace && proto->AllowableRace != uint32(-1)
            && !(proto->AllowableRace & bot->getRaceMask()))
            return false;

        if (proto->Class == ITEM_CLASS_WEAPON)
        {
            if (proto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE
                || proto->SubClass == ITEM_SUBCLASS_WEAPON_MISC)
                return true;
            uint32 skill = WeaponSkillFor(proto);
            if (skill && !bot->HasSkill(skill))
                return false;
            return true;
        }

        if (proto->Class != ITEM_CLASS_ARMOR)
            return true;

        switch (proto->InventoryType)
        {
            case INVTYPE_CLOAK:
            case INVTYPE_NECK:
            case INVTYPE_FINGER:
            case INVTYPE_TRINKET:
            case INVTYPE_BODY:
            case INVTYPE_TABARD:
            case INVTYPE_HOLDABLE:
                return true;
            default:
                break;
        }

        switch (proto->SubClass)
        {
            case ITEM_SUBCLASS_ARMOR_SHIELD:
            case ITEM_SUBCLASS_ARMOR_BUCKLER:
                return bot->HasSkill(SKILL_SHIELD);
            case ITEM_SUBCLASS_ARMOR_LIBRAM:
                return bot->getClass() == CLASS_PALADIN;
            case ITEM_SUBCLASS_ARMOR_IDOL:
                return bot->getClass() == CLASS_DRUID;
            case ITEM_SUBCLASS_ARMOR_TOTEM:
                return bot->getClass() == CLASS_SHAMAN;
            case ITEM_SUBCLASS_ARMOR_SIGIL:
                return bot->getClass() == CLASS_DEATH_KNIGHT;
            case ITEM_SUBCLASS_ARMOR_MISC:
            case ITEM_SUBCLASS_ARMOR_CLOTH:
                return true;
            case ITEM_SUBCLASS_ARMOR_LEATHER:
                return bot->HasSkill(SKILL_LEATHER);
            case ITEM_SUBCLASS_ARMOR_MAIL:
                return bot->HasSkill(SKILL_MAIL);
            case ITEM_SUBCLASS_ARMOR_PLATE:
                return bot->HasSkill(SKILL_PLATE_MAIL);
            default:
                return true;
        }
    }

    bool IsGearProto(ItemTemplate const* proto)
    {
        return proto && (proto->Class == ITEM_CLASS_ARMOR || proto->Class == ITEM_CLASS_WEAPON)
            && !ToolFamiliesOf(proto);
    }

    void EquipBetterGear(Player* bot)
    {
        if (!bot)
            return;
        BotRole role = BotAI::GetRole(bot->GetGUID());
        auto consider = [&](Item* item) -> bool
        {
            if (!item || !item->GetTemplate())
                return false;
            ItemTemplate const* proto = item->GetTemplate();
            if (!IsGearProto(proto))
                return false;
            if (BotAI::IsProfessionTool(proto))
                return false;
            if (!ClassCanWear(bot, proto))
                return false;
            if (proto->RequiredLevel && bot->GetLevel() < proto->RequiredLevel)
                return false;
            uint8 eslot = bot->FindEquipSlot(proto, NULL_SLOT, true);
            if (eslot == NULL_SLOT)
                return false;
            Item* current = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, eslot);
            float newScore = BotAI::ScoreItemForBot(bot, proto, role);
            if (current)
            {
                float currentScore = BotAI::ScoreItemForBot(bot, current->GetTemplate(), role);
                if (newScore <= currentScore * 1.05f)
                    return false;
            }
            uint16 dest = uint16(eslot) | (uint16(INVENTORY_SLOT_BAG_0) << 8);
            if (bot->CanEquipItem(NULL_SLOT, dest, item, true) != EQUIP_ERR_OK)
                return false;
            bot->SwapItem(item->GetPos(), dest);
            return true;
        };

        for (uint32 n = 0; n < 8; ++n)
        {
            bool swapped = false;
            ForEachInventoryItem(bot, [&](Item* item)
            {
                if (!swapped && consider(item))
                    swapped = true;
            });
            if (!swapped)
                break;
        }
    }

    struct GearPlan
    {
        std::unordered_set<ObjectGuid> sell;
    };

    GearPlan ClassifyGear(Player* bot)
    {
        GearPlan plan;
        if (!bot)
            return plan;
        BotRole role = BotAI::GetRole(bot->GetGUID());
        uint8 const level = bot->GetLevel();
        struct Candidate
        {
            Item* item;
            uint8 eslot;
            float score;
            uint32 requiredLevel;
            bool canWear;
            bool tooHigh;
            bool upcoming;
        };
        std::vector<Candidate> items;
        ForEachInventoryItem(bot, [&](Item* item)
        {
            if (!item || !item->GetTemplate())
                return;
            ItemTemplate const* proto = item->GetTemplate();
            if (!IsGearProto(proto))
                return;
            if (proto->Quality >= ITEM_QUALITY_EPIC)
                return;
            bool canWear = ClassCanWear(bot, proto);
            uint32 req = proto->RequiredLevel;
            bool tooHigh = req && req > uint32(level) + 5;
            bool upcoming = canWear && req > level && req <= uint32(level) + 5;
            uint8 eslot = bot->FindEquipSlot(proto, NULL_SLOT, true);
            float score = BotAI::ScoreItemForBot(bot, proto, role);
            items.push_back({ item, eslot, score, req, canWear, tooHigh, upcoming });
        });

        // Keep at most one upcoming spare per slot, the highest-scoring one.
        std::unordered_map<uint8, ObjectGuid> bestSpare;
        std::unordered_map<uint8, float> bestSpareScore;
        for (Candidate const& c : items)
        {
            if (!c.upcoming || c.eslot == NULL_SLOT)
                continue;
            Item* current = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, c.eslot);
            float currentScore = current ? BotAI::ScoreItemForBot(bot, current->GetTemplate(), role) : 0.0f;
            if (current && c.score <= currentScore * 1.05f)
                continue;
            auto it = bestSpareScore.find(c.eslot);
            if (it == bestSpareScore.end() || c.score > it->second)
            {
                bestSpare[c.eslot] = c.item->GetGUID();
                bestSpareScore[c.eslot] = c.score;
            }
        }

        for (Candidate const& c : items)
        {
            ItemTemplate const* proto = c.item->GetTemplate();
            if (!c.canWear || c.tooHigh)
            {
                plan.sell.insert(c.item->GetGUID());
                continue;
            }
            if (c.upcoming)
            {
                auto it = bestSpare.find(c.eslot);
                if (it == bestSpare.end() || it->second != c.item->GetGUID())
                    plan.sell.insert(c.item->GetGUID());
                continue;
            }

            Item* current = (c.eslot != NULL_SLOT) ? bot->GetItemByPos(INVENTORY_SLOT_BAG_0, c.eslot) : nullptr;
            float currentScore = current ? BotAI::ScoreItemForBot(bot, current->GetTemplate(), role) : 0.0f;
            bool upgrade = current ? (c.score > currentScore * 1.05f) : (c.eslot != NULL_SLOT);
            bool uselessBlue = proto->Quality >= ITEM_QUALITY_RARE
                && current && c.score < currentScore * 0.5f;
            bool whiteOrGreen = proto->Quality <= ITEM_QUALITY_UNCOMMON;

            if (whiteOrGreen && !upgrade)
                plan.sell.insert(c.item->GetGUID());
            else if (uselessBlue)
                plan.sell.insert(c.item->GetGUID());
        }
        return plan;
    }

    bool IsOutgrownGearItem(Player* bot, Item const* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON))
            return false;
        uint8 eslot = bot->FindEquipSlot(proto, NULL_SLOT, true);
        if (eslot == NULL_SLOT)
            return false;
        Item* current = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, eslot);
        if (!current)
            return false;
        BotRole role = BotAI::GetRole(bot->GetGUID());
        if (BotAI::ScoreItemForBot(bot, proto, role) > BotAI::ScoreItemForBot(bot, current->GetTemplate(), role) * 1.05f)
            return false;
        uint32 const keepQuality = sBotMgr->InventoryKeepQuality();
        if (proto->Quality >= keepQuality && CountFreeGeneralSlotsLocal(bot) > _settings.freeSlotTarget)
            return false;
        return true;
    }

    bool IsProtectedBagItem(Player* bot, Item const* item)
    {
        if (!item || item->IsNotEmptyBag() || item->IsRefundable() || bot->GetLootGUID() == item->GetGUID())
            return true;
        if (BotGear::Archived(item))
            return true;
        ItemTemplate const* proto = item->GetTemplate();
        if (!proto)
            return true;
        if (proto->ItemId == HEARTHSTONE_ENTRY)
            return true;
        if (BotEconomy::IsProtectedItem(proto->ItemId))
            return true;
        if (proto->ItemId == OrderItemOf(bot))
            return true;
        // Profession tools are classified separately so a bot keeps one best
        // tool per profession and can sell duplicates, worse ones, and tools
        // for professions it does not have.
        if (proto->Class == ITEM_CLASS_KEY || proto->Class == ITEM_CLASS_PROJECTILE)
            return true;
        if (proto->Class == ITEM_CLASS_REAGENT)
            return true;
        return false;
    }

    enum class BagItemFate : uint8
    {
        Keep,
        Sell,
        Destroy,
    };

    uint32 ConsumableKeepLimit(ItemTemplate const* proto)
    {
        if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE)
            return 0;
        switch (proto->SubClass)
        {
            case ITEM_SUBCLASS_POTION:
            case ITEM_SUBCLASS_ELIXIR:
            case ITEM_SUBCLASS_FLASK:
            case ITEM_SUBCLASS_BANDAGE:
                return 20;
            case ITEM_SUBCLASS_FOOD:
                return 40;
            default:
                return 10;
        }
    }

    BagItemFate FateOfItem(Player* bot, Item const* item, uint32 heldTrade, uint32 heldConsumable, bool bagsTight)
    {
        if (IsProtectedBagItem(bot, item))
            return BagItemFate::Keep;
        ItemTemplate const* proto = item->GetTemplate();

        if (IsGeneralBagProto(proto))
        {
            // Equipped bags are never visited here (inventory walk only). Keep a bag that
            // is an upgrade for an empty or smaller slot; sell a leftover smaller one
            // once bags are tight.
            uint8 weakSlot = NULL_SLOT;
            uint32 weakSize = 0;
            EquippedGeneralBagSlots(bot, &weakSlot, &weakSize, nullptr);
            if (FirstEmptyBagSlot(bot) != NULL_SLOT)
                return BagItemFate::Keep;
            if (weakSlot != NULL_SLOT && proto->ContainerSlots > weakSize)
                return BagItemFate::Keep;
            return bagsTight ? BagItemFate::Sell : BagItemFate::Keep;
        }

        if (proto->Class == ITEM_CLASS_CONTAINER || proto->Class == ITEM_CLASS_QUIVER)
        {
            if (BotCanUseSpecialBag(bot, proto))
                return BagItemFate::Keep;
            return bagsTight ? BagItemFate::Sell : BagItemFate::Keep;
        }

        if (StaleQuestItem(bot, proto))
            return proto->SellPrice > 0 ? BagItemFate::Sell : BagItemFate::Destroy;

        if (proto->Class == ITEM_CLASS_QUEST || proto->StartQuest || proto->Bonding == BIND_QUEST_ITEM
            || proto->Bonding == BIND_QUEST_ITEM1 || bot->HasQuestForItem(proto->ItemId))
            return BagItemFate::Keep;

        if (proto->Quality == ITEM_QUALITY_POOR)
            return proto->SellPrice > 0 ? BagItemFate::Sell : BagItemFate::Destroy;

        if (proto->Class == ITEM_CLASS_TRADE_GOODS)
        {
            if (!bagsTight)
                return BagItemFate::Keep;
            if (heldTrade > _settings.tradeGoodsKeep)
                return proto->SellPrice > 0 ? BagItemFate::Sell : BagItemFate::Keep;
            return BagItemFate::Keep;
        }

        if (proto->Class == ITEM_CLASS_CONSUMABLE)
        {
            if (proto->RequiredLevel && bot->GetLevel() < proto->RequiredLevel)
                return bagsTight && proto->SellPrice > 0 ? BagItemFate::Sell : BagItemFate::Keep;
            uint32 limit = ConsumableKeepLimit(proto);
            if (bagsTight && heldConsumable > limit)
                return proto->SellPrice > 0 ? BagItemFate::Sell : BagItemFate::Keep;
            // Outleveled food: required level is 10+ below the bot.
            if (bagsTight && proto->SubClass == ITEM_SUBCLASS_FOOD && proto->RequiredLevel + 10 < bot->GetLevel()
                && proto->SellPrice > 0)
                return BagItemFate::Sell;
            return BagItemFate::Keep;
        }

        if (proto->Class == ITEM_CLASS_RECIPE)
        {
            if (proto->RequiredSkill && !bot->HasSkill(proto->RequiredSkill))
                return bagsTight && proto->SellPrice > 0 ? BagItemFate::Sell : BagItemFate::Keep;
            bool known = false;
            for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
                if (proto->Spells[i].SpellId && bot->HasSpell(proto->Spells[i].SpellId))
                    known = true;
            if (known)
                return bagsTight && proto->SellPrice > 0 ? BagItemFate::Sell : BagItemFate::Keep;
            return BagItemFate::Keep;
        }

        if (IsOutgrownGearItem(bot, item))
            return proto->SellPrice > 0 ? BagItemFate::Sell : (bagsTight ? BagItemFate::Destroy : BagItemFate::Keep);

        // White vendor trash that is not useful gear, trade goods, or a consumable.
        if (bagsTight && proto->Quality <= ITEM_QUALITY_NORMAL && proto->SellPrice > 0
            && proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON
            && proto->Class != ITEM_CLASS_GEM)
            return BagItemFate::Sell;

        return BagItemFate::Keep;
    }

    // When the economy is live, bag cleanup may only vendor items that PlanSale
    // marks Vendor. Auction and implicit Keep (including protected entries and
    // profession tools the economy never plans) stay in the bags for the economy trip.
    bool TryLiveVerdicts(Player* bot, std::unordered_map<uint32, BotEconomy::Verdict>& out)
    {
        out.clear();
        if (!bot || !BotEconomy::IsLive())
            return false;
        for (BotEconomy::Line const& line : BotEconomy::PlanSale(bot))
            out[line.entry] = line.verdict;
        return true;
    }

    bool EconomyHoldsItem(uint32 entry, std::unordered_map<uint32, BotEconomy::Verdict> const* live)
    {
        if (!live)
            return false;
        if (BotEconomy::IsProtectedItem(entry))
            return true;
        auto it = live->find(entry);
        if (it == live->end())
            return true;
        return it->second != BotEconomy::Verdict::Vendor;
    }

    uint32 CountSellableBagItems(Player* bot, ToolPlan const& tools, GearPlan const& gear)
    {
        if (!bot)
            return 0;
        std::unordered_map<uint32, BotEconomy::Verdict> liveVerdicts;
        bool const live = TryLiveVerdicts(bot, liveVerdicts);
        std::unordered_map<uint32, uint32> tradeCount;
        std::unordered_map<uint32, uint32> consCount;
        ForEachInventoryItem(bot, [&](Item* item)
        {
            if (!item || !item->GetTemplate())
                return;
            if (item->GetTemplate()->Class == ITEM_CLASS_TRADE_GOODS)
                tradeCount[item->GetEntry()] += item->GetCount();
            if (item->GetTemplate()->Class == ITEM_CLASS_CONSUMABLE)
                consCount[item->GetEntry()] += item->GetCount();
        });
        uint32 count = 0;
        ForEachInventoryItem(bot, [&](Item* item)
        {
            if (!item)
                return;
            ObjectGuid const guid = item->GetGUID();
            if (tools.keep.count(guid))
                return;
            ItemTemplate const* proto = item->GetTemplate();
            if (EconomyHoldsItem(proto ? proto->ItemId : 0, live ? &liveVerdicts : nullptr))
                return;
            if (tools.sell.count(guid) || gear.sell.count(guid))
            {
                ++count;
                return;
            }
            uint32 traded = proto && proto->Class == ITEM_CLASS_TRADE_GOODS ? tradeCount[item->GetEntry()] : 0;
            uint32 consumed = proto && proto->Class == ITEM_CLASS_CONSUMABLE ? consCount[item->GetEntry()] : 0;
            if (FateOfItem(bot, item, traded, consumed, true) == BagItemFate::Sell)
                ++count;
        });
        return count;
    }

    void CollectInventoryItems(Player* bot, std::vector<Item*>& out)
    {
        out.clear();
        ForEachInventoryItem(bot, [&](Item* item)
        {
            if (item)
                out.push_back(item);
        });
    }

    uint32 MergeStacks(Player* bot)
    {
        std::vector<Item*> items;
        CollectInventoryItems(bot, items);
        uint32 merged = 0;
        for (size_t i = 0; i < items.size(); ++i)
        {
            Item* dst = items[i];
            if (!dst)
                continue;
            uint32 cap = dst->GetMaxStackCount();
            if (cap <= 1 || dst->GetCount() >= cap)
                continue;
            for (size_t j = i + 1; j < items.size(); ++j)
            {
                Item* src = items[j];
                if (!src || src->GetEntry() != dst->GetEntry() || src->GetCount() == 0)
                    continue;
                if (src->GetMaxStackCount() <= 1)
                    continue;
                uint32 room = cap - dst->GetCount();
                if (!room)
                    break;
                uint32 moved = src->GetCount() < room ? src->GetCount() : room;
                dst->SetCount(dst->GetCount() + moved);
                dst->SetState(ITEM_CHANGED, bot);
                ++merged;
                if (moved == src->GetCount())
                {
                    items[j] = nullptr;
                    bot->DestroyItem(src->GetBagSlot(), src->GetSlot(), true);
                }
                else
                {
                    src->SetCount(src->GetCount() - moved);
                    src->SetState(ITEM_CHANGED, bot);
                }
            }
        }
        return merged;
    }

    bool EquipBagInto(Player* bot, Item* bagItem, uint8 destSlot)
    {
        if (!bot || !bagItem)
            return false;
        uint16 dest = uint16(destSlot) | (uint16(INVENTORY_SLOT_BAG_0) << 8);
        bool swap = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, destSlot) != nullptr;
        if (bot->CanEquipItem(destSlot, dest, bagItem, swap) != EQUIP_ERR_OK)
            return false;
        bot->SwapItem(bagItem->GetPos(), dest);
        return bot->GetItemByPos(INVENTORY_SLOT_BAG_0, destSlot) == bagItem
            || (bot->GetBagByPos(destSlot) && bot->GetBagByPos(destSlot)->GetEntry() == bagItem->GetEntry());
    }

    // After SwapItem the Item* may have moved; check the dest slot's bag size.
    uint32 EquipUpgradeBags(Player* bot, uint32& outUpgradeSlots)
    {
        uint32 equipped = 0;
        outUpgradeSlots = 0;
        std::vector<Item*> items;
        CollectInventoryItems(bot, items);
        std::sort(items.begin(), items.end(), [](Item* a, Item* b)
        {
            uint32 sa = a && a->GetTemplate() ? a->GetTemplate()->ContainerSlots : 0;
            uint32 sb = b && b->GetTemplate() ? b->GetTemplate()->ContainerSlots : 0;
            return sa > sb;
        });

        for (Item* item : items)
        {
            if (!item)
                continue;
            ItemTemplate const* proto = item->GetTemplate();
            if (!IsGeneralBagProto(proto))
                continue;
            if (proto->RequiredLevel && bot->GetLevel() < proto->RequiredLevel)
                continue;
            if (bot->CanUseItem(proto) != EQUIP_ERR_OK)
                continue;

            uint8 empty = FirstEmptyBagSlot(bot);
            if (empty != NULL_SLOT)
            {
                uint32 slots = proto->ContainerSlots;
                if (EquipBagInto(bot, item, empty))
                {
                    ++equipped;
                    if (slots > outUpgradeSlots)
                        outUpgradeSlots = slots;
                }
                continue;
            }

            uint8 weakSlot = NULL_SLOT;
            uint32 weakSize = 0;
            EquippedGeneralBagSlots(bot, &weakSlot, &weakSize, nullptr);
            if (weakSlot == NULL_SLOT || proto->ContainerSlots <= weakSize)
                continue;
            uint32 slots = proto->ContainerSlots;
            if (EquipBagInto(bot, item, weakSlot))
            {
                ++equipped;
                if (slots > outUpgradeSlots)
                    outUpgradeSlots = slots;
            }
        }
        return equipped;
    }

    Item* StoreOneItem(Player* bot, uint32 entry)
    {
        ItemPosCountVec dest;
        if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, 1) != EQUIP_ERR_OK)
            return nullptr;
        return bot->StoreNewItem(dest, entry, true);
    }

    uint32 GrantEnsureBags(Player* bot, uint32& outUpgradeSlots)
    {
        ItemTemplate const* proto = ResolveEnsureBag(bot->GetLevel());
        if (!proto)
            return 0;
        uint32 granted = 0;
        for (int step = 0; step < 4; ++step)
        {
            uint8 empty = FirstEmptyBagSlot(bot);
            uint8 weakSlot = NULL_SLOT;
            uint32 weakSize = 0;
            EquippedGeneralBagSlots(bot, &weakSlot, &weakSize, nullptr);
            uint8 destSlot = empty;
            if (destSlot == NULL_SLOT)
            {
                if (weakSlot == NULL_SLOT || proto->ContainerSlots <= weakSize)
                    break;
                destSlot = weakSlot;
            }
            if (CountFreeGeneralSlotsLocal(bot) == 0 && empty == NULL_SLOT)
                break;
            Item* bagItem = StoreOneItem(bot, proto->ItemId);
            if (!bagItem)
                break;
            if (EquipBagInto(bot, bagItem, destSlot))
            {
                ++granted;
                if (proto->ContainerSlots > outUpgradeSlots)
                    outUpgradeSlots = proto->ContainerSlots;
            }
            else
                break;
        }
        return granted;
    }

    ItemTemplate const* BestVendorBag(uint32 vendorEntry, Player const* bot, uint32 biggerThan)
    {
        VendorItemData const* items = VendorItemsOf(vendorEntry);
        if (!items || items->Empty())
            return nullptr;
        ItemTemplate const* best = nullptr;
        for (uint8 slot = 0; slot < items->GetItemCount(); ++slot)
        {
            VendorItem const* vItem = items->GetItem(slot);
            if (!vItem || vItem->maxcount || vItem->ExtendedCost)
                continue;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(vItem->item);
            if (!proto || !IsGeneralBagProto(proto) || !vItem->IsGoldRequired(proto) || !proto->BuyPrice)
                continue;
            if (proto->RequiredLevel && bot->GetLevel() < proto->RequiredLevel)
                continue;
            if (bot->CanUseItem(proto) != EQUIP_ERR_OK)
                continue;
            if (proto->ContainerSlots <= biggerThan)
                continue;
            if (!best || proto->ContainerSlots > best->ContainerSlots
                || (proto->ContainerSlots == best->ContainerSlots && proto->BuyPrice < best->BuyPrice))
                best = proto;
        }
        return best;
    }

    uint32 BuyUpgradeBag(Player* bot, Creature* vendor, uint32& outUpgradeSlots, uint32& copperSpent)
    {
        if (!vendor || !_settings.buyBags)
            return 0;
        uint8 empty = FirstEmptyBagSlot(bot);
        uint8 weakSlot = NULL_SLOT;
        uint32 weakSize = 0;
        EquippedGeneralBagSlots(bot, &weakSlot, &weakSize, nullptr);
        uint32 biggerThan = 0;
        if (empty == NULL_SLOT)
        {
            if (weakSlot == NULL_SLOT)
                return 0;
            biggerThan = weakSize;
        }
        ItemTemplate const* proto = BestVendorBag(vendor->GetEntry(), bot, biggerThan);
        if (!proto)
            return 0;
        float discount = bot->GetReputationPriceDiscount(vendor);
        uint32 price = uint32(std::floor(proto->BuyPrice * discount));
        if (!price)
            price = 1;
        uint32 money = bot->GetMoney();
        uint32 reserve = _settings.bagBuyReserveCopper;
        if (BotEconomy::IsLive())
            reserve = std::max(reserve, BotEconomy::MoneyReserveCopper());
        if (money < price + reserve)
            return 0;
        if (CountFreeGeneralSlotsLocal(bot) == 0 && empty == NULL_SLOT)
            return 0;
        Item* bagItem = StoreOneItem(bot, proto->ItemId);
        if (!bagItem)
            return 0;
        bot->ModifyMoney(-int32(price));
        copperSpent += price;
        uint8 destSlot = empty != NULL_SLOT ? empty : weakSlot;
        if (destSlot != NULL_SLOT)
            EquipBagInto(bot, bagItem, destSlot);
        if (proto->ContainerSlots > outUpgradeSlots)
            outUpgradeSlots = proto->ContainerSlots;
        return 1;
    }

    void ReportBagCleanup(Player* tellPlayer, Player* bot, BotMaintenance::BagCleanupResult const& result)
    {
        if (tellPlayer && (result.stacksSold || result.copperEarned))
        {
            std::string text = Acore::StringFormat("{} sold {} items ({}).",
                bot->GetName(), result.stacksSold, FormatCopperLocal(result.copperEarned));
            SendAltBots(tellPlayer, text);
        }
        if (tellPlayer && (result.bagsEquipped || result.bagsBought || result.bagsGranted) && result.upgradeSlots)
        {
            std::string text = Acore::StringFormat("{} upgraded a bag ({} slots).",
                bot->GetName(), result.upgradeSlots);
            SendAltBots(tellPlayer, text);
        }
        if (result.stacksSold || result.stacksDestroyed || result.bagsEquipped || result.bagsBought || result.bagsGranted)
            LOG_INFO("module.coa-playerbots",
                "BotMaintenance: '{}' bags: sold {} ({}c), destroyed {}, merged {}, equipped {}, bought {}, granted {}, upgrade {} slots, free now {}.",
                bot->GetName(), result.stacksSold, result.copperEarned, result.stacksDestroyed, result.stacksMerged,
                result.bagsEquipped, result.bagsBought, result.bagsGranted, result.upgradeSlots,
                CountFreeGeneralSlotsLocal(bot));
    }

    bool HasVendorSellable(Player* bot)
    {
        ToolPlan tools = ClassifyTools(bot);
        GearPlan gear = ClassifyGear(bot);
        return CountSellableBagItems(bot, tools, gear) > 0;
    }
}

namespace BotMaintenance
{
    void LoadConfig()
    {
        _settings.repairOnLeaderVendor = sConfigMgr->GetOption<bool>("CoaBots.Maintenance.RepairOnLeaderVendor", true);
        _settings.buyAmmoOnLeaderVendor = sConfigMgr->GetOption<bool>("CoaBots.Maintenance.BuyAmmoOnLeaderVendor", true);
        _settings.autoTravel = sConfigMgr->GetOption<bool>("CoaBots.Maintenance.AutoTravel", true);
        _settings.shareRange = sConfigMgr->GetOption<float>("CoaBots.Maintenance.ShareRange", 80.0f);
        _settings.travelSearchYards = sConfigMgr->GetOption<float>("CoaBots.Maintenance.TravelSearchYards", 30000.0f);
        _settings.nearbyServiceYards = sConfigMgr->GetOption<float>("CoaBots.Maintenance.NearbyServiceYards", 80.0f);
        _settings.repairItemPct = sConfigMgr->GetOption<uint32>("CoaBots.Maintenance.RepairItemPct", 30);
        _settings.repairAveragePct = sConfigMgr->GetOption<uint32>("CoaBots.Maintenance.RepairAveragePct", 50);
        _settings.ammoLow = sConfigMgr->GetOption<uint32>("CoaBots.Maintenance.AmmoLow", 200);
        _settings.ammoFill = sConfigMgr->GetOption<uint32>("CoaBots.Maintenance.AmmoFill", 1000);
        _settings.cleanupEnable = sConfigMgr->GetOption<bool>("CoaBots.Inventory.CleanupEnable", true);
        _settings.freeSlotTarget = sConfigMgr->GetOption<uint32>("CoaBots.Inventory.FreeSlotTarget", 4);
        _settings.tradeGoodsKeep = sConfigMgr->GetOption<uint32>("CoaBots.Inventory.TradeGoodsKeep", 20);
        _settings.ensureBags = sConfigMgr->GetOption<bool>("CoaBots.Inventory.EnsureBags", true);
        _settings.buyBags = sConfigMgr->GetOption<bool>("CoaBots.Inventory.BuyBags", true);
        _settings.bagBuyReserveCopper = sConfigMgr->GetOption<uint32>("CoaBots.Inventory.BagBuyReserveCopper", 10000);

        if (_settings.freeSlotTarget < 1)
            _settings.freeSlotTarget = 1;
        if (_settings.shareRange < 5.0f)
            _settings.shareRange = 5.0f;
        if (_settings.travelSearchYards < 20.0f)
            _settings.travelSearchYards = 20.0f;
        if (_settings.nearbyServiceYards < 10.0f)
            _settings.nearbyServiceYards = 10.0f;
        if (_settings.repairItemPct > 100)
            _settings.repairItemPct = 100;
        if (_settings.repairAveragePct > 100)
            _settings.repairAveragePct = 100;
        if (_settings.ammoFill < _settings.ammoLow)
            _settings.ammoFill = _settings.ammoLow;
    }

    bool AutoTravelEnabled()
    {
        return _settings.autoTravel;
    }

    float TravelSearchYards()
    {
        return _settings.travelSearchYards;
    }

    bool HasDamagedGear(Player* bot)
    {
        return bot && SnapshotDurability(bot).anyDamaged;
    }

    bool NeedsRepair(Player* bot)
    {
        if (!bot)
            return false;
        DurabilitySnapshot snap = SnapshotDurability(bot);
        if (!snap.pieces)
            return false;
        if (snap.anyBelowItemPct)
            return true;
        return snap.maxSum && snap.curSum * 100 < snap.maxSum * _settings.repairAveragePct;
    }

    uint32 RequiredAmmoSubClass(Player const* bot)
    {
        if (!bot)
            return 0;
        Item const* weapon = bot->GetWeaponForAttack(RANGED_ATTACK);
        if (!weapon || weapon->IsBroken())
            return 0;
        ItemTemplate const* proto = weapon->GetTemplate();
        if (!proto || proto->Class != ITEM_CLASS_WEAPON)
            return 0;
        switch (proto->SubClass)
        {
            case ITEM_SUBCLASS_WEAPON_BOW:
            case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                return ITEM_SUBCLASS_ARROW;
            case ITEM_SUBCLASS_WEAPON_GUN:
                return ITEM_SUBCLASS_BULLET;
            default:
                return 0;
        }
    }

    uint32 CountAmmo(Player const* bot, uint32 subClass)
    {
        if (!bot || !subClass)
            return 0;
        uint32 total = 0;
        ForEachInventoryItem(bot, [&](Item const* item)
        {
            if (!item)
                return;
            ItemTemplate const* proto = item->GetTemplate();
            if (proto && proto->Class == ITEM_CLASS_PROJECTILE && proto->SubClass == subClass)
                total += item->GetCount();
        });
        return total;
    }

    bool NeedsAmmo(Player* bot)
    {
        uint32 subClass = RequiredAmmoSubClass(bot);
        if (!subClass)
            return false;
        return CountAmmo(bot, subClass) < _settings.ammoLow;
    }

    bool VendorSellsUsableAmmo(uint32 vendorEntry, Player const* bot, uint32 subClass)
    {
        return BestVendorAmmo(vendorEntry, bot, subClass) != nullptr;
    }

    RepairOutcome RepairGear(Player* bot, Creature* repairNpc)
    {
        RepairOutcome result;
        if (!bot)
            return result;

        float discountMod = repairNpc ? bot->GetReputationPriceDiscount(repairNpc) : 1.0f;

        VisitRepairPositions(bot, [&](uint16 pos)
        {
            Item* item = bot->GetItemByPos(pos);
            RepairCost cost = ComputeRepairCost(item, discountMod);
            if (cost.kind == RepairCostKind::None)
                return;
            if (cost.kind == RepairCostKind::MissingTable)
            {
                // Paid Player::DurabilityRepair returns without mending when ItemLevel has no
                // DurabilityCosts.dbc row -- typical of custom CoA gear. Mend those anyway.
                bot->DurabilityRepair(pos, false, 1.0f, false);
                ++result.piecesRepaired;
                ++result.freeMends;
                return;
            }
            if (!bot->HasEnoughMoney(cost.copper))
            {
                ++result.piecesSkipped;
                return;
            }
            uint32 charged = bot->DurabilityRepair(pos, true, discountMod, false);
            result.copperSpent += charged;
            ++result.piecesRepaired;
        });
        return result;
    }

    AmmoOutcome BuyAmmoFromVendorEntry(Player* bot, uint32 vendorEntry, Creature* discountNpc)
    {
        AmmoOutcome result;
        if (!bot || !vendorEntry)
            return result;

        uint32 subClass = RequiredAmmoSubClass(bot);
        if (!subClass)
            return result;

        ItemTemplate const* proto = BestVendorAmmo(vendorEntry, bot, subClass);
        if (!proto)
            return result;

        uint32 have = CountAmmo(bot, subClass);
        if (have >= _settings.ammoFill)
        {
            if (bot->HasItemCount(proto->ItemId) && bot->GetUInt32Value(PLAYER_AMMO_ID) != proto->ItemId)
                bot->SetAmmo(proto->ItemId);
            return result;
        }

        uint32 buyCount = proto->BuyCount ? proto->BuyCount : 1;
        uint32 want = _settings.ammoFill - have;
        uint32 purchases = (want + buyCount - 1) / buyCount;

        float discount = discountNpc ? bot->GetReputationPriceDiscount(discountNpc) : 1.0f;
        uint32 unitPrice = uint32(std::floor(proto->BuyPrice * discount));
        if (!unitPrice)
            unitPrice = 1;

        uint32 money = bot->GetMoney();
        if (unitPrice * purchases > money)
            purchases = money / unitPrice;
        if (!purchases)
        {
            result.fail = "gold";
            result.itemName = proto->Name1.c_str();
            return result;
        }

        uint32 items = purchases * buyCount;
        while (purchases)
        {
            items = purchases * buyCount;
            ItemPosCountVec dest;
            if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, proto->ItemId, items) == EQUIP_ERR_OK)
            {
                uint32 price = unitPrice * purchases;
                if (price)
                    bot->ModifyMoney(-int32(price));
                bot->StoreNewItem(dest, proto->ItemId, true);
                result.itemId = proto->ItemId;
                result.bought = items;
                result.copperSpent = price;
                result.itemName = proto->Name1.c_str();
                if (bot->HasItemCount(proto->ItemId))
                    bot->SetAmmo(proto->ItemId);
                return result;
            }
            --purchases;
        }

        result.fail = "bags";
        result.itemName = proto->Name1.c_str();
        return result;
    }

    AmmoOutcome BuyAmmo(Player* bot, Creature* vendor)
    {
        if (!bot || !vendor)
            return {};
        return BuyAmmoFromVendorEntry(bot, vendor->GetEntry(), vendor);
    }

    std::string FormatCopper(uint32 copper)
    {
        return FormatCopperLocal(copper);
    }

    void Tell(Player* player, std::string const& text)
    {
        SendAltBots(player, text);
    }

    void ServiceNearby(Player* bot, bool arrivedForRepair, bool arrivedForVendor, Player* tellPlayer)
    {
        if (!bot || !bot->IsAlive() || !bot->IsInWorld() || bot->IsBeingTeleported())
            return;

        Player* listener = tellPlayer ? tellPlayer : GatherRequesterOf(bot);
        float const serviceRange = arrivedForRepair || arrivedForVendor ? _settings.nearbyServiceYards : 20.0f;

        if (HasDamagedGear(bot))
        {
            Creature* armorer = FindArmorer(bot, serviceRange);
            if (arrivedForRepair || armorer)
            {
                RepairOutcome repaired = RepairGear(bot, armorer);
                ReportRepair(listener, bot, repaired);
            }
        }

        uint32 subClass = RequiredAmmoSubClass(bot);
        if (subClass && CountAmmo(bot, subClass) < _settings.ammoFill)
        {
            Creature* vendor = FindAmmoVendor(bot, serviceRange, subClass);
            if (vendor)
            {
                AmmoOutcome bought = BuyAmmo(bot, vendor);
                ReportAmmo(listener, bot, bought);
            }
            else if (arrivedForVendor)
            {
                // Arrived at a vendor POI that may not be loaded in this grid: still buy from its entry
                // if the trip saved one... handled by the caller passing a live vendor when possible.
            }
        }

        Creature* bagVendor = FindAnyVendor(bot, serviceRange);
        if (bagVendor || arrivedForVendor)
            CleanBags(bot, arrivedForVendor, true, bagVendor, nullptr);
    }

    void OnPlayerOpenedNpc(Player* player, Creature* npc)
    {
        if (!player || !npc || !player->GetSession())
            return;
        if (BotMgr::IsBotAccountId(player->GetSession()->GetAccountId()))
            return;
        if (!npc->IsAlive())
            return;

        bool const canRepair = npc->IsArmorer();
        bool const canSell = npc->IsVendor();
        if (!canRepair && !canSell)
            return;

        Group* group = player->GetGroup();
        if (!group)
            return;

        uint32 const now = getMSTime();
        static std::unordered_map<ObjectGuid, uint32> lastServiceAt;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == player || !member->IsAlive() || !member->IsInWorld())
                continue;
            if (member->IsBeingTeleported() || !sBotMgr->FindBotPlayer(member->GetGUID().GetCounter()))
                continue;
            if (member->GetMap() != player->GetMap() || !member->IsWithinDistInMap(player, _settings.shareRange))
                continue;

            uint32& lastAt = lastServiceAt[member->GetGUID()];
            if (lastAt && getMSTimeDiff(lastAt, now) < 2000)
                continue;
            lastAt = now;

            if (_settings.repairOnLeaderVendor && canRepair && HasDamagedGear(member))
            {
                RepairOutcome repaired = RepairGear(member, npc);
                ReportRepair(player, member, repaired);
            }

            uint32 subClass = RequiredAmmoSubClass(member);
            if (_settings.buyAmmoOnLeaderVendor && canSell && subClass &&
                CountAmmo(member, subClass) < _settings.ammoFill &&
                VendorSellsUsableAmmo(npc->GetEntry(), member, subClass))
            {
                AmmoOutcome bought = BuyAmmo(member, npc);
                ReportAmmo(player, member, bought);
            }

            if (canSell)
                CleanBags(member, true, false, npc, player);
        }
    }
}

void BotMaintenance::ServiceAfterNeedTrip(Player* bot, bool arrivedForRepair, uint32 vendorEntry)
{
    if (!bot || !bot->IsAlive() || !bot->IsInWorld() || bot->IsBeingTeleported())
        return;

    Player* listener = GatherRequesterOf(bot);

    if (arrivedForRepair || HasDamagedGear(bot))
    {
        Creature* armorer = FindArmorer(bot, _settings.nearbyServiceYards);
        if (arrivedForRepair || armorer)
            ReportRepair(listener, bot, RepairGear(bot, armorer));
    }

    uint32 subClass = RequiredAmmoSubClass(bot);
    if (subClass && CountAmmo(bot, subClass) < _settings.ammoFill)
    {
        Creature* vendor = FindAmmoVendor(bot, _settings.nearbyServiceYards, subClass);
        if (vendor)
            ReportAmmo(listener, bot, BuyAmmo(bot, vendor));
        else if (vendorEntry && VendorSellsUsableAmmo(vendorEntry, bot, subClass))
            ReportAmmo(listener, bot, BuyAmmoFromVendorEntry(bot, vendorEntry, nullptr));
    }

    Creature* bagVendor = FindAnyVendor(bot, _settings.nearbyServiceYards);
    CleanBags(bot, true, true, bagVendor, listener);
}

namespace BotMaintenance
{
    uint32 CountFreeGeneralSlots(Player const* bot)
    {
        return CountFreeGeneralSlotsLocal(bot);
    }

    std::string DescribeBags(Player const* bot)
    {
        if (!bot)
            return "";
        uint32 freeSlots = CountFreeGeneralSlotsLocal(bot);
        uint32 total = INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START;
        std::string bags;
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Bag const* bag = bot->GetBagByPos(bagSlot);
            if (!bag)
            {
                bags += bags.empty() ? "none" : "/empty";
                continue;
            }
            total += bag->GetBagSize();
            if (!bags.empty())
                bags += "/";
            bags += std::to_string(bag->GetBagSize());
        }
        ToolPlan tools = ClassifyTools(const_cast<Player*>(bot));
        GearPlan gear = ClassifyGear(const_cast<Player*>(bot));
        uint32 sellable = CountSellableBagItems(const_cast<Player*>(bot), tools, gear);
        return Acore::StringFormat("bags: {}/{} free (backpack 16, equipped {}), {} sellable",
            freeSlots, total, bags, sellable);
    }

    bool NeedsSellTrip(Player* bot)
    {
        if (!_settings.cleanupEnable || !bot || !bot->IsAlive() || !bot->IsInWorld())
            return false;
        if (bot->GetGroup())
            return false;
        // An economy sale trip already going (or queued) handles vendoring and AH
        // listings; do not start a second walk to a vendor.
        if (BotEconomy::HasSaleTrip(bot))
            return false;
        // Full or nearly full: trip when free general slots are at or below the
        // configured target (default 4) and something a vendor would buy is in the bags,
        // including unusable white/green gear and extra profession tools.
        if (CountFreeGeneralSlotsLocal(bot) > _settings.freeSlotTarget)
            return false;
        return HasVendorSellable(bot);
    }

    BagCleanupResult CleanBags(Player* bot, bool force, bool allowDestroy, Creature* vendor, Player* tellPlayer)
    {
        BagCleanupResult result;
        if (!_settings.cleanupEnable || !CanActOnBags(bot))
            return result;

        result.stacksMerged = MergeStacks(bot);
        result.bagsEquipped = EquipUpgradeBags(bot, result.upgradeSlots);
        EquipBetterGear(bot);

        uint32 freeSlots = CountFreeGeneralSlotsLocal(bot);
        bool bagsTight = force || freeSlots < _settings.freeSlotTarget;
        // Unusable gear and extra tools are sold whenever a vendor is present,
        // not only once bags are already tight. Trade goods stay tight-only.
        bool sellNow = bagsTight || vendor != nullptr;

        if (sellNow)
        {
            ToolPlan tools = ClassifyTools(bot);
            GearPlan gear = ClassifyGear(bot);
            std::unordered_map<uint32, BotEconomy::Verdict> liveVerdicts;
            bool const live = TryLiveVerdicts(bot, liveVerdicts);

            std::unordered_map<uint32, uint32> tradeCount;
            std::unordered_map<uint32, uint32> consCount;
            std::vector<Item*> items;
            CollectInventoryItems(bot, items);
            for (Item* item : items)
            {
                if (!item || !item->GetTemplate())
                    continue;
                if (item->GetTemplate()->Class == ITEM_CLASS_TRADE_GOODS)
                    tradeCount[item->GetEntry()] += item->GetCount();
                if (item->GetTemplate()->Class == ITEM_CLASS_CONSUMABLE)
                    consCount[item->GetEntry()] += item->GetCount();
            }

            // Lowest value first so leftover useful stacks of a trade good stay.
            std::sort(items.begin(), items.end(), [](Item* a, Item* b)
            {
                uint64 va = 0, vb = 0;
                if (a && a->GetTemplate())
                    va = uint64(a->GetTemplate()->SellPrice) * a->GetCount();
                if (b && b->GetTemplate())
                    vb = uint64(b->GetTemplate()->SellPrice) * b->GetCount();
                return va < vb;
            });

            for (Item* item : items)
            {
                if (!item)
                    continue;
                ItemTemplate const* proto = item->GetTemplate();
                if (!proto)
                    continue;
                ObjectGuid const guid = item->GetGUID();
                if (tools.keep.count(guid))
                    continue;
                if (EconomyHoldsItem(proto->ItemId, live ? &liveVerdicts : nullptr))
                    continue;

                uint32 traded = proto->Class == ITEM_CLASS_TRADE_GOODS ? tradeCount[item->GetEntry()] : 0;
                uint32 consumed = proto->Class == ITEM_CLASS_CONSUMABLE ? consCount[item->GetEntry()] : 0;
                BagItemFate fate = BagItemFate::Keep;
                if (tools.sell.count(guid) || gear.sell.count(guid))
                    fate = proto->SellPrice > 0 ? BagItemFate::Sell : BagItemFate::Destroy;
                else if (bagsTight)
                    fate = FateOfItem(bot, item, traded, consumed, true);
                if (fate == BagItemFate::Keep)
                    continue;

                uint8 bagSlot = item->GetBagSlot();
                uint8 slot = item->GetSlot();
                uint32 count = item->GetCount();
                uint32 entry = item->GetEntry();

                if (fate == BagItemFate::Sell && vendor && proto->SellPrice > 0
                    && sScriptMgr->OnPlayerCanSellItem(bot, item, vendor))
                {
                    uint32 money = proto->SellPrice * count;
                    bot->ModifyMoney(money);
                    result.copperEarned += money;
                    bot->DestroyItem(bagSlot, slot, true);
                    ++result.stacksSold;
                    if (live)
                        BotEconomy::RecordCleanupVendorSale(bot, entry, count, proto->SellPrice, "bag-cleanup");
                    if (proto->Class == ITEM_CLASS_TRADE_GOODS)
                    {
                        if (tradeCount[entry] > count)
                            tradeCount[entry] -= count;
                        else
                            tradeCount[entry] = 0;
                    }
                    if (proto->Class == ITEM_CLASS_CONSUMABLE)
                    {
                        if (consCount[entry] > count)
                            consCount[entry] -= count;
                        else
                            consCount[entry] = 0;
                    }
                    continue;
                }

                if (fate == BagItemFate::Destroy && allowDestroy)
                {
                    bot->DestroyItem(bagSlot, slot, true);
                    ++result.stacksDestroyed;
                    continue;
                }

                // Sellable but no vendor: destroy greys, leftover quest items, and
                // unusable white/green (or extra tools) only when there is no general
                // slot left, so a later sell trip still has something to sell.
                bool const disposable = proto->Quality == ITEM_QUALITY_POOR
                    || StaleQuestItem(bot, proto)
                    || tools.sell.count(guid)
                    || gear.sell.count(guid);
                if (fate == BagItemFate::Sell && !vendor && allowDestroy
                    && CountFreeGeneralSlotsLocal(bot) == 0
                    && disposable)
                {
                    bot->DestroyItem(bagSlot, slot, true);
                    ++result.stacksDestroyed;
                }
            }
        }

        if (vendor)
        {
            uint32 bagCopper = 0;
            result.bagsBought = BuyUpgradeBag(bot, vendor, result.upgradeSlots, bagCopper);
        }

        // Grant after a sell so a newly freed slot can hold the bag before it is equipped.
        if (_settings.ensureBags)
            result.bagsGranted = GrantEnsureBags(bot, result.upgradeSlots);

        // A leftover granted/bought bag still sitting in inventory.
        uint32 moreSlots = 0;
        uint32 more = EquipUpgradeBags(bot, moreSlots);
        result.bagsEquipped += more;
        if (moreSlots > result.upgradeSlots)
            result.upgradeSlots = moreSlots;

        ReportBagCleanup(tellPlayer, bot, result);
        return result;
    }

    void MaintainBags(Player* bot)
    {
        if (!_settings.cleanupEnable || !bot)
            return;
        Creature* vendor = FindAnyVendor(bot, 20.0f);
        CleanBags(bot, false, true, vendor, nullptr);
    }

    BagCleanupResult CleanBagsAround(Player* bot, bool force, bool allowDestroy, float yards, Player* tellPlayer)
    {
        Creature* vendor = bot ? FindAnyVendor(bot, yards) : nullptr;
        return CleanBags(bot, force, allowDestroy, vendor, tellPlayer);
    }
}
