/*
 * mod-coa-playerbots
 *
 * Bot economy: what bots sell, buy and carry to town.
 *
 * A bot carries things it will never use again: grey junk, gear it has outgrown or cannot wear,
 * potions and food beyond what it needs, valuable finds. PlanSale looks into a bot's bags and sorts
 * every stack into Keep / Vendor / Auction by a fixed set of rules (BotEconomy.cpp), puts a price on
 * it (the auction houses' current lowest buyout when there is one, the vendor price otherwise) and
 * writes what it found to a journal the owner can read. With CoaBots.Economy.DryRun = 0 bots act on
 * the plan: they travel to a vendor or an auction house, sell and list, collect their auction mail,
 * buy food, drink and potions, and pay a share into the guild bank.
 *
 * Buying: with CoaBots.Economy.AuctionBuyGear = 1 a bot with money to spare also shops the auction
 * house for gear upgrades for its spec. Crafting orders (BotMgr::CraftOrder) fetch missing reagents
 * through SourceReagents: from the guild bank, from vendors and from the auction house, each one
 * switched on by its own CoaBots.Crafting.* setting.
 *
 * The rules never touch what a bot needs: equipped gear, quest items, profession tools, bags,
 * trade goods (they go to the guild bank), recipes, keys, the hearthstone, anything the owner
 * protected with `.botcmd economy protect <item>`, and gear that is, or could become, an upgrade.
 */

#ifndef COA_PLAYERBOTS_BOT_ECONOMY_H
#define COA_PLAYERBOTS_BOT_ECONOMY_H

#include "Define.h"
#include "ObjectGuid.h"
#include <string>
#include <vector>

class ChatHandler;
class Item;
class Player;
class SpellInfo;
class WorldSession;

namespace BotEconomy
{
    enum class Verdict : uint8
    {
        Keep,
        Vendor,
        Auction,
    };

    // One stack (or several stacks of the same item, merged) the plan would sell.
    struct Line
    {
        uint32 entry = 0;
        uint32 count = 0;
        Verdict verdict = Verdict::Keep;
        char const* reason = "";
        // What one item is expected to fetch, and where that number comes from.
        uint32 unitCopper = 0;
        char const* priceSource = "";
        std::string name;

        uint64 TotalCopper() const { return uint64(unitCopper) * count; }
    };

    // Everything in the bot's bags that would be sold (Keep lines are left out). Read-only.
    std::vector<Line> PlanSale(Player* bot);

    // Called every world tick with the online bots: plans, a few bots at a time, and journals.
    void Update(uint32 diff, std::vector<WorldSession*> const& sessions);

    // Called for an idle, ungrouped bot before its other solo activities. Returns true while the bot is on a trip to
    // a city with an auction house (it walks or flies there, sells, and comes back to its normal life); the caller
    // then skips everything else this tick. Only active when CoaBots.Economy.DryRun = 0.
    bool UpdateTrip(Player* bot, uint32 diff);

    // "Collect gold" orders from the guild task board: a free guild-mate bot is asked to earn `goldAmount` gold for the
    // guild bank. It goes about its usual life (kills, quests and loot give it money, its trips to town turn its finds
    // into money sooner) and pays everything above its own reserve into the bank until the amount is in. 0 gold cancels
    // the guild's gold orders. The requester must be the guild master or an officer (not checked for a console caller).
    void PlaceGoldOrder(Player* requester, uint32 goldAmount, ChatHandler* handler);

    // The roster line for a bot with a gold order ("earning gold for the guild (12/100 g - 12%)"), empty otherwise.
    std::string GoldOrderTask(Player* bot);
    bool GetGoldOrderInfo(Player* bot, uint64& outTargetCopper, uint64& outDepositedCopper);

    // Food and drink that cost something. With CoaBots.Economy.ConsumeSupplies = 1 a bot that sits down to eat or drink
    // really uses up one item of its level range (the same instant-regeneration auras as before, 433 and 431, are
    // applied); with none it just rests the slow way and wants a trip to town, where it buys more with its own money.
    // With the setting off this simply casts the aura, as bots always did.
    void StartRestAura(Player* bot, bool food);

    // Guild resource limits ("Stock" tab of the task board). The guild's officers say how many of a resource the guild bank
    // should keep; a free guild-mate bot takes the surplus out, carries it to a city with an auction house and sells it
    // (the money goes to the bank by the usual trip share). Only resources already in the bank are listed.
    //
    // GetStockLines: replies for GETSTOCK, `STOCK:<entry>,<count>,<limit>,<name>|...` (chunked by length).
    // SetStockLimit: keep >= 0 sets the limit, keep < 0 clears it. Guild master or officers only (not checked for console).
    std::vector<std::string> GetStockLines(Player* requester);
    void SetStockLimit(Player* requester, uint32 itemEntry, int64 keep, ChatHandler* handler);

    // `.botcmd economy ...` (plan <guid>, journal [n], status, protect <item>, unprotect <item>).
    void HandleCommand(ChatHandler* handler, std::string const& args);

    // True when item_entry is in mod_coa_bot_protected_items and that table has been loaded.
    // False when the economy tables have not been created yet.
    bool IsProtectedItem(uint32 itemEntry);

    // CoaBots.Economy.Enable = 1 and DryRun = 0: bots actually trade, not just plan.
    bool IsLive();

    // True while this bot is travelling to town for an economy sale, or is queued to.
    bool HasSaleTrip(Player const* bot);

    // CoaBots.Economy.MoneyReserveCopper (default 5g).
    uint32 MoneyReserveCopper();

    // Write an executed vendor-sale row for a bag-cleanup sell. No-op when the economy is off.
    void RecordCleanupVendorSale(Player* bot, uint32 itemEntry, uint32 count, uint32 unitCopper, char const* reason);

    // Reagents of the bot's crafting order: bag cleanup, guild-order deposits and sales leave them alone.
    bool IsReservedReagent(Player* bot, uint32 itemEntry);

    // A reserved reagent, or gear the bot bought at the auction house in the last few hours (it is equipped right
    // away; one that could not be is kept for a while instead of being sold straight back).
    bool IsReservedItem(Player* bot, Item const* item);

    // Takes the items and money a player mailed the bot (crafting reagents, for one), from that sender only.
    void CollectMailFrom(Player* bot, ObjectGuid sender);

    struct ReagentShortfall
    {
        uint32 entry = 0;
        uint32 count = 0;
    };

    // What `crafts` casts of the recipe still need beyond what the crafter carries.
    std::vector<ReagentShortfall> MissingReagents(Player* crafter, SpellInfo const* recipe, uint32 crafts);

    // Fetches the reagents a crafting order is short of, for up to a few casts at once: from the guild bank
    // (CoaBots.Crafting.ReagentsFromGuildBank), then from vendors that sell them for gold
    // (CoaBots.Crafting.ReagentsFromVendor), then from the auction house (CoaBots.Crafting.ReagentsFromAuction,
    // needs the live economy). Purchases come out of the crafter's money above its reserve and out of
    // budgetLeftCopper. Returns what is still missing.
    std::vector<ReagentShortfall> SourceReagents(Player* crafter, SpellInfo const* recipe, uint32 crafts, uint64& budgetLeftCopper);

    // "3x Bolt of Linen Cloth, 1x Coarse Thread".
    std::string DescribeReagents(std::vector<ReagentShortfall> const& reagents);

    bool CraftSourcingEnabled();
    uint64 CraftReagentBudgetCopper();
    // 0: a crafting order waits for its reagents forever.
    uint32 CraftOrderTimeoutMs();
}

#endif // COA_PLAYERBOTS_BOT_ECONOMY_H
