/*
 * Grouped bots repair, restock ammo, sell junk and upgrade bags when the
 * commander opens a suitable NPC. Ungrouped bots (including those on a gather
 * order) travel to the nearest repairer, ammo vendor or vendor when durability,
 * ammo or bag space is low, then resume what they were doing. The gather-order
 * assignment is left in place so work continues after the trip.
 */

#ifndef COA_PLAYERBOTS_BOT_MAINTENANCE_H
#define COA_PLAYERBOTS_BOT_MAINTENANCE_H

#include "Define.h"
#include <string>
#include <vector>

class Creature;
class Player;

namespace BotMaintenance
{
    void LoadConfig();

    bool CleanupEnabled();
    bool AutoTravelEnabled();
    float TravelSearchYards();

    // Travel when any equipped piece is below the item-percent setting, or the
    // average of all equipped durability is below the average-percent setting.
    bool NeedsRepair(Player* bot);
    bool HasDamagedGear(Player* bot);

    // 0 if the bot has no bow, gun, or crossbow equipped.
    uint32 RequiredAmmoSubClass(Player const* bot);
    uint32 CountAmmo(Player const* bot, uint32 subClass);
    bool NeedsAmmo(Player* bot);
    bool VendorSellsUsableAmmo(uint32 vendorEntry, Player const* bot, uint32 subClass);

    struct RepairOutcome
    {
        uint32 piecesRepaired = 0;
        uint32 piecesSkipped = 0;
        uint32 copperSpent = 0;
        uint32 freeMends = 0;
    };
    // Mends every damaged piece the bot can afford. Items with no durability-cost
    // table entry (common on this server for custom gear) are still mended without
    // a charge. guild bank is never used. repairNpc may be null (no reputation discount).
    RepairOutcome RepairGear(Player* bot, Creature* repairNpc);

    struct AmmoOutcome
    {
        uint32 itemId = 0;
        uint32 bought = 0;
        uint32 copperSpent = 0;
        char const* fail = nullptr;
        char const* itemName = "";
    };
    AmmoOutcome BuyAmmo(Player* bot, Creature* vendor);
    AmmoOutcome BuyAmmoFromVendorEntry(Player* bot, uint32 vendorEntry, Creature* discountNpc);

    // A real (non-bot) player opened a repair-capable NPC or a vendor. Grouped bots
    // on the same map and within the configured share range repair and/or restock.
    // At a vendor (CoaBots.Inventory.SellOnLeaderVendor) they also list their
    // auction finds when the economy lists from anywhere, then sell junk and safe
    // vendor trash; the commander is told in chat (prefix "AltBots:") what sold.
    void OnPlayerOpenedNpc(Player* player, Creature* npc);

    // Act on whatever service is already at hand: an armorer / ammo vendor standing
    // next to the bot, or a need-trip arrival (arrivedForRepair / arrivedForVendor
    // skip the live-NPC search for that service). tellPlayer receives chat; if null,
    // a gather-order requester is notified instead, and random world bots stay quiet.
    void ServiceNearby(Player* bot, bool arrivedForRepair, bool arrivedForVendor, Player* tellPlayer);

    // Called when an ungrouped bot arrives at a repairer or ammo vendor it travelled to.
    // vendorEntry is the destination NPC's template id (0 if unknown).
    void ServiceAfterNeedTrip(Player* bot, bool arrivedForRepair, uint32 vendorEntry);

    std::string FormatCopper(uint32 copper);
    void Tell(Player* player, std::string const& text);

    // Free slots in the backpack and general-purpose bags only. Quiver and
    // profession-bag slots are skipped -- most loot cannot go there.
    uint32 CountFreeGeneralSlots(Player const* bot);

    // One-line bag status for `.botcmd why`.
    std::string DescribeBags(Player const* bot);

    // Ungrouped bots should travel to a vendor: general slots are below the
    // configured target and a vendor would buy something.
    bool NeedsSellTrip(Player* bot);

    struct SoldStack
    {
        uint32 entry = 0;
        uint32 count = 0;
    };

    struct BagCleanupResult
    {
        uint32 stacksSold = 0;
        uint32 stacksDestroyed = 0;
        uint32 copperEarned = 0;
        uint32 bagsEquipped = 0;
        uint32 bagsBought = 0;
        uint32 bagsGranted = 0;
        uint32 stacksMerged = 0;
        uint32 upgradeSlots = 0;
        std::vector<SoldStack> sold;
    };

    // Consolidate stacks, equip or swap larger bags, sell or destroy junk, and
    // optionally buy or grant level-appropriate bags. `force` skips the nearly-full
    // gate. `allowDestroy` lets greys and leftover quest items be destroyed when
    // no vendor is at hand. `vendor` may be null. `tellPlayer` gets AltBots chat.
    BagCleanupResult CleanBags(Player* bot, bool force, bool allowDestroy, Creature* vendor, Player* tellPlayer);

    // Periodic bag maintenance for every bot (grouped or not).
    void MaintainBags(Player* bot);

    // CleanBags after looking for a vendor within `yards` of the bot.
    BagCleanupResult CleanBagsAround(Player* bot, bool force, bool allowDestroy, float yards, Player* tellPlayer);
}

#endif // COA_PLAYERBOTS_BOT_MAINTENANCE_H
