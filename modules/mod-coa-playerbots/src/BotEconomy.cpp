#include "training/TrainingArenaManager.h"
#include "BotGear.h"
#include "BotEconomy.h"
#include "BotEconomyPolicy.h"
#include "Map.h"

#include "AuctionHouseMgr.h"
#include "Bag.h"
#include "BotAI.h"
#include "BotMgr.h"
#include "BotMovement.h"
#include "BotProgression.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Mail.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldConfig.h"
#include "WorldSession.h"
#include "world/BotWorldBehavior.h"
#include "world/BotWorldPoi.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
    constexpr uint32 HEARTHSTONE_ENTRY = 6948;

    // Settings, re-read every few seconds: the config manager warns on every lookup of a key the active
    // file does not have, and servers updated from older packages do not have these.
    struct Settings
    {
        bool enable = false;
        uint32 minTripCopper = 50000;      // 5 gold: below this a bot is not worth a trip to town
        uint32 maxAuctionTripYards = 6000; // farther than this the auction house is not worth the walk: sell to the nearest vendor instead
        uint32 bagsFullFreeSlots = 3;      // this few free slots (or fewer) is a reason for a trip even when the load is worth little
        uint32 potionTarget = 5;           // healing and mana potions a bot buys up to
        uint32 keepConsumables = 10;       // potions / food of its own level range kept per item
        uint32 outdatedBelowLevels = 15;   // a consumable this many levels under the bot is outdated
        uint32 minAuctionQuality = 2;      // uncommon and better go to the auction, the rest to a vendor
        uint32 undercutPercent = 5;        // what the plan assumes below the lowest current buyout
        uint32 scanIntervalSec = 300;      // how often one bot is looked at
        uint32 botsPerTick = 2;            // bots looked at per world tick
        uint32 journalDays = 14;           // journal rows older than this are removed
        bool dryRun = true;                // plan only: nobody travels or sells
        uint32 maxTrips = 15;              // bots on their way to town at the same time
        uint32 tripCooldownMin = 30;       // minutes before the same bot is sent again
        uint32 guildSharePercent = 10;     // of what a trip earns, paid into the guild bank
        uint32 moneyReserveCopper = 50000; // a bot keeps at least this much for its own needs
        uint32 maxListings = 40;           // auctions one bot may have running
        uint32 spreadMin = 5;              // listing prices vary by this many percent ...
        uint32 spreadMax = 10;             // ... up to this many, up or down
        uint32 minListingCopper = 1000;    // a listing worth less than this is not worth the deposit: a vendor takes it
        uint32 listingMinutes = 2880;      // how long a listing runs
        bool consumeSupplies = false;      // eating and drinking use up food and drink
        uint32 supplyTarget = 20;          // how many of each a bot likes to carry
        uint32 dailyBudgetCopper = 20000;  // what one bot spends on supplies in a day
        bool auctionBuyGear = false;       // bots buy gear upgrades at the auction house
        uint32 auctionBuyBudgetPercent = 25;   // of the money above the reserve, spent on one shopping visit
        uint32 auctionBuyMaxItems = 2;         // upgrades bought on one visit
        uint32 auctionBuyMinUpgradePercent = 10; // how much better than the worn piece an upgrade has to be
        bool auctionBuyFromBots = true;        // bots may buy other bots' listings, not only players'
        uint32 shoppingMinSpareCopper = 200000; // 20 gold above the reserve before a bot goes shopping on its own
        uint32 shoppingIntervalMin = 120;      // minutes between shopping trips of one bot
        uint32 auctionHourlyCapCopper = 5000000; // what all bots together may spend at the auction houses in one hour (0 = no cap)
        uint32 auctionMaxVendorMultiple = 20;  // a bot pays at most this many times the vendor price per item (0 = no ceiling)
        bool listFromAnywhere = false;         // bots list their auction finds wherever they are, without a trip to town
        bool sellTradeGoods = false;           // gathered and looted materials go to the auction house
        uint32 tradeGoodsKeep = 0;             // of each material a bot keeps for itself
        uint32 estimateMultiplier = 3;         // unlisted items are priced at this many times the vendor price
        bool craftForMarket = false;           // crafters turn their own materials into goods and list them
        uint32 craftForMarketMaxCasts = 10;    // crafts per auction house visit
        uint32 craftForMarketMarginPercent = 25; // how much more the product must fetch than its reagents are worth
        bool craftFromGuildBank = false;   // crafting orders take missing reagents from the guild bank
        bool craftFromVendor = false;      // ... buy the ones vendors sell for gold
        bool craftFromAuction = false;     // ... and buy the rest at the auction house
        uint32 craftBudgetCopper = 100000; // what one crafting order may spend on reagents
        uint32 craftTimeoutMin = 120;      // a crafting order still short of reagents after this long is given up
        uint32 craftMaxCastFailures = 10;  // failed casts in a row before a crafting order is given up
    } g_settings;

    uint32 g_settingsAgeMs = 1000000;
    bool g_tablesReady = false;
    std::unordered_set<uint32> g_protected;

    struct GoldOrder
    {
        uint32 guildId = 0;
        uint64 targetCopper = 0;
        uint64 depositedCopper = 0;
    };
    std::unordered_map<ObjectGuid, GoldOrder> g_goldOrders;

    // Stock limits per guild, the bots carrying a stock surplus to town, and one run at a time per guild.
    std::unordered_map<uint32, std::unordered_map<uint32, uint32>> g_stockLimits; // guild -> item -> keep
    std::unordered_map<ObjectGuid, std::unordered_set<uint32>> g_stockCarry;       // bot -> items it carries for sale
    std::unordered_map<uint32, ObjectGuid> g_stockRuns;                            // guild -> the bot on a run
    std::unordered_map<uint64, uint32> g_stockBlockedUntil;                        // (guild << 32 | item) -> game ms
    uint32 g_stockCheckAgeMs = 0;
    constexpr uint32 STOCK_CHECK_MS = 60 * 1000;
    uint32 g_goldCheckAgeMs = 0;
    constexpr uint32 GOLD_CHECK_MS = 60 * 1000;
    constexpr uint32 GOLD_MIN_DEPOSIT_COPPER = 10000; // 1 gold

    // Supplies: bots that found nothing to eat or drink, and what each spent today.
    std::unordered_set<ObjectGuid> g_needsSupplies;
    std::unordered_map<uint64, uint32> g_noSupplyUntil; // (bot guid * 2 + food) -> a bot with nothing to eat is not searched again for a few seconds
    struct Spending
    {
        uint32 dayStartedAt = 0;
        uint64 copper = 0;
    };
    std::unordered_map<ObjectGuid, Spending> g_spent;
    uint64 g_boughtCopper = 0;

    // Lowest buyout per single item over every auction house, refreshed every few minutes.
    std::unordered_map<uint32, uint32> g_market;
    std::unordered_map<uint32, uint32> g_listingCount; // auctions per owner (guid low), from the same scan
    uint32 g_marketAgeMs = 1000000;
    constexpr uint32 MARKET_REFRESH_MS = 5 * 60 * 1000;

    // Per bot: a fingerprint of the plan last written to the journal, so an unchanged plan is not written again.
    std::unordered_map<ObjectGuid, uint64> g_lastPlan;

    // Per bot: the game time (ms) it is next looked at.
    std::unordered_map<ObjectGuid, uint32> g_nextScanAt;
    size_t g_rotation = 0;
    uint32 g_pruneAgeMs = 0;

    // A few bots' plans are kept for `.botcmd economy status`.
    struct Totals
    {
        uint32 botsScanned = 0;
        uint32 botsWithSales = 0;
        uint64 vendorCopper = 0;
        uint64 auctionCopper = 0;
        uint32 journalRows = 0;
    } g_totals;

    void RefreshSettings(uint32 diff)
    {
        g_settingsAgeMs += diff;
        if (g_settingsAgeMs < 10000)
            return;
        g_settingsAgeMs = 0;
        g_settings.enable = sConfigMgr->GetOption<bool>("CoaBots.Economy.Enable", false);
        g_settings.minTripCopper = sConfigMgr->GetOption<uint32>("CoaBots.Economy.MinTripCopper", 50000);
        g_settings.keepConsumables = sConfigMgr->GetOption<uint32>("CoaBots.Economy.KeepConsumables", 10);
        g_settings.maxAuctionTripYards = sConfigMgr->GetOption<uint32>("CoaBots.Economy.MaxAuctionTripYards", 6000);
        g_settings.bagsFullFreeSlots = sConfigMgr->GetOption<uint32>("CoaBots.Economy.BagsFullFreeSlots", 3);
        g_settings.potionTarget = std::min<uint32>(20, sConfigMgr->GetOption<uint32>("CoaBots.Economy.PotionTarget", 5));
        g_settings.outdatedBelowLevels = sConfigMgr->GetOption<uint32>("CoaBots.Economy.OutdatedBelowLevels", 15);
        g_settings.minAuctionQuality = sConfigMgr->GetOption<uint32>("CoaBots.Economy.MinAuctionQuality", 2);
        g_settings.undercutPercent = std::min<uint32>(50, sConfigMgr->GetOption<uint32>("CoaBots.Economy.UndercutPercent", 5));
        g_settings.scanIntervalSec = std::max<uint32>(30, sConfigMgr->GetOption<uint32>("CoaBots.Economy.ScanIntervalSec", 300));
        g_settings.botsPerTick = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("CoaBots.Economy.BotsPerTick", 2));
        g_settings.journalDays = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("CoaBots.Economy.JournalDays", 14));
        g_settings.dryRun = sConfigMgr->GetOption<bool>("CoaBots.Economy.DryRun", true);
        g_settings.maxTrips = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("CoaBots.Economy.MaxTrips", 15));
        g_settings.tripCooldownMin = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("CoaBots.Economy.TripCooldownMin", 30));
        g_settings.guildSharePercent = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("CoaBots.Economy.GuildSharePercent", 10));
        g_settings.moneyReserveCopper = sConfigMgr->GetOption<uint32>("CoaBots.Economy.MoneyReserveCopper", 50000);
        g_settings.maxListings = sConfigMgr->GetOption<uint32>("CoaBots.Economy.MaxListings", 40);
        g_settings.spreadMin = std::min<uint32>(50, sConfigMgr->GetOption<uint32>("CoaBots.Economy.PriceSpreadMinPercent", 5));
        g_settings.consumeSupplies = sConfigMgr->GetOption<bool>("CoaBots.Economy.ConsumeSupplies", false);
        g_settings.supplyTarget = std::max<uint32>(1, std::min<uint32>(200, sConfigMgr->GetOption<uint32>("CoaBots.Economy.SupplyTarget", 20)));
        g_settings.dailyBudgetCopper = sConfigMgr->GetOption<uint32>("CoaBots.Economy.DailyBudgetCopper", 20000);
        g_settings.minListingCopper = sConfigMgr->GetOption<uint32>("CoaBots.Economy.MinListingCopper", 1000);
        g_settings.listingMinutes = std::max<uint32>(5, std::min<uint32>(2880, sConfigMgr->GetOption<uint32>("CoaBots.Economy.ListingMinutes", 2880)));
        g_settings.spreadMax = std::max<uint32>(g_settings.spreadMin, std::min<uint32>(50, sConfigMgr->GetOption<uint32>("CoaBots.Economy.PriceSpreadMaxPercent", 10)));
        g_settings.auctionBuyGear = sConfigMgr->GetOption<bool>("CoaBots.Economy.AuctionBuyGear", false);
        g_settings.auctionBuyBudgetPercent = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("CoaBots.Economy.AuctionBuyBudgetPercent", 25));
        g_settings.auctionBuyMaxItems = std::min<uint32>(10, sConfigMgr->GetOption<uint32>("CoaBots.Economy.AuctionBuyMaxItems", 2));
        g_settings.auctionBuyMinUpgradePercent = std::min<uint32>(500, sConfigMgr->GetOption<uint32>("CoaBots.Economy.AuctionBuyMinUpgradePercent", 10));
        g_settings.auctionBuyFromBots = sConfigMgr->GetOption<bool>("CoaBots.Economy.AuctionBuyFromBots", true);
        g_settings.shoppingMinSpareCopper = sConfigMgr->GetOption<uint32>("CoaBots.Economy.ShoppingMinSpareCopper", 200000);
        g_settings.shoppingIntervalMin = std::max<uint32>(10, sConfigMgr->GetOption<uint32>("CoaBots.Economy.ShoppingIntervalMin", 120));
        g_settings.auctionHourlyCapCopper = sConfigMgr->GetOption<uint32>("CoaBots.Economy.AuctionBuyHourlyCapCopper", 5000000);
        g_settings.auctionMaxVendorMultiple = sConfigMgr->GetOption<uint32>("CoaBots.Economy.AuctionBuyMaxVendorMultiple", 20);
        g_settings.listFromAnywhere = sConfigMgr->GetOption<bool>("CoaBots.Economy.ListFromAnywhere", false);
        g_settings.sellTradeGoods = sConfigMgr->GetOption<bool>("CoaBots.Economy.SellTradeGoods", false);
        g_settings.tradeGoodsKeep = sConfigMgr->GetOption<uint32>("CoaBots.Economy.TradeGoodsKeep", 0);
        g_settings.estimateMultiplier = std::max<uint32>(1, std::min<uint32>(20, sConfigMgr->GetOption<uint32>("CoaBots.Economy.EstimateMultiplier", 3)));
        g_settings.craftForMarket = sConfigMgr->GetOption<bool>("CoaBots.Economy.CraftForMarket", false);
        g_settings.craftForMarketMaxCasts = std::min<uint32>(50, sConfigMgr->GetOption<uint32>("CoaBots.Economy.CraftForMarketMaxCasts", 10));
        g_settings.craftForMarketMarginPercent = std::min<uint32>(1000, sConfigMgr->GetOption<uint32>("CoaBots.Economy.CraftForMarketMarginPercent", 25));
        g_settings.craftFromGuildBank = sConfigMgr->GetOption<bool>("CoaBots.Crafting.ReagentsFromGuildBank", false);
        g_settings.craftFromVendor = sConfigMgr->GetOption<bool>("CoaBots.Crafting.ReagentsFromVendor", false);
        g_settings.craftFromAuction = sConfigMgr->GetOption<bool>("CoaBots.Crafting.ReagentsFromAuction", false);
        g_settings.craftBudgetCopper = sConfigMgr->GetOption<uint32>("CoaBots.Crafting.ReagentBudgetCopper", 100000);
        g_settings.craftTimeoutMin = sConfigMgr->GetOption<uint32>("CoaBots.Crafting.OrderTimeoutMin", 120);
        g_settings.craftMaxCastFailures = sConfigMgr->GetOption<uint32>("CoaBots.Crafting.MaxCastFailures", 10);
    }

    void LoadProtected()
    {
        g_protected.clear();
        if (QueryResult result = CharacterDatabase.Query("SELECT item_entry FROM mod_coa_bot_protected_items"))
            do
                g_protected.insert((*result)[0].Get<uint32>());
            while (result->NextRow());
    }

    void EnsureTables()
    {
        if (g_tablesReady)
            return;
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS mod_coa_bot_trade_journal ("
            "  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
            "  at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "  bot_guid INT UNSIGNED NOT NULL,"
            "  bot_name VARCHAR(24) NOT NULL,"
            "  item_entry INT UNSIGNED NOT NULL,"
            "  item_count INT UNSIGNED NOT NULL,"
            "  verdict VARCHAR(8) NOT NULL,"
            "  reason VARCHAR(32) NOT NULL,"
            "  unit_copper INT UNSIGNED NOT NULL,"
            "  price_source VARCHAR(12) NOT NULL,"
            "  executed TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "  KEY idx_at (at),"
            "  KEY idx_bot (bot_guid)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS mod_coa_bot_protected_items ("
            "  item_entry INT UNSIGNED NOT NULL PRIMARY KEY"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS mod_coa_bot_gold_orders ("
            "  bot_guid INT UNSIGNED NOT NULL PRIMARY KEY,"
            "  guild_id INT UNSIGNED NOT NULL,"
            "  target_copper BIGINT UNSIGNED NOT NULL,"
            "  deposited_copper BIGINT UNSIGNED NOT NULL DEFAULT 0"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS mod_coa_bot_stock_limits ("
            "  guild_id INT UNSIGNED NOT NULL,"
            "  item_entry INT UNSIGNED NOT NULL,"
            "  keep_count INT UNSIGNED NOT NULL,"
            "  PRIMARY KEY (guild_id, item_entry)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");
        if (QueryResult limits = CharacterDatabase.Query("SELECT guild_id, item_entry, keep_count FROM mod_coa_bot_stock_limits"))
            do
                g_stockLimits[(*limits)[0].Get<uint32>()][(*limits)[1].Get<uint32>()] = (*limits)[2].Get<uint32>();
            while (limits->NextRow());
        LoadProtected();
        if (QueryResult result = CharacterDatabase.Query("SELECT bot_guid, guild_id, target_copper, deposited_copper FROM mod_coa_bot_gold_orders"))
            do
            {
                Field* f = (*result).Fetch();
                GoldOrder o;
                o.guildId = f[1].Get<uint32>();
                o.targetCopper = f[2].Get<uint64>();
                o.depositedCopper = f[3].Get<uint64>();
                g_goldOrders[ObjectGuid::Create<HighGuid::Player>(f[0].Get<uint32>())] = o;
            } while (result->NextRow());
        g_tablesReady = true;
    }

    void RefreshMarket(uint32 diff)
    {
        g_marketAgeMs += diff;
        if (g_marketAgeMs < MARKET_REFRESH_MS)
            return;
        g_marketAgeMs = 0;
        g_market.clear();
        g_listingCount.clear();
        for (AuctionHouseId house : { AuctionHouseId::Alliance, AuctionHouseId::Horde, AuctionHouseId::Neutral })
        {
            AuctionHouseObject* auctions = sAuctionMgr->GetAuctionsMapByHouseId(house);
            if (!auctions)
                continue;
            for (auto const& pair : auctions->GetAuctions())
            {
                AuctionEntry const* a = pair.second;
                if (a)
                    ++g_listingCount[a->owner.GetCounter()];
                if (!a || !a->itemCount || !a->buyout)
                    continue;
                uint32 const unit = std::max<uint32>(1, a->buyout / a->itemCount);
                auto [it, inserted] = g_market.try_emplace(a->item_template, unit);
                if (!inserted)
                    it->second = std::min(it->second, unit);
            }
        }
    }

    // What one item would fetch at the auction: just under the lowest buyout on offer, or - when nobody
    // sells it - a markup on the vendor price.
    uint32 AuctionUnitPrice(uint32 entry, ItemTemplate const* proto, char const*& source)
    {
        auto it = g_market.find(entry);
        if (it != g_market.end())
        {
            source = "market";
            return std::max<uint32>(1, uint32(uint64(it->second) * (100 - g_settings.undercutPercent) / 100));
        }
        source = "estimate";
        return uint32(std::min<uint64>(2000000, BotEconomyPolicy::EstimateUnitPrice(proto->SellPrice, proto->BuyPrice, g_settings.estimateMultiplier)));
    }

    template <typename F>
    void ForEachBagItem(Player* bot, F&& fn)
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (!BotGear::Archived(item))
                    fn(item);
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        {
            Bag* pBag = bot->GetBagByPos(bag);
            if (!pBag)
                continue;
            for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
                if (Item* item = pBag->GetItemByPos(j))
                    if (!BotGear::Archived(item))
                        fn(item);
        }
    }

    bool IsConsumableOfInterest(ItemTemplate const* proto)
    {
        if (proto->Class != ITEM_CLASS_CONSUMABLE)
            return false;
        return proto->SubClass == ITEM_SUBCLASS_POTION || proto->SubClass == ITEM_SUBCLASS_ELIXIR || proto->SubClass == ITEM_SUBCLASS_FLASK ||
            proto->SubClass == ITEM_SUBCLASS_FOOD || proto->SubClass == ITEM_SUBCLASS_BANDAGE;
    }

    // Gear the bot cannot wear, or that does not beat what it already wears in the slot (the same 5% margin the
    // upgrade pass uses). Gear that would fill an empty slot is not dead weight: the bot will put it on.
    bool IsDeadWeightGear(Player* bot, ItemTemplate const* proto, char const*& reason)
    {
        if (BotAI::IsProfessionTool(proto))
            return false;
        uint8 const slot = bot->FindEquipSlot(proto, NULL_SLOT, true);
        if (slot == NULL_SLOT)
        {
            reason = "unusable-gear";
            return true;
        }
        Item* current = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!current)
            return false;
        BotRole const role = BotAI::GetRole(bot->GetGUID());
        reason = "outdated-gear";
        return BotAI::ScoreItemForBot(bot, proto, role) <= BotAI::ScoreItemForBot(bot, current->GetTemplate(), role) * 1.05f;
    }

    // Auction when it can be listed and is worth it, a vendor otherwise.
    void Decide(Item const* item, ItemTemplate const* proto, bool qualityGate, BotEconomy::Line& line)
    {
        bool const canList = !item->IsSoulBound() && proto->Bonding != BIND_WHEN_PICKED_UP && item->CanBeTraded() && !item->IsConjuredConsumable();
        if (canList && (!qualityGate || proto->Quality >= g_settings.minAuctionQuality))
        {
            line.verdict = BotEconomy::Verdict::Auction;
            line.unitCopper = AuctionUnitPrice(line.entry, proto, line.priceSource);
            return;
        }
        line.verdict = BotEconomy::Verdict::Vendor;
        line.unitCopper = proto->SellPrice;
        line.priceSource = "vendor";
    }

    std::string QuoteSql(std::string const& s)
    {
        std::string out;
        for (char c : s)
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')
                out.push_back(c);
        return out;
    }

    std::string Money(uint64 copper)
    {
        return Acore::StringFormat("{}g {}s {}c", copper / 10000, (copper / 100) % 100, copper % 100);
    }

    void Journal(Player* bot, std::vector<BotEconomy::Line> const& lines)
    {
        for (BotEconomy::Line const& l : lines)
        {
            CharacterDatabase.Execute(Acore::StringFormat(
                "INSERT INTO mod_coa_bot_trade_journal (bot_guid, bot_name, item_entry, item_count, verdict, reason, unit_copper, price_source, executed) "
                "VALUES ({}, '{}', {}, {}, '{}', '{}', {}, '{}', 0)",
                bot->GetGUID().GetCounter(), QuoteSql(bot->GetName()), l.entry, l.count,
                l.verdict == BotEconomy::Verdict::Auction ? "auction" : "vendor", QuoteSql(l.reason), l.unitCopper, QuoteSql(l.priceSource)));
            ++g_totals.journalRows;
        }
    }

    // ---- Gold orders --------------------------------------------------------------------------------------------

    void SaveGoldOrder(ObjectGuid guid, GoldOrder const& o)
    {
        CharacterDatabase.Execute(Acore::StringFormat(
            "REPLACE INTO mod_coa_bot_gold_orders (bot_guid, guild_id, target_copper, deposited_copper) VALUES ({}, {}, {}, {})",
            guid.GetCounter(), o.guildId, o.targetCopper, o.depositedCopper));
    }

    void DeleteGoldOrder(ObjectGuid guid)
    {
        CharacterDatabase.Execute(Acore::StringFormat("DELETE FROM mod_coa_bot_gold_orders WHERE bot_guid = {}", guid.GetCounter()));
        g_goldOrders.erase(guid);
    }

    // ---- Trips to town -------------------------------------------------------------------------------------------

    struct Trip
    {
        uint32 mapId = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        uint32 faction = 0; // the auctioneer's faction: decides which auction house is used
        uint32 startedAt = 0;
        uint64 goalId = 0;
        bool flightTried = false;
        bool flightRequested = false; // a flight was asked for and has not finished yet
        bool vendorOnly = false;      // the nearest vendor, not the auction house: only what a vendor takes is sold
    };

    std::unordered_map<ObjectGuid, Trip> g_trips;
    std::unordered_set<ObjectGuid> g_wantsTrip;
    std::unordered_map<ObjectGuid, uint32> g_nextTripAt;
    uint64 g_goalSeed = 0xEC000000000ull;
    constexpr float ARRIVE_RADIUS = 30.0f;
    constexpr float TAXI_MIN_DISTANCE = 900.0f;
    constexpr uint32 TRIP_TIMEOUT_MS = 25 * 60 * 1000;

    struct TripTotals
    {
        uint32 trips = 0;
        uint32 listed = 0;
        uint32 soldToVendor = 0;
        uint64 earnedCopper = 0;
        uint64 guildCopper = 0;
        uint32 noDestination = 0;
        uint32 timedOut = 0;
    } g_tripTotals;

    // The trip outranks the walks of ordinary activities (a quest walk, grinding, gathering, fishing, strolling); a
    // claim left over from one of them would keep the bot from ever starting out.
    void ReleaseOrdinaryWalks(Player* bot)
    {
        for (MoveOwner owner : { MoveOwner::Ambient, MoveOwner::Grind, MoveOwner::Gather, MoveOwner::Fish, MoveOwner::Quest })
            BotMovement::ForceReleaseOwner(bot, owner);
    }

    bool IsFriendly(Player const* bot, Poi const& poi)
    {
        FactionTemplateEntry const* mine = bot->GetFactionTemplateEntry();
        FactionTemplateEntry const* theirs = poi.faction ? sFactionTemplateStore.LookupEntry(poi.faction) : nullptr;
        return !theirs || (mine && !mine->IsHostileTo(*theirs) && !theirs->IsHostileTo(*mine));
    }

    // The nearest friendly auctioneer on the bot's map.
    Poi const* NearestAuctioneer(Player* bot)
    {
        std::vector<Poi const*> found;
        BotWorldPoi::Query(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), 30000.0f, PoiKind::Auctioneer, found);
        Poi const* best = nullptr;
        float bestDist = 0.0f;
        for (Poi const* poi : found)
        {
            if (!poi->faction || !IsFriendly(bot, *poi) || !AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(poi->faction))
                continue;
            float const d = std::hypot(poi->x - bot->GetPositionX(), poi->y - bot->GetPositionY());
            if (!best || d < bestDist)
            {
                best = poi;
                bestDist = d;
            }
        }
        return best;
    }

    // The nearest friendly vendor on the bot's map. A vendor buys anything, so junk and surplus can be sold there; most of a map's vendors are far
    // closer than its auction house (the soak test saw trips of 17 000 yards).
    Poi const* NearestVendor(Player* bot)
    {
        std::vector<Poi const*> found;
        BotWorldPoi::Query(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), 30000.0f, PoiKind::Vendor, found);
        Poi const* best = nullptr;
        float bestDist = 0.0f;
        for (Poi const* poi : found)
        {
            if (!IsFriendly(bot, *poi))
                continue;
            float const d = std::hypot(poi->x - bot->GetPositionX(), poi->y - bot->GetPositionY());
            if (!best || d < bestDist)
            {
                best = poi;
                bestDist = d;
            }
        }
        return best;
    }

    // Listing price for one item: the plan's price with a random spread of a few percent either way so the market
    // looks alive, never below what a vendor would pay plus a little.
    uint32 ListingUnitPrice(BotEconomy::Line const& line, ItemTemplate const* proto)
    {
        uint32 const spread = urand(g_settings.spreadMin, g_settings.spreadMax);
        uint64 unit = line.unitCopper;
        unit = urand(0, 1) ? unit * (100 + spread) / 100 : unit * (100 - std::min<uint32>(spread, 90)) / 100;
        unit = std::max<uint64>(unit, uint64(proto->SellPrice) * 105 / 100);
        return uint32(std::max<uint64>(1, std::min<uint64>(unit, 2000000)));
    }

    bool ListOnAuction(Player* bot, Item* item, uint32 count, uint32 buyout, uint32 auctioneerFaction)
    {
        AuctionHouseEntry const* houseEntry = AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(auctioneerFaction);
        AuctionHouseObject* house = sAuctionMgr->GetAuctionsMap(auctioneerFaction);
        if (!houseEntry || !house || !count || count > item->GetCount() || !buyout)
            return false;
        if (sAuctionMgr->GetAItem(item->GetGUID()) || !item->CanBeTraded() || item->IsNotEmptyBag() || item->GetTemplate()->HasFlag(ITEM_FLAG_CONJURED) ||
            item->GetUInt32Value(ITEM_FIELD_DURATION))
            return false;

        uint32 const etime = g_settings.listingMinutes * MINUTE;
        uint32 const auctionTime = uint32(etime * sWorld->getRate(RATE_AUCTION_TIME));
        uint32 const deposit = sAuctionMgr->GetAuctionDeposit(houseEntry, etime, item, count);
        if (!bot->HasEnoughMoney(deposit))
            return false;

        AuctionHouseId houseId = AuctionHouseId::Neutral;
        if (!sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION))
            houseId = AuctionHouseId(houseEntry->houseId);

        Item* listed = item;
        if (item->GetCount() != count)
        {
            listed = item->CloneItem(count, bot);
            if (!listed)
                return false;
        }

        bot->ModifyMoney(-int32(deposit));
        AuctionEntry* ah = new AuctionEntry;
        ah->Id = sObjectMgr->GenerateAuctionID();
        ah->houseId = houseId;
        ah->owner = bot->GetGUID();
        // The auction house suggests the first bid; the seller only chooses the buyout, a little above it.
        ah->startbid = std::max<uint32>(1, uint32(uint64(buyout) * 85 / 100));
        ah->bidder = ObjectGuid::Empty;
        ah->bid = 0;
        ah->buyout = buyout;
        ah->expire_time = GameTime::GetGameTime().count() + auctionTime;
        ah->deposit = deposit;
        ah->auctionHouseEntry = houseEntry;
        ah->item_guid = listed->GetGUID();
        ah->item_template = listed->GetEntry();
        ah->itemCount = listed->GetCount();

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        sAuctionMgr->AddAItem(listed);
        house->AddAuction(ah);
        if (listed == item)
        {
            bot->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);
            item->DeleteFromInventoryDB(trans);
            item->SaveToDB(trans);
        }
        else
        {
            item->SetCount(item->GetCount() - count);
            item->SetState(ITEM_CHANGED, bot);
            bot->ItemRemovedQuestCheck(item->GetEntry(), count);
            listed->SaveToDB(trans);
        }
        ah->SaveToDB(trans);
        bot->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);
        ++g_listingCount[bot->GetGUID().GetCounter()];
        return true;
    }

    // Takes the money and items out of the bot's delivered mails that `wanted` accepts (never cash-on-delivery ones).
    // Returns the money collected.
    template <typename Wanted>
    uint64 CollectMail(Player* bot, Wanted&& wanted)
    {
        uint64 money = 0;
        time_t const now = GameTime::GetGameTime().count();
        std::vector<Mail*> mails;
        for (Mail* m : bot->GetMails())
            if (m && m->state != MAIL_STATE_DELETED && m->deliver_time <= now && !m->COD && wanted(*m))
                mails.push_back(m);
        if (mails.empty())
            return 0;

        bool changed = false;
        for (Mail* m : mails)
        {
            if (m->money && bot->ModifyMoney(m->money, false))
            {
                money += m->money;
                m->money = 0;
                m->state = MAIL_STATE_CHANGED;
                changed = true;
            }
            for (MailItemInfo const& info : std::vector<MailItemInfo>(m->items))
            {
                Item* it = bot->GetMItem(info.item_guid);
                if (!it)
                    continue;
                ItemPosCountVec dest;
                if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, it, false) != EQUIP_ERR_OK)
                    continue;
                m->RemoveItem(info.item_guid);
                m->removedItems.push_back(info.item_guid);
                m->state = MAIL_STATE_CHANGED;
                bot->RemoveMItem(it->GetGUID().GetCounter());
                it->SetState(ITEM_UNCHANGED);
                bot->MoveItemToInventory(dest, it, true);
                changed = true;
            }
            if (!m->money && m->items.empty())
            {
                m->state = MAIL_STATE_DELETED;
                changed = true;
            }
        }
        if (changed)
            bot->m_mailsUpdated = true; // the save at the end of the trip writes the mail, the items and the gold
        return money;
    }

    // Money and returned items from earlier auctions sit in the bot's mailbox; a player collects them at a mailbox,
    // a bot in town just takes them. Returns the money collected.
    uint64 CollectAuctionMail(Player* bot)
    {
        return CollectMail(bot, [](Mail const& m) { return m.messageType == MAIL_AUCTION; });
    }

    void JournalDone(Player* bot, BotEconomy::Line const& l, uint32 count, uint32 unitCopper, char const* verdict, char const* source)
    {
        CharacterDatabase.Execute(Acore::StringFormat(
            "INSERT INTO mod_coa_bot_trade_journal (bot_guid, bot_name, item_entry, item_count, verdict, reason, unit_copper, price_source, executed) "
            "VALUES ({}, '{}', {}, {}, '{}', '{}', {}, '{}', 1)",
            bot->GetGUID().GetCounter(), QuoteSql(bot->GetName()), l.entry, count, verdict, QuoteSql(l.reason), unitCopper, QuoteSql(source)));
    }

    // ---- Buying --------------------------------------------------------------------------------------------------

    struct BuyTotals
    {
        uint32 shoppingTrips = 0;
        uint32 gearBought = 0;
        uint64 gearCopper = 0;
        uint32 reagentStacksBought = 0;
        uint64 reagentAuctionCopper = 0;
        uint64 reagentVendorCopper = 0;
        uint32 reagentsFromGuildBank = 0;
        uint32 refusedOverCap = 0;
        uint32 craftedForMarket = 0;
    } g_buyTotals;

    uint32 g_capWindowStartMs = 0;
    uint64 g_capWindowSpent = 0;

    // All bots share one hourly purchase budget, so a large population cannot empty the auction houses.
    uint64 HourlyCapLeft()
    {
        if (!g_settings.auctionHourlyCapCopper)
            return BotEconomyPolicy::CapLeft(0, 0);
        uint32 const now = GameTime::GetGameTimeMS().count();
        if (!g_capWindowStartMs || now - g_capWindowStartMs >= 60u * 60 * 1000)
        {
            g_capWindowStartMs = std::max<uint32>(1, now);
            g_capWindowSpent = 0;
        }
        return BotEconomyPolicy::CapLeft(g_settings.auctionHourlyCapCopper, g_capWindowSpent);
    }

    bool WithinPriceCeiling(AuctionEntry const* a)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(a->item_template);
        return BotEconomyPolicy::WithinPriceCeiling(a->buyout, a->itemCount, proto ? proto->SellPrice : 0, g_settings.auctionMaxVendorMultiple);
    }

    std::unordered_map<ObjectGuid, uint32> g_nextShopAt;
    std::unordered_map<ObjectGuid, std::unordered_map<ObjectGuid, uint32>> g_purchases; // bot -> bought item -> kept until (game ms)
    constexpr uint32 PURCHASE_KEEP_MS = 6 * 60 * 60 * 1000;
    std::unordered_set<ObjectGuid> g_wantsShopping;

    uint64 SpareMoney(Player* bot)
    {
        return bot->GetMoney() > g_settings.moneyReserveCopper ? bot->GetMoney() - g_settings.moneyReserveCopper : 0;
    }

    // The auction house a bot trades at: the auctioneer's when it stands at one, otherwise its own side's.
    AuctionHouseObject* HouseFor(Player* bot, uint32 auctioneerFaction)
    {
        if (auctioneerFaction)
            return sAuctionMgr->GetAuctionsMap(auctioneerFaction);
        if (sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION))
            return sAuctionMgr->GetAuctionsMapByHouseId(AuctionHouseId::Neutral);
        return sAuctionMgr->GetAuctionsMapByHouseId(bot->GetTeamId() == TEAM_ALLIANCE ? AuctionHouseId::Alliance : AuctionHouseId::Horde);
    }

    bool OwnedByBot(ObjectGuid owner)
    {
        uint32 const accountId = sCharacterCache->GetCharacterAccountIdByGuid(owner);
        return accountId && BotMgr::IsBotAccountId(accountId);
    }

    bool MayBuy(Player* bot, AuctionEntry const* a)
    {
        if (!a || !a->buyout || !a->itemCount || a->owner == bot->GetGUID() || !sAuctionMgr->GetAItem(a->item_guid))
            return false;
        if (!WithinPriceCeiling(a))
            return false;
        return g_settings.auctionBuyFromBots || !OwnedByBot(a->owner);
    }

    // The same steps as a player's buyout (WorldSession::HandleAuctionPlaceBid): the seller is paid by mail, the bot gets
    // the item by mail and takes it out at once.
    bool Buyout(Player* bot, AuctionHouseObject* house, uint32 auctionId)
    {
        AuctionEntry* a = house ? house->GetAuction(auctionId) : nullptr;
        if (!MayBuy(bot, a))
            return false;
        ObjectGuid const itemGuid = a->item_guid;
        uint32 const cost = a->bidder == bot->GetGUID() ? a->buyout - a->bid : a->buyout;
        if (!bot->HasEnoughMoney(cost))
            return false;
        if (cost > HourlyCapLeft())
        {
            ++g_buyTotals.refusedOverCap;
            return false;
        }
        g_capWindowSpent += cost;

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        bot->ModifyMoney(-int32(cost));
        if (a->bidder && a->bidder != bot->GetGUID())
            sAuctionMgr->SendAuctionOutbiddedMail(a, a->buyout, bot, trans);
        a->bidder = bot->GetGUID();
        a->bid = a->buyout;
        sAuctionMgr->SendAuctionSalePendingMail(a, trans);
        sAuctionMgr->SendAuctionSuccessfulMail(a, trans);
        sAuctionMgr->SendAuctionWonMail(a, trans);
        sScriptMgr->OnAuctionSuccessful(house, a);

        uint32 const ownerLow = a->owner.GetCounter();
        a->DeleteFromDB(trans);
        sAuctionMgr->RemoveAItem(a->item_guid);
        house->RemoveAuction(a);
        bot->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);

        if (auto count = g_listingCount.find(ownerLow); count != g_listingCount.end() && count->second)
            --count->second;
        auto& purchases = g_purchases[bot->GetGUID()];
        uint32 const now = GameTime::GetGameTimeMS().count();
        for (auto it = purchases.begin(); it != purchases.end();)
            it = BotEconomyPolicy::StillBefore(now, it->second) ? std::next(it) : purchases.erase(it);
        purchases[itemGuid] = now + PURCHASE_KEEP_MS;
        CollectAuctionMail(bot);
        return true;
    }

    // Buys stacks of `entry`, cheapest per item first, until `count` are bought or the money runs out. Returns how many items came.
    uint32 BuyFromAuction(Player* bot, uint32 entry, uint32 count, uint64& budgetLeft, char const* reason)
    {
        AuctionHouseObject* house = HouseFor(bot, 0);
        if (!house || !count)
            return 0;

        struct Offer
        {
            uint32 id;
            uint32 items;
            uint32 buyout;
        };
        std::vector<Offer> offers;
        for (auto const& [id, a] : house->GetAuctions())
            if (a && a->item_template == entry && MayBuy(bot, a))
                offers.push_back({ id, a->itemCount, a->buyout });
        std::sort(offers.begin(), offers.end(), [](Offer const& x, Offer const& y) { return uint64(x.buyout) * y.items < uint64(y.buyout) * x.items; });

        uint32 bought = 0;
        for (Offer const& o : offers)
        {
            if (bought >= count)
                break;
            if (o.buyout > budgetLeft || o.buyout > SpareMoney(bot))
                continue;
            ItemPosCountVec dest;
            if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, o.items) != EQUIP_ERR_OK)
                continue;
            if (!Buyout(bot, house, o.id))
                continue;
            bought += o.items;
            budgetLeft -= o.buyout;
            ++g_buyTotals.reagentStacksBought;
            g_buyTotals.reagentAuctionCopper += o.buyout;
            BotEconomy::Line line;
            line.entry = entry;
            line.reason = reason;
            JournalDone(bot, line, o.items, o.buyout / std::max<uint32>(1, o.items), "buy", "auction");
        }
        return bought;
    }

    // Items some vendor sells for gold in unlimited supply; built on first use.
    std::unordered_set<uint32> const& VendorSoldItems()
    {
        static std::unordered_set<uint32> items;
        static bool loaded = false;
        if (!loaded)
        {
            loaded = true;
            if (QueryResult result = WorldDatabase.Query("SELECT DISTINCT item FROM npc_vendor WHERE item > 0 AND maxcount = 0 AND ExtendedCost = 0"))
                do
                    items.insert((*result)[0].Get<uint32>());
                while (result->NextRow());
        }
        return items;
    }

    uint32 BuyFromVendor(Player* bot, uint32 entry, uint32 count, uint64& budgetLeft, char const* reason)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!proto || !proto->BuyPrice || !count || !VendorSoldItems().count(entry))
            return 0;
        auto costOf = [&](uint32 n) { return BotEconomyPolicy::VendorCost(proto->BuyPrice, proto->BuyCount, n); };
        uint64 const money = std::min(budgetLeft, SpareMoney(bot));
        while (count && costOf(count) > money)
            --count;
        if (!count)
            return 0;
        ItemPosCountVec dest;
        if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, count) != EQUIP_ERR_OK)
            return 0;
        uint64 const cost = costOf(count);
        bot->ModifyMoney(-int64(cost));
        bot->StoreNewItem(dest, entry, true);
        budgetLeft -= cost;
        g_buyTotals.reagentVendorCopper += cost;
        BotEconomy::Line line;
        line.entry = entry;
        line.reason = reason;
        JournalDone(bot, line, count, uint32(cost / count), "buy", "vendor");
        return count;
    }

    struct GearPick
    {
        uint32 auctionId = 0;
        uint32 entry = 0;
        uint32 buyout = 0;
        uint8 slot = NULL_SLOT;
        float gain = 0.0f;
    };

    // The best upgrade on offer for each equipment slot, best gain first, every one within the shopping budget.
    std::vector<GearPick> FindGearUpgrades(Player* bot, AuctionHouseObject* house, uint64 budget)
    {
        std::vector<GearPick> picks;
        if (!house || !budget || BotGear::TrainingLocked(bot))
            return picks;
        BotRole const role = BotAI::GetRole(bot->GetGUID());
        std::unordered_map<uint8, GearPick> bestPerSlot;
        std::unordered_map<uint8, float> wornScore;
        for (auto const& [id, a] : house->GetAuctions())
        {
            if (!a || a->itemCount != 1 || a->buyout > budget || !MayBuy(bot, a))
                continue;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(a->item_template);
            if (!proto || (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON) || BotAI::IsProfessionTool(proto))
                continue;
            if (!BotGear::Allowed(bot, proto))
                continue;
            uint8 const slot = bot->FindEquipSlot(proto, NULL_SLOT, true);
            if (slot == NULL_SLOT || BotGear::PreserveSlot(bot, slot))
                continue;
            auto worn = wornScore.find(slot);
            if (worn == wornScore.end())
            {
                Item* current = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                worn = wornScore.emplace(slot, current ? BotAI::ScoreItemForBot(bot, current->GetTemplate(), role) : 0.0f).first;
            }
            float const score = BotAI::ScoreItemForBot(bot, proto, role);
            if (!BotEconomyPolicy::IsUpgrade(score, worn->second, g_settings.auctionBuyMinUpgradePercent))
                continue;
            float const gain = score - worn->second;
            GearPick& best = bestPerSlot[slot];
            if (!best.auctionId || gain > best.gain || (gain == best.gain && a->buyout < best.buyout))
                best = { id, a->item_template, a->buyout, slot, gain };
        }
        for (auto const& [slot, pick] : bestPerSlot)
            picks.push_back(pick);
        std::sort(picks.begin(), picks.end(), [](GearPick const& x, GearPick const& y) { return x.gain > y.gain; });
        return picks;
    }

    uint64 ShoppingBudget(Player* bot)
    {
        return std::min(SpareMoney(bot) * g_settings.auctionBuyBudgetPercent / 100, HourlyCapLeft());
    }

    // A bot with money to spare and an upgrade on offer is worth a trip to the auction house even when it has nothing to sell.
    bool WantsShopping(Player* bot, uint32 now)
    {
        if (!g_settings.auctionBuyGear || SpareMoney(bot) < g_settings.shoppingMinSpareCopper)
            return false;
        auto next = g_nextShopAt.find(bot->GetGUID());
        if (next != g_nextShopAt.end() && int32(now - next->second) < 0)
            return false;
        g_nextShopAt[bot->GetGUID()] = now + g_settings.shoppingIntervalMin * 60 * 1000;
        return !FindGearUpgrades(bot, HouseFor(bot, 0), ShoppingBudget(bot)).empty();
    }

    void ShopForGear(Player* bot, uint32 auctioneerFaction)
    {
        if (!g_settings.auctionBuyGear)
            return;
        AuctionHouseObject* house = HouseFor(bot, auctioneerFaction);
        uint64 budget = ShoppingBudget(bot);
        uint32 bought = 0;
        for (GearPick const& pick : FindGearUpgrades(bot, house, budget))
        {
            if (bought >= g_settings.auctionBuyMaxItems)
                break;
            if (pick.buyout > budget)
                continue;
            ItemPosCountVec dest;
            if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, pick.entry, 1) != EQUIP_ERR_OK)
                break;
            if (!Buyout(bot, house, pick.auctionId))
                continue;
            budget -= pick.buyout;
            ++bought;
            ++g_buyTotals.gearBought;
            g_buyTotals.gearCopper += pick.buyout;
            BotEconomy::Line line;
            line.entry = pick.entry;
            line.reason = "auction-upgrade";
            JournalDone(bot, line, 1, pick.buyout, "buy", "auction");
            LOG_DEBUG("module.coa-playerbots", "BotEconomy: '{}' bought item {} at the auction house for {}.", bot->GetName(), pick.entry, Money(pick.buyout));
        }
        if (bought)
            BotAI::MaintainEquipmentNow(bot);
        g_nextShopAt[bot->GetGUID()] = GameTime::GetGameTimeMS().count() + g_settings.shoppingIntervalMin * 60 * 1000;
    }

    // ---- Crafting for the market ---------------------------------------------------------------------------------

    uint64 ReagentValue(uint32 entry)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        uint64 value = proto ? proto->SellPrice : 0;
        if (auto market = g_market.find(entry); market != g_market.end())
            value = std::max<uint64>(value, market->second);
        return value;
    }

    struct MarketCraft
    {
        uint32 spellId = 0;
        uint32 product = 0;
        uint32 casts = 0;
        uint64 profitPerCast = 0;
    };

    // Recipes the bot knows whose reagents it carries (none of them held for an order or protected) and whose product
    // fetches CraftForMarketMarginPercent more at the auction house than the reagents are worth. Most profitable first.
    std::vector<MarketCraft> PlanMarketCrafts(Player* bot)
    {
        std::vector<MarketCraft> plan;
        uint32 gatherItem = 0;
        sBotMgr->GetGatherOrderItem(bot->GetGUID(), &gatherItem);
        for (auto const& [spellId, known] : bot->GetSpellMap())
        {
            if (!known || known->State == PLAYERSPELL_REMOVED || !known->Active)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info)
                continue;
            uint32 product = 0;
            uint32 perCast = 1;
            for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
                if (info->Effects[e].Effect == SPELL_EFFECT_CREATE_ITEM && info->Effects[e].ItemType)
                {
                    product = info->Effects[e].ItemType;
                    perCast = uint32(std::max<int32>(1, info->Effects[e].CalcValue(bot)));
                    break;
                }
            ItemTemplate const* proto = product ? sObjectMgr->GetItemTemplate(product) : nullptr;
            if (!proto || proto->Bonding == BIND_WHEN_PICKED_UP || proto->Class == ITEM_CLASS_QUEST || proto->HasFlag(ITEM_FLAG_CONJURED))
                continue;

            uint32 casts = std::numeric_limits<uint32>::max();
            uint64 reagentValue = 0;
            bool usable = false;
            for (uint8 i = 0; i < MAX_SPELL_REAGENTS && casts; ++i)
            {
                if (info->Reagent[i] <= 0 || !info->ReagentCount[i])
                    continue;
                uint32 const entry = uint32(info->Reagent[i]);
                if (entry == gatherItem || g_protected.count(entry) || BotEconomy::IsReservedReagent(bot, entry))
                {
                    casts = 0;
                    break;
                }
                usable = true;
                casts = std::min(casts, bot->GetItemCount(entry, false) / info->ReagentCount[i]);
                reagentValue += ReagentValue(entry) * info->ReagentCount[i];
            }
            if (!usable || !casts)
                continue;
            char const* source = "";
            uint64 const productValue = uint64(AuctionUnitPrice(product, proto, source)) * perCast;
            if (!BotEconomyPolicy::CraftPays(productValue, reagentValue, g_settings.craftForMarketMarginPercent))
                continue;
            plan.push_back({ spellId, product, casts, productValue - reagentValue });
        }
        std::sort(plan.begin(), plan.end(), [](MarketCraft const& x, MarketCraft const& y) { return x.profitPerCast > y.profitPerCast; });
        return plan;
    }

    void CraftForMarket(Player* bot, uint32 auctioneerFaction, uint32& listingsLeft)
    {
        if (!g_settings.craftForMarket || !listingsLeft || BotGear::TrainingLocked(bot) || bot->IsNonMeleeSpellCast(false))
            return;
        uint32 castsLeft = g_settings.craftForMarketMaxCasts;
        std::vector<uint32> products;
        for (MarketCraft const& craft : PlanMarketCrafts(bot))
        {
            for (uint32 c = 0; c < craft.casts && castsLeft; ++c)
            {
                uint32 const before = bot->GetItemCount(craft.product, false);
                SpellCastResult const result = bot->CastSpell(bot, craft.spellId,
                    TriggerCastFlags(TRIGGERED_CAST_DIRECTLY | TRIGGERED_IGNORE_GCD | TRIGGERED_IGNORE_SPELL_AND_CATEGORY_CD));
                if (result != SPELL_CAST_OK || bot->GetItemCount(craft.product, false) <= before)
                    break;
                --castsLeft;
                ++g_buyTotals.craftedForMarket;
                if (std::find(products.begin(), products.end(), craft.product) == products.end())
                    products.push_back(craft.product);
            }
            if (!castsLeft)
                break;
        }

        for (uint32 product : products)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(product);
            if (!proto)
                continue;
            BotEconomy::Line line;
            line.entry = product;
            line.reason = "crafted-for-market";
            line.unitCopper = AuctionUnitPrice(product, proto, line.priceSource);
            std::vector<Item*> stacks;
            ForEachBagItem(bot, [&](Item* it) { if (it->GetEntry() == product) stacks.push_back(it); });
            for (Item* it : stacks)
            {
                if (!listingsLeft)
                    return;
                uint32 const n = it->GetCount();
                uint32 const unit = ListingUnitPrice(line, proto);
                if (uint64(unit) * n < g_settings.minListingCopper || !ListOnAuction(bot, it, n, unit * n, auctioneerFaction))
                    break;
                --listingsLeft;
                ++g_tripTotals.listed;
                JournalDone(bot, line, n, unit, "auction", line.priceSource);
            }
        }
    }

    // ---- Supplies ------------------------------------------------------------------------------------------------

    // The consumable (food: spell category 11, drink: 59) in the bot's bags that fits its level best, or null.
    Item* BestSupplyInBags(Player* bot, bool food)
    {
        Item* best = nullptr;
        uint32 bestLevel = 0;
        ForEachBagItem(bot, [&](Item* item)
        {
            ItemTemplate const* proto = item->GetTemplate();
            if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE || proto->SubClass != ITEM_SUBCLASS_FOOD)
                return;
            if (proto->Spells[0].SpellCategory != (food ? 11u : 59u) || proto->RequiredLevel > bot->GetLevel())
                return;
            if (!best || proto->RequiredLevel > bestLevel)
            {
                best = item;
                bestLevel = proto->RequiredLevel;
            }
        });
        return best;
    }

    // How many items of the level range a bot may still use (RequiredLevel within the outdated margin).
    uint32 SupplyCount(Player* bot, bool food)
    {
        uint32 count = 0;
        ForEachBagItem(bot, [&](Item* item)
        {
            ItemTemplate const* proto = item->GetTemplate();
            if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE || proto->SubClass != ITEM_SUBCLASS_FOOD)
                return;
            if (proto->Spells[0].SpellCategory != (food ? 11u : 59u) || proto->RequiredLevel > bot->GetLevel())
                return;
            if (proto->RequiredLevel + g_settings.outdatedBelowLevels < bot->GetLevel())
                return;
            count += item->GetCount();
        });
        return count;
    }

    bool UsesMana(Player* bot)
    {
        return bot->getPowerType() == POWER_MANA;
    }

    // What a trip to town should buy: the best food (and drink for a mana user) of the bot's level up to the target
    // count, within the money the bot may spend today.
    void BuySupplies(Player* bot)
    {
        if (!g_settings.consumeSupplies)
            return;
        uint32 const now = GameTime::GetGameTimeMS().count();
        Spending& spent = g_spent[bot->GetGUID()];
        if (!spent.dayStartedAt || now - spent.dayStartedAt > 24u * 60 * 60 * 1000)
        {
            spent.dayStartedAt = now ? now : 1;
            spent.copper = 0;
        }

        for (bool food : { true, false })
        {
            if (!food && !UsesMana(bot))
                continue;
            uint32 const entry = food ? BotProgression::PickFood(bot->GetLevel()) : BotProgression::PickDrink(bot->GetLevel());
            ItemTemplate const* proto = entry ? sObjectMgr->GetItemTemplate(entry) : nullptr;
            if (!proto)
                continue;
            uint32 const have = SupplyCount(bot, food);
            if (have >= g_settings.supplyTarget)
                continue;
            uint32 want = g_settings.supplyTarget - have;
            uint32 const unit = std::max<uint32>(1, proto->BuyPrice / std::max<uint32>(1, proto->BuyCount));
            uint64 const money = bot->GetMoney() > g_settings.moneyReserveCopper ? bot->GetMoney() - g_settings.moneyReserveCopper : 0;
            uint64 const budget = spent.copper < g_settings.dailyBudgetCopper ? g_settings.dailyBudgetCopper - spent.copper : 0;
            want = uint32(std::min<uint64>(want, std::min(money, budget) / unit));
            if (!want)
                continue;
            ItemPosCountVec dest;
            if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, want) != EQUIP_ERR_OK)
                continue;
            uint64 const cost = uint64(unit) * want;
            bot->ModifyMoney(-int64(cost));
            bot->StoreNewItem(dest, entry, true);
            spent.copper += cost;
            g_boughtCopper += cost;
            BotEconomy::Line line;
            line.entry = entry;
            line.reason = food ? "supplies-food" : "supplies-drink";
            JournalDone(bot, line, want, unit, "buy", "vendor");
        }

        // Healing and (for a mana user) mana potions for the fight: the best tier the level allows, up to the target count, within the same daily budget.
        for (bool healing : { true, false })
        {
            if (!healing && !UsesMana(bot))
                continue;
            uint32 const entry = healing ? BotProgression::PickHealingPotion(bot->GetLevel()) : BotProgression::PickManaPotion(bot->GetLevel());
            ItemTemplate const* proto = entry ? sObjectMgr->GetItemTemplate(entry) : nullptr;
            if (!proto)
                continue;
            uint32 const have = bot->GetItemCount(entry);
            if (have >= g_settings.potionTarget)
                continue;
            uint32 want = g_settings.potionTarget - have;
            uint32 const unit = std::max<uint32>(1, proto->BuyPrice / std::max<uint32>(1, proto->BuyCount));
            uint64 const money = bot->GetMoney() > g_settings.moneyReserveCopper ? bot->GetMoney() - g_settings.moneyReserveCopper : 0;
            uint64 const budget = spent.copper < g_settings.dailyBudgetCopper ? g_settings.dailyBudgetCopper - spent.copper : 0;
            want = uint32(std::min<uint64>(want, std::min(money, budget) / unit));
            if (!want)
                continue;
            ItemPosCountVec dest;
            if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, want) != EQUIP_ERR_OK)
                continue;
            uint64 const cost = uint64(unit) * want;
            bot->ModifyMoney(-int64(cost));
            bot->StoreNewItem(dest, entry, true);
            spent.copper += cost;
            g_boughtCopper += cost;
            BotEconomy::Line line;
            line.entry = entry;
            line.reason = healing ? "supplies-health-potion" : "supplies-mana-potion";
            JournalDone(bot, line, want, unit, "buy", "vendor");
        }
        g_needsSupplies.erase(bot->GetGUID());
    }

    // Whether a bot that eats and drinks has run out and can afford more: worth a trip to town.
    bool NeedsSupplies(Player* bot)
    {
        if (!g_settings.consumeSupplies)
            return false;
        if (SupplyCount(bot, true) == 0)
            return true;
        return UsesMana(bot) && SupplyCount(bot, false) == 0;
    }

    // What a bot does once it has reached the auctioneers: collect what earlier auctions earned, list and sell, and
    // pay the guild its share.
    // ---- Listing -------------------------------------------------------------------------------------------------

    // Materials the bot's guild bank is short of (an officer's stock limit above what the bank holds): item -> how many.
    std::unordered_map<uint32, uint32> GuildStockShortfall(Player* bot)
    {
        std::unordered_map<uint32, uint32> shortfall;
        auto limits = bot && bot->GetGuildId() ? g_stockLimits.find(bot->GetGuildId()) : g_stockLimits.end();
        if (limits == g_stockLimits.end() || limits->second.empty())
            return shortfall;
        auto const banked = sBotMgr->GuildBankTradeGoods(bot->GetGuildId());
        for (auto const& [entry, keep] : limits->second)
        {
            auto const have = banked.find(entry);
            uint32 const inBank = have == banked.end() ? 0 : have->second;
            if (inBank < keep)
                shortfall[entry] = keep - inBank;
        }
        return shortfall;
    }

    // The guild bank comes first: materials it is short of are paid in before anything is put up for sale.
    void TopUpGuildStock(Player* bot)
    {
        for (auto const& [entry, missing] : GuildStockShortfall(bot))
        {
            uint32 const carried = std::min(bot->GetItemCount(entry, false), missing);
            if (carried && !BotEconomy::IsReservedReagent(bot, entry))
                sBotMgr->GuildDepositItem(bot->GetGUID().GetCounter(), entry, carried, nullptr);
        }
    }

    // Lists the stacks of one auction line at the house of `auctioneerFaction`; `remaining` is what is still unlisted.
    BotEconomyPolicy::ListingStop ListLine(Player* bot, BotEconomy::Line const& line, ItemTemplate const* proto, uint32 auctioneerFaction,
        uint32& listingsLeft, uint32& remaining)
    {
        using BotEconomyPolicy::ListingStop;
        std::vector<Item*> stacks;
        ForEachBagItem(bot, [&](Item* it) { if (it->GetEntry() == line.entry) stacks.push_back(it); });
        for (Item* it : stacks)
        {
            if (!remaining)
                return ListingStop::Done;
            if (!listingsLeft)
                return ListingStop::NoListingsLeft;
            uint32 const n = std::min(remaining, it->GetCount());
            uint32 const unit = ListingUnitPrice(line, proto);
            // When a vendor would pay more than the auction nets after its 5% cut, sell to the vendor instead.
            if (uint64(unit) * n < g_settings.minListingCopper || uint64(unit) * n * 95 / 100 <= uint64(proto->SellPrice) * n)
                return ListingStop::VendorBetter;
            if (!ListOnAuction(bot, it, n, unit * n, auctioneerFaction))
                return ListingStop::Failed;
            remaining -= n;
            --listingsLeft;
            ++g_tripTotals.listed;
            JournalDone(bot, line, n, unit, "auction", line.priceSource);
        }
        return remaining ? ListingStop::Failed : ListingStop::Done;
    }

    uint32 ListingsLeft(Player* bot)
    {
        uint32 const listingsNow = g_listingCount[bot->GetGUID().GetCounter()];
        return g_settings.maxListings > listingsNow ? g_settings.maxListings - listingsNow : 0;
    }

    uint32 g_listedFromAnywhere = 0;

    // CoaBots.Economy.ListFromAnywhere: a bot collects its auction mail and lists its auction lines at its own side's
    // auction house from wherever it is, the way a player's mailbox and a quick trip would. Vendor lines wait for a trip.
    bool ListFromAnywhere(Player* bot)
    {
        if (!g_settings.listFromAnywhere || !BotEconomy::IsLive() || g_trips.count(bot->GetGUID()))
            return false;
        uint32 listingsLeft = ListingsLeft(bot);
        uint64 const collected = CollectAuctionMail(bot);
        TopUpGuildStock(bot);
        uint32 listed = 0;
        for (BotEconomy::Line const& line : BotEconomy::PlanSale(bot))
        {
            if (line.verdict != BotEconomy::Verdict::Auction || !listingsLeft)
                continue;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(line.entry);
            if (!proto)
                continue;
            uint32 remaining = line.count;
            uint32 const before = listingsLeft;
            ListLine(bot, line, proto, bot->GetFaction(), listingsLeft, remaining);
            listed += before - listingsLeft;
        }
        g_listedFromAnywhere += listed;
        if (listed || collected || bot->m_mailsUpdated)
            bot->SaveToDB(false, false);
        return listed > 0;
    }

    void SellInTown(Player* bot, uint32 auctioneerFaction, bool vendorOnly)
    {
        uint64 gained = vendorOnly ? 0 : CollectAuctionMail(bot);
        bool const bagsFull = bot->GetFreeInventorySpace() <= g_settings.bagsFullFreeSlots;

        uint32 listingsLeft = ListingsLeft(bot);

        TopUpGuildStock(bot);
        if (!vendorOnly)
            CraftForMarket(bot, auctioneerFaction, listingsLeft);

        std::vector<BotEconomy::Line> plan = BotEconomy::PlanSale(bot);
        for (BotEconomy::Line const& line : plan)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(line.entry);
            if (!proto)
                continue;
            uint32 remaining = line.count;

            // At a vendor the lines meant for the auction house wait for a later trip, unless the bags are full: then they go for the vendor price.
            if (vendorOnly && line.verdict == BotEconomy::Verdict::Auction && !bagsFull)
                continue;

            if (line.verdict == BotEconomy::Verdict::Auction && !vendorOnly)
            {
                BotEconomyPolicy::ListingStop const stop = ListLine(bot, line, proto, auctioneerFaction, listingsLeft, remaining);
                // A full auction list or a refused deposit keeps the rest for the next listing instead of selling it off cheap.
                if (!BotEconomyPolicy::VendorTheRest(stop, bagsFull))
                    continue;
            }

            // Whatever is left (or was meant for a vendor) is sold at the vendor price.
            if (remaining && proto->SellPrice)
            {
                uint32 const have = bot->GetItemCount(line.entry);
                uint32 const n = std::min(remaining, have);
                if (n)
                    bot->DestroyItemCount(line.entry, n, true);
                if (n && bot->GetItemCount(line.entry) + n == have)
                {
                    uint64 const copper = uint64(proto->SellPrice) * n;
                    bot->ModifyMoney(int64(copper));
                    gained += copper;
                    ++g_tripTotals.soldToVendor;
                    JournalDone(bot, line, n, proto->SellPrice, "vendor", "vendor");
                }
            }
        }

        BuySupplies(bot);

        // The guild's small share, leaving the bot its own reserve.
        if (gained && g_settings.guildSharePercent && bot->GetGuild() && !g_goldOrders.count(bot->GetGUID()))
        {
            uint64 share = gained * g_settings.guildSharePercent / 100;
            uint64 const spare = bot->GetMoney() > g_settings.moneyReserveCopper ? bot->GetMoney() - g_settings.moneyReserveCopper : 0;
            share = std::min(share, spare);
            if (share)
            {
                sBotMgr->GuildDepositMoney(bot->GetGUID().GetCounter(), uint32(std::min<uint64>(share, 4000000000ull)), nullptr);
                g_tripTotals.guildCopper += share;
            }
        }
        if (!vendorOnly)
            ShopForGear(bot, auctioneerFaction);
        g_tripTotals.earnedCopper += gained;
        bot->SaveToDB(false, false);
    }
}

namespace BotEconomy
{
    void ProcessGoldOrders();
    void BalanceStock();
    void EndStockRun(ObjectGuid bot);

    std::vector<Line> PlanSale(Player* bot)
    {
        std::vector<Line> out;
        if (!bot)
            return out;

        uint8 const botLevel = bot->GetLevel();

        // Consumables are judged per item over all stacks together.
        std::unordered_map<uint32, uint32> consumableTotal;
        ForEachBagItem(bot, [&](Item* item)
        {
            ItemTemplate const* proto = item->GetTemplate();
            if (proto && IsConsumableOfInterest(proto))
                consumableTotal[item->GetEntry()] += item->GetCount();
        });

        std::unordered_set<uint32> consumableDone;
        std::unordered_map<uint32, uint32> tradeTotal;
        std::unordered_map<uint32, uint32> guildShortfall;
        uint32 gatherItem = 0;
        // A bot on a guild gather order banks its materials for the guild (BotMgr::MakeRoomForGatherOrder): it sells none.
        bool const sellsTradeGoods = g_settings.sellTradeGoods && !sBotMgr->GetGatherOrderItem(bot->GetGUID(), &gatherItem);
        if (sellsTradeGoods)
        {
            ForEachBagItem(bot, [&](Item* item)
            {
                if (item->GetTemplate() && item->GetTemplate()->Class == ITEM_CLASS_TRADE_GOODS)
                    tradeTotal[item->GetEntry()] += item->GetCount();
            });
            guildShortfall = GuildStockShortfall(bot);
        }
        auto add = [&](Line line)
        {
            if (line.verdict == Verdict::Keep || !line.count)
                return;
            for (Line& existing : out)
                if (existing.entry == line.entry && existing.verdict == line.verdict && std::string(existing.reason) == line.reason)
                {
                    existing.count += line.count;
                    return;
                }
            out.push_back(std::move(line));
        };

        ForEachBagItem(bot, [&](Item* item)
        {
            ItemTemplate const* proto = item->GetTemplate();
            if (!proto)
                return;
            uint32 const entry = item->GetEntry();
            if (entry == HEARTHSTONE_ENTRY || g_protected.count(entry) || BotEconomy::IsReservedItem(bot, item))
                return;
            // A resource taken out of the guild bank to be sold (see the Stock limits).
            if (proto->Class == ITEM_CLASS_TRADE_GOODS)
            {
                auto carried = g_stockCarry.find(bot->GetGUID());
                if (carried != g_stockCarry.end() && carried->second.count(entry) && proto->SellPrice)
                {
                    Line stock;
                    stock.entry = entry;
                    stock.name = proto->Name1;
                    stock.count = item->GetCount();
                    stock.reason = "stock-surplus";
                    Decide(item, proto, false, stock);
                    add(std::move(stock));
                    return;
                }
                // Gathered and looted materials (herbs, ore, leather, cloth, meat...), above what the bot keeps and what
                // its guild bank is short of. Crafting-order reagents stay (IsReservedItem above).
                if (sellsTradeGoods && !BotAI::IsProfessionTool(proto) && !bot->HasQuestForItem(entry))
                {
                    if (!consumableDone.insert(entry).second)
                        return;
                    auto const shortfall = guildShortfall.find(entry);
                    uint32 const forSale = BotEconomyPolicy::TradeGoodsForSale(tradeTotal[entry], g_settings.tradeGoodsKeep,
                        shortfall == guildShortfall.end() ? 0 : shortfall->second);
                    if (!forSale)
                        return;
                    Line goods;
                    goods.entry = entry;
                    goods.name = proto->Name1;
                    goods.count = forSale;
                    goods.reason = "trade-goods";
                    Decide(item, proto, false, goods);
                    add(std::move(goods));
                    return;
                }
            }
            if (proto->Class == ITEM_CLASS_QUEST || proto->StartQuest || proto->Class == ITEM_CLASS_KEY || proto->Class == ITEM_CLASS_CONTAINER ||
                proto->Class == ITEM_CLASS_TRADE_GOODS || proto->Class == ITEM_CLASS_RECIPE || proto->Class == ITEM_CLASS_PROJECTILE ||
                proto->Class == ITEM_CLASS_QUIVER || proto->Class == ITEM_CLASS_GLYPH)
                return;
            if (proto->Bonding == BIND_QUEST_ITEM || proto->Bonding == BIND_QUEST_ITEM1)
                return;

            Line line;
            line.entry = entry;
            line.name = proto->Name1;

            // Grey junk.
            if (proto->Quality == ITEM_QUALITY_POOR)
            {
                if (proto->SellPrice == 0)
                    return;
                line.count = item->GetCount();
                line.reason = "junk";
                line.verdict = Verdict::Vendor;
                line.unitCopper = proto->SellPrice;
                line.priceSource = "vendor";
                add(std::move(line));
                return;
            }

            // Gear.
            if (proto->Class == ITEM_CLASS_ARMOR || proto->Class == ITEM_CLASS_WEAPON)
            {
                char const* reason = "";
                if (!IsDeadWeightGear(bot, proto, reason))
                    return;
                line.count = item->GetCount();
                line.reason = reason;
                Decide(item, proto, true, line);
                add(std::move(line));
                return;
            }

            // Potions, elixirs, flasks, food, bandages.
            if (IsConsumableOfInterest(proto))
            {
                if (!consumableDone.insert(entry).second)
                    return; // handled with the first stack
                uint32 const total = consumableTotal[entry];
                bool const outdated = proto->RequiredLevel + g_settings.outdatedBelowLevels < botLevel;
                bool const tooHigh = proto->RequiredLevel > botLevel;
                if (tooHigh)
                    return; // the bot will grow into it
                uint32 const keep = outdated ? 0 : g_settings.keepConsumables;
                if (total <= keep)
                    return;
                line.count = total - keep;
                line.reason = outdated ? "outdated-consumable" : "surplus-consumable";
                Decide(item, proto, false, line); // potions and food are worth listing whatever their quality
                add(std::move(line));
                return;
            }

            // Gems and other finds worth a listing.
            if ((proto->Class == ITEM_CLASS_GEM || proto->Class == ITEM_CLASS_MISC) && proto->Quality >= g_settings.minAuctionQuality)
            {
                line.count = item->GetCount();
                line.reason = "found-valuable";
                Decide(item, proto, true, line);
                if (line.verdict == Verdict::Auction)
                    add(std::move(line));
            }
        });

        // Biggest first: it is what a trip to town is for.
        std::sort(out.begin(), out.end(), [](Line const& a, Line const& b) { return a.TotalCopper() > b.TotalCopper(); });
        return out;
    }

    void Update(uint32 diff, std::vector<WorldSession*> const& sessions)
    {
        RefreshSettings(diff);
        if (!g_settings.enable || sessions.empty())
            return;
        EnsureTables();
        RefreshMarket(diff);
        g_goldCheckAgeMs += diff;
        if (g_goldCheckAgeMs >= GOLD_CHECK_MS)
        {
            g_goldCheckAgeMs = 0;
            ProcessGoldOrders();
        }
        g_stockCheckAgeMs += diff;
        if (g_stockCheckAgeMs >= STOCK_CHECK_MS)
        {
            g_stockCheckAgeMs = 0;
            BalanceStock();
        }

        // Tick every waiting bot down, then look at the ones that are due, a few per tick.
        g_pruneAgeMs += diff;
        if (g_pruneAgeMs > 3600000)
        {
            g_pruneAgeMs = 0;
            CharacterDatabase.Execute(Acore::StringFormat("DELETE FROM mod_coa_bot_trade_journal WHERE at < NOW() - INTERVAL {} DAY", g_settings.journalDays));
            if (g_lastPlan.size() > sessions.size() * 2)
            {
                std::unordered_set<ObjectGuid> online;
                for (WorldSession* s : sessions)
                    if (Player* p = s->GetPlayer())
                        online.insert(p->GetGUID());
                for (auto it = g_lastPlan.begin(); it != g_lastPlan.end();)
                    it = online.count(it->first) ? std::next(it) : g_lastPlan.erase(it);
            }
            if (g_nextScanAt.size() > sessions.size() * 2)
            {
                std::unordered_set<ObjectGuid> online;
                for (WorldSession* s : sessions)
                    if (Player* p = s->GetPlayer())
                        online.insert(p->GetGUID());
                for (auto it = g_nextScanAt.begin(); it != g_nextScanAt.end();)
                    it = online.count(it->first) ? std::next(it) : g_nextScanAt.erase(it);
            }
        }

        // Look at a slice of the bots each tick; those that are due get planned, a few per tick.
        uint32 const now = GameTime::GetGameTimeMS().count();
        uint32 looked = 0;
        size_t const n = sessions.size();
        size_t const slice = std::min<size_t>(n, 64);
        for (size_t i = 0; i < slice && looked < g_settings.botsPerTick; ++i)
        {
            WorldSession* session = sessions[(g_rotation + i) % n];
            Player* bot = session->GetPlayer();
            if (!bot || CoaTraining::Owns(bot->GetGUID()) || !bot->IsInWorld() || bot->IsInCombat() || !bot->IsAlive())
                continue;
            if (bot->GetGroup() || bot->InBattleground() || (bot->GetMap() && bot->GetMap()->IsDungeon()))
                continue;

            auto [it, isNew] = g_nextScanAt.try_emplace(bot->GetGUID(), now);
            if (!isNew && int32(now - it->second) < 0)
                continue;
            // Spread the next looks out so a thousand bots are not all planned at the same moment.
            it->second = now + g_settings.scanIntervalSec * 1000 + urand(0, g_settings.scanIntervalSec * 200);
            ++looked;
            ++g_totals.botsScanned;

            if (ListFromAnywhere(bot))
                g_lastPlan.erase(bot->GetGUID());
            std::vector<Line> plan = PlanSale(bot);
            uint64 total = 0;
            for (Line const& l : plan)
                total += l.TotalCopper();
            // A bot working on a gold order goes to town for a smaller load: the sooner its finds are money, the sooner they are in the bank.
            uint64 const minTrip = g_goldOrders.count(bot->GetGUID()) ? std::min<uint64>(g_settings.minTripCopper, 5000) : g_settings.minTripCopper;
            bool const bagsFull = total > 0 && bot->GetFreeInventorySpace() <= g_settings.bagsFullFreeSlots;
            bool const worthSelling = total >= minTrip || bagsFull || g_needsSupplies.count(bot->GetGUID()) || NeedsSupplies(bot);
            bool const shopping = !worthSelling && !g_settings.dryRun && !g_trips.count(bot->GetGUID()) && WantsShopping(bot, now);
            if (!worthSelling && !shopping)
                continue;
            if (shopping)
                g_wantsShopping.insert(bot->GetGUID());

            uint64 fingerprint = 1469598103934665603ull;
            for (Line const& l : plan)
                for (uint64 v : { uint64(l.entry), uint64(l.count), uint64(l.verdict) })
                    fingerprint = (fingerprint ^ v) * 1099511628211ull;
            if (!g_settings.dryRun && !g_trips.count(bot->GetGUID()))
                g_wantsTrip.insert(bot->GetGUID());
            auto [last, firstPlan] = g_lastPlan.try_emplace(bot->GetGUID(), fingerprint);
            if (!firstPlan && last->second == fingerprint)
                continue; // nothing new since this bot was last written down
            last->second = fingerprint;

            if (!plan.empty())
                ++g_totals.botsWithSales;
            for (Line const& l : plan)
                (l.verdict == Verdict::Auction ? g_totals.auctionCopper : g_totals.vendorCopper) += l.TotalCopper();
            Journal(bot, plan);
            LOG_DEBUG("module.coa-playerbots", "BotEconomy: '{}' would sell {} stack(s) worth about {} (plan only).", bot->GetName(), plan.size(), Money(total));
        }
        g_rotation = (g_rotation + slice) % std::max<size_t>(1, n);
    }

    // Guild master or an officer (rank 0 or 1).
    bool IsGuildLeader(Player* player)
    {
        Guild* guild = player ? sGuildMgr->GetGuildById(player->GetGuildId()) : nullptr;
        Guild::Member const* member = guild ? guild->GetMember(player->GetGUID()) : nullptr;
        return member && member->GetRankId() <= 1;
    }

    void EndStockRun(ObjectGuid bot)
    {
        g_stockCarry.erase(bot);
        for (auto it = g_stockRuns.begin(); it != g_stockRuns.end();)
            it = it->second == bot ? g_stockRuns.erase(it) : std::next(it);
    }

    // Once a minute, for each guild with limits that is not already running a surplus out: the most valuable surplus is
    // withdrawn by a free bot that has room for it, and that bot sets off for town.
    void BalanceStock()
    {
        uint32 const now = GameTime::GetGameTimeMS().count();
        for (auto const& [guildId, limits] : g_stockLimits)
        {
            if (limits.empty() || g_stockRuns.count(guildId))
                continue;
            Guild* guild = sGuildMgr->GetGuildById(guildId);
            if (!guild)
                continue;

            auto counts = sBotMgr->GuildBankTradeGoods(guildId);
            uint32 bestEntry = 0;
            uint32 bestSurplus = 0;
            uint64 bestValue = 0;
            for (auto const& [entry, keep] : limits)
            {
                auto have = counts.find(entry);
                if (have == counts.end() || have->second <= keep)
                    continue;
                uint32 const surplus = have->second - keep;
                if (surplus < std::max<uint32>(10, keep / 10))
                    continue;
                auto blocked = g_stockBlockedUntil.find((uint64(guildId) << 32) | entry);
                if (blocked != g_stockBlockedUntil.end() && int32(now - blocked->second) < 0)
                    continue;
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
                uint64 const value = uint64(surplus) * std::max<uint32>(1, proto ? proto->SellPrice : 1);
                if (value > bestValue)
                {
                    bestValue = value;
                    bestEntry = entry;
                    bestSurplus = surplus;
                }
            }
            if (!bestEntry)
                continue;

            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(bestEntry);
            if (!proto)
                continue;
            Player* chosen = nullptr;
            for (Player* bot : sBotMgr->GetOnlineBots())
            {
                if (bot->GetGuildId() != guildId || bot->GetGroup() || !bot->IsAlive() || bot->IsInCombat() || g_trips.count(bot->GetGUID()) ||
                    g_goldOrders.count(bot->GetGUID()) || sBotMgr->GetGuildGatherOrder(bot->GetGUID()) || bot->GetFreeInventorySpace() < 6)
                    continue;
                chosen = bot;
                break;
            }
            if (!chosen)
                continue;

            uint32 const room = chosen->GetFreeInventorySpace() * std::max<uint32>(1, proto->GetMaxStackSize()) * 8 / 10;
            uint32 const take = std::min(bestSurplus, std::max<uint32>(1, room));
            uint32 const got = sBotMgr->GuildWithdrawItem(chosen->GetGUID().GetCounter(), bestEntry, take, nullptr);
            if (!got)
            {
                g_stockBlockedUntil[(uint64(guildId) << 32) | bestEntry] = now + 30 * 60 * 1000;
                continue;
            }
            g_stockCarry[chosen->GetGUID()].insert(bestEntry);
            g_stockRuns[guildId] = chosen->GetGUID();
            g_nextTripAt.erase(chosen->GetGUID());
            g_wantsTrip.insert(chosen->GetGUID());
            LOG_DEBUG("module.coa-playerbots", "BotEconomy: '{}' took {}x {} out of the guild bank to sell.", chosen->GetName(), got, proto->Name1);
        }
    }

    std::vector<std::string> GetEconomyLines(Player* requester)
    {
        std::vector<std::string> lines;
        if (!requester)
            return lines;
        static std::unordered_map<ObjectGuid, uint32> lastAskedAt;
        uint32 const now = GameTime::GetGameTimeMS().count();
        if (auto asked = lastAskedAt.find(requester->GetGUID()); asked != lastAskedAt.end() && now - asked->second < 3000)
            return lines;
        lastAskedAt[requester->GetGUID()] = now;
        EnsureTables();
        lines.push_back(Acore::StringFormat("ECONSTATE:{},{},{},{},{}", g_settings.enable ? 1 : 0, IsLive() ? 1 : 0, g_settings.auctionBuyGear ? 1 : 0,
            CraftSourcingEnabled() ? 1 : 0, g_settings.auctionHourlyCapCopper ? int64(HourlyCapLeft()) : int64(-1)));

        constexpr size_t MAX_BOTS = 60;
        std::vector<Player*> bots;
        for (Player* bot : sBotMgr->GetOnlineBots())
        {
            if (bots.size() >= MAX_BOTS)
                break;
            bool const guildMate = requester->GetGuildId() && bot->GetGuildId() == requester->GetGuildId();
            bool const groupMate = requester->GetGroup() && bot->GetGroup() == requester->GetGroup();
            if (guildMate || groupMate)
                bots.push_back(bot);
        }
        if (bots.empty())
        {
            lines.push_back("ECONEND");
            return lines;
        }

        std::string guids;
        for (Player* bot : bots)
            guids += (guids.empty() ? "" : ",") + std::to_string(bot->GetGUID().GetCounter());
        std::unordered_map<uint32, std::pair<uint64, uint64>> flows;
        if (QueryResult result = CharacterDatabase.Query(Acore::StringFormat(
                "SELECT bot_guid, SUM(IF(verdict = 'buy', 0, unit_copper * item_count)), SUM(IF(verdict = 'buy', unit_copper * item_count, 0)) "
                "FROM mod_coa_bot_trade_journal WHERE executed = 1 AND at > NOW() - INTERVAL 1 DAY AND bot_guid IN ({}) GROUP BY bot_guid", guids)))
            do
                flows[(*result)[0].Get<uint32>()] = { (*result)[1].Get<uint64>(), (*result)[2].Get<uint64>() };
            while (result->NextRow());

        constexpr size_t BUDGET = 200;
        auto chunked = [&](std::string const& verb, std::vector<std::string> const& pieces)
        {
            std::string body;
            for (std::string const& piece : pieces)
            {
                if (!body.empty() && body.size() + 1 + piece.size() > BUDGET)
                {
                    lines.push_back(verb + ":" + body);
                    body.clear();
                }
                body += (body.empty() ? "" : "|") + piece;
            }
            if (!body.empty())
                lines.push_back(verb + ":" + body);
        };

        std::vector<std::string> botPieces;
        for (Player* bot : bots)
        {
            auto const flow = flows[bot->GetGUID().GetCounter()];
            botPieces.push_back(Acore::StringFormat("{},{},{},{},{}", bot->GetGUID().GetCounter(), CoaBotWireText(bot->GetName(), 24, ":|,"),
                bot->GetMoney(), flow.first, flow.second));
        }
        chunked("ECONBOT", botPieces);

        std::vector<std::string> logPieces;
        if (QueryResult result = CharacterDatabase.Query(Acore::StringFormat(
                "SELECT bot_name, item_entry, item_count, verdict, unit_copper, TIMESTAMPDIFF(MINUTE, at, NOW()) FROM mod_coa_bot_trade_journal "
                "WHERE executed = 1 AND bot_guid IN ({}) ORDER BY id DESC LIMIT 15", guids)))
            do
            {
                Field* f = result->Fetch();
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(f[1].Get<uint32>());
                logPieces.push_back(Acore::StringFormat("{},{},{},{},{},{}", CoaBotWireText(f[0].Get<std::string>(), 24, ":|,"),
                    CoaBotWireText(proto ? proto->Name1 : std::to_string(f[1].Get<uint32>()), 60, ":|,"), f[2].Get<uint32>(),
                    CoaBotWireText(f[3].Get<std::string>(), 8, ":|,"), f[4].Get<uint32>(), f[5].Get<int64>()));
            } while (result->NextRow());
        chunked("ECONLOG", logPieces);
        lines.push_back("ECONEND");
        return lines;
    }

    std::vector<std::string> GetStockLines(Player* requester)
    {
        std::vector<std::string> lines;
        Guild* guild = requester ? sGuildMgr->GetGuildById(requester->GetGuildId()) : nullptr;
        if (!guild)
            return lines;
        EnsureTables();

        auto counts = sBotMgr->GuildBankTradeGoods(guild->GetId());
        auto const limitsIt = g_stockLimits.find(guild->GetId());
        struct Row
        {
            uint32 entry;
            uint32 count;
            uint32 limit;
            std::string name;
        };
        std::vector<Row> rows;
        for (auto const& [entry, count] : counts)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
            if (!proto)
                continue;
            uint32 limit = 0;
            if (limitsIt != g_stockLimits.end())
                if (auto l = limitsIt->second.find(entry); l != limitsIt->second.end())
                    limit = l->second;
            std::string name;
            name = CoaBotWireText(proto->Name1, 100, ":|,"); // one row can never outgrow a STOCK line
            rows.push_back({ entry, count, limit, name });
        }
        std::sort(rows.begin(), rows.end(), [](Row const& x, Row const& y) { return x.name < y.name; });

        constexpr size_t BUDGET = 200;
        std::string body;
        auto flush = [&]()
        {
            if (!body.empty())
                lines.push_back("STOCK:" + body);
            body.clear();
        };
        for (Row const& r : rows)
        {
            std::string const piece = std::to_string(r.entry) + "," + std::to_string(r.count) + "," + std::to_string(r.limit) + "," + r.name;
            if (!body.empty() && body.size() + 1 + piece.size() > BUDGET)
                flush();
            if (!body.empty())
                body += "|";
            body += piece;
        }
        flush();
        return lines;
    }

    void SetStockLimit(Player* requester, uint32 itemEntry, int64 keep, ChatHandler* handler)
    {
        if (!requester || !itemEntry)
            return;
        EnsureTables();
        uint32 const guildId = requester->GetGuildId();
        if (!guildId || (!handler && !IsGuildLeader(requester)))
            return;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
        if (!proto || proto->Class != ITEM_CLASS_TRADE_GOODS)
        {
            if (handler)
                handler->SendSysMessage("Limits can be set for resources (trade goods) only.");
            return;
        }
        if (keep < 0)
        {
            g_stockLimits[guildId].erase(itemEntry);
            CharacterDatabase.Execute(Acore::StringFormat("DELETE FROM mod_coa_bot_stock_limits WHERE guild_id = {} AND item_entry = {}", guildId, itemEntry));
            if (handler)
                handler->PSendSysMessage("Limit of {} removed.", proto->Name1);
            return;
        }
        uint32 const value = uint32(std::min<int64>(keep, 1000000));
        g_stockLimits[guildId][itemEntry] = value;
        g_stockBlockedUntil.erase((uint64(guildId) << 32) | itemEntry);
        CharacterDatabase.Execute(Acore::StringFormat("REPLACE INTO mod_coa_bot_stock_limits (guild_id, item_entry, keep_count) VALUES ({}, {}, {})", guildId, itemEntry, value));
        if (handler)
            handler->PSendSysMessage("The guild bank keeps {} of {}; the surplus is sold by the bots.", value, proto->Name1);
    }

    void ProcessGoldOrders()
    {
        std::vector<ObjectGuid> guids;
        guids.reserve(g_goldOrders.size());
        for (auto const& entry : g_goldOrders)
            guids.push_back(entry.first);
        for (ObjectGuid const& guid : guids)
        {
            Player* bot = ObjectAccessor::FindPlayer(guid);
            if (!bot)
                continue; // offline: the order waits
            GoldOrder& o = g_goldOrders[guid];
            if (!bot->GetGuild() || bot->GetGuildId() != o.guildId)
            {
                DeleteGoldOrder(guid); // the bot left the guild
                continue;
            }
            uint64 const money = bot->GetMoney();
            uint64 const spare = money > g_settings.moneyReserveCopper ? money - g_settings.moneyReserveCopper : 0;
            if (spare < GOLD_MIN_DEPOSIT_COPPER)
                continue;
            uint64 const remaining = o.targetCopper > o.depositedCopper ? o.targetCopper - o.depositedCopper : 0;
            uint64 const amount = std::min<uint64>({ spare, remaining, 4000000000ull });
            if (amount)
            {
                sBotMgr->GuildDepositMoney(guid.GetCounter(), uint32(amount), nullptr);
                o.depositedCopper += amount;
                g_tripTotals.guildCopper += amount;
            }
            if (o.depositedCopper >= o.targetCopper)
            {
                LOG_INFO("module.coa-playerbots", "BotEconomy: '{}' finished a gold order of {} for guild {}.", bot->GetName(), Money(o.targetCopper), o.guildId);
                DeleteGoldOrder(guid);
            }
            else
                SaveGoldOrder(guid, o);
        }
    }

    void PlaceGoldOrder(Player* requester, uint32 goldAmount, ChatHandler* handler)
    {
        if (!requester)
            return;
        EnsureTables();
        uint32 const guildId = requester->GetGuildId();
        auto say = [&](std::string const& text)
        {
            if (handler)
                handler->SendSysMessage(text);
        };
        if (!guildId)
        {
            say("Gold orders need a guild: the requester is not in one.");
            return;
        }
        if (!handler)
        {
            // From a player (the addon): guild master or an officer only.
            Guild* guild = sGuildMgr->GetGuildById(guildId);
            Guild::Member const* member = guild ? guild->GetMember(requester->GetGUID()) : nullptr;
            if (!member || member->GetRankId() > 1)
                return;
        }

        if (goldAmount == 0)
        {
            std::vector<ObjectGuid> mine;
            for (auto const& [guid, o] : g_goldOrders)
                if (o.guildId == guildId)
                    mine.push_back(guid);
            for (ObjectGuid const& guid : mine)
                DeleteGoldOrder(guid);
            say(Acore::StringFormat("Cancelled {} gold order(s).", mine.size()));
            return;
        }

        Player* chosen = nullptr;
        for (Player* bot : sBotMgr->GetOnlineBots())
        {
            if (bot->GetGuildId() != guildId || bot->GetGroup() || g_goldOrders.count(bot->GetGUID()))
                continue;
            if (!sBotMgr->GetGuildGatherOrder(bot->GetGUID()))
            {
                chosen = bot;
                break;
            }
            if (!chosen)
                chosen = bot;
        }
        if (!chosen)
        {
            say("No free online guild-mate bot found for a gold order.");
            return;
        }
        GoldOrder order;
        order.guildId = guildId;
        order.targetCopper = uint64(std::min<uint32>(goldAmount, 100000)) * 10000;
        g_goldOrders[chosen->GetGUID()] = order;
        SaveGoldOrder(chosen->GetGUID(), order);
        say(Acore::StringFormat("{} will earn {} for the guild.", chosen->GetName(), Money(order.targetCopper)));
    }

    std::string GoldOrderTask(Player* bot)
    {
        if (!bot)
            return std::string();
        auto it = g_goldOrders.find(bot->GetGUID());
        if (it == g_goldOrders.end())
            return std::string();
        GoldOrder const& o = it->second;
        uint64 const target = std::max<uint64>(1, o.targetCopper);
        return Acore::StringFormat("earning gold for the guild ({}/{} g - {}%)", o.depositedCopper / 10000, target / 10000, std::min<uint64>(100, o.depositedCopper * 100 / target));
    }

    bool GetGoldOrderInfo(Player* bot, uint64& outTargetCopper, uint64& outDepositedCopper)
    {
        if (!bot)
            return false;
        auto it = g_goldOrders.find(bot->GetGUID());
        if (it == g_goldOrders.end())
            return false;
        outTargetCopper = it->second.targetCopper;
        outDepositedCopper = it->second.depositedCopper;
        return true;
    }

    void StartRestAura(Player* bot, bool food)
    {
        uint32 const spell = food ? 433 : 431;
        if (!g_settings.consumeSupplies || !g_settings.enable)
        {
            bot->CastSpell(bot, spell, true);
            return;
        }
        uint32 const now = GameTime::GetGameTimeMS().count();
        uint64 const key = uint64(bot->GetGUID().GetCounter()) * 2 + (food ? 1 : 0);
        auto wait = g_noSupplyUntil.find(key);
        if (wait != g_noSupplyUntil.end() && int32(now - wait->second) < 0)
            return;
        Item* item = BestSupplyInBags(bot, food);
        if (!item)
        {
            g_needsSupplies.insert(bot->GetGUID()); // rests the slow way until it has bought something
            g_noSupplyUntil[key] = now + 5000;
            return;
        }
        bot->DestroyItemCount(item->GetEntry(), 1, true);
        bot->CastSpell(bot, spell, true);
    }

    bool UpdateTrip(Player* bot, uint32 /*diff*/)
    {
        if (!g_settings.enable || g_settings.dryRun || !bot)
            return false;

        ObjectGuid const guid = bot->GetGUID();
        uint32 const now = GameTime::GetGameTimeMS().count();

        // A bot in a group, a dungeon or a battleground belongs to that: it does not walk off to town (confirmed in the soak test: the
        // members of dungeon teams were sent to the auction house in the middle of a run).
        if (bot->GetGroup() || bot->InBattleground() || bot->InArena() || (bot->GetMap() && bot->GetMap()->IsDungeon()))
        {
            if (g_trips.erase(guid))
            {
                BotMovement::ForceReleaseOwner(bot, MoveOwner::Travel);
                EndStockRun(guid);
            }
            g_wantsTrip.erase(guid);
            g_wantsShopping.erase(guid);
            return false;
        }

        auto trip = g_trips.find(guid);
        if (trip == g_trips.end())
        {
            if (!g_wantsTrip.count(guid) || g_trips.size() >= g_settings.maxTrips || !bot->IsAlive() || bot->IsInFlight())
                return false;
            auto cooldown = g_nextTripAt.find(guid);
            if (cooldown != g_nextTripAt.end() && int32(now - cooldown->second) < 0)
                return false;
            g_wantsTrip.erase(guid);
            bool const shopping = g_wantsShopping.erase(guid) > 0;
            // The auction house only for a load worth the walk (or a shopping visit); otherwise (or when it is too far) the nearest vendor, which is usually close.
            Poi const* auctioneer = NearestAuctioneer(bot);
            Poi const* vendor = NearestVendor(bot);
            uint64 auctionCopper = 0;
            for (BotEconomy::Line const& l : BotEconomy::PlanSale(bot))
                if (l.verdict == BotEconomy::Verdict::Auction)
                    auctionCopper += l.TotalCopper();
            bool const auctionWorth = auctioneer && (auctionCopper >= g_settings.minTripCopper || shopping) &&
                std::hypot(auctioneer->x - bot->GetPositionX(), auctioneer->y - bot->GetPositionY()) <= g_settings.maxAuctionTripYards;
            Poi const* poi = auctionWorth ? auctioneer : (vendor ? vendor : auctioneer);
            bool const vendorOnly = poi && poi == vendor && !auctionWorth;
            if (!poi)
            {
                ++g_tripTotals.noDestination;
                g_nextTripAt[guid] = now + 20 * 60 * 1000;
                return false;
            }
            Trip t;
            t.mapId = bot->GetMapId();
            t.x = poi->x;
            t.y = poi->y;
            t.z = poi->z;
            t.faction = poi->faction;
            t.vendorOnly = vendorOnly;
            t.startedAt = now;
            t.goalId = ++g_goalSeed;
            trip = g_trips.emplace(guid, t).first;
            ++g_tripTotals.trips;
            if (shopping && auctionWorth)
                ++g_buyTotals.shoppingTrips;
            ReleaseOrdinaryWalks(bot);
            LOG_DEBUG("module.coa-playerbots", "BotEconomy: '{}' sets off for the auctioneers ({:.0f}, {:.0f}).", bot->GetName(), t.x, t.y);
        }

        Trip& t = trip->second;
        auto finish = [&](bool timedOut)
        {
            BotMovement::ForceReleaseOwner(bot, MoveOwner::Travel);
            g_nextTripAt[guid] = now + g_settings.tripCooldownMin * 60 * 1000 + urand(0, 5 * 60 * 1000);
            if (timedOut)
                ++g_tripTotals.timedOut;
            g_trips.erase(guid);
            EndStockRun(guid);
        };

        if (!bot->IsAlive() || bot->GetMapId() != t.mapId || int32(now - t.startedAt) > int32(TRIP_TIMEOUT_MS))
        {
            finish(bot->IsAlive());
            return false;
        }

        // The ambient layer drives the flight (walk to the flight master, take off, land) once asked; until it is done
        // the bot is left to it. Any other ambient errand (sitting at an inn, strolling) gives way to the trip.
        if (bot->IsInFlight())
            return false;
        if (BotWorldBehavior::HasActiveErrand(guid))
        {
            if (t.flightRequested)
                return false;
            BotWorldBehavior::Forget(guid);
            BotMovement::ForceReleaseOwner(bot, MoveOwner::Ambient);
        }
        else
            t.flightRequested = false;

        float const dist = std::hypot(bot->GetPositionX() - t.x, bot->GetPositionY() - t.y);
        if (dist <= ARRIVE_RADIUS)
        {
            if (bot->IsMounted())
                BotMovement::RequestDismount(bot, MoveOwner::Travel, DismountReason::Arrival);
            SellInTown(bot, t.faction, t.vendorOnly);
            finish(false);
            return false;
        }

        if (!t.flightTried && dist > TAXI_MIN_DISTANCE)
        {
            t.flightTried = true;
            if (BotWorldBehavior::RequestTravel(bot, t.mapId, t.x, t.y, t.z, false))
            {
                t.flightRequested = true;
                BotMovement::ForceReleaseOwner(bot, MoveOwner::Travel);
                return true;
            }
        }

        NavStatus const status = BotMovement::Navigate(bot, MoveOwner::Travel, t.goalId, t.x, t.y, t.z, ARRIVE_RADIUS * 0.7f);
        if (status == NavStatus::Stuck)
        {
            finish(false);
            return false;
        }
        if (status == NavStatus::Blocked)
            ReleaseOrdinaryWalks(bot);
        return true;
    }

    void HandleCommand(ChatHandler* handler, std::string const& args)
    {
        EnsureTables();
        std::vector<std::string> tok;
        {
            std::string cur;
            for (char c : args)
            {
                if (c == ' ')
                {
                    if (!cur.empty())
                        tok.push_back(cur), cur.clear();
                }
                else
                    cur.push_back(c);
            }
            if (!cur.empty())
                tok.push_back(cur);
        }
        std::string const sub = tok.empty() ? "status" : tok[0];

        if (sub == "status")
        {
            handler->PSendSysMessage("Economy: {}, {}. Looked at {} bots, {} with something to sell; journal rows written: {}.",
                g_settings.enable ? "on" : "off - set CoaBots.Economy.Enable = 1", g_settings.dryRun ? "plan only (CoaBots.Economy.DryRun = 1)" : "bots trade for real",
                g_totals.botsScanned, g_totals.botsWithSales, g_totals.journalRows);
            handler->PSendSysMessage("Stock: {} guild(s) with limits, {} surplus run(s) now.", g_stockLimits.size(), g_stockRuns.size());
            handler->PSendSysMessage("Trips: {} bots on their way, {} waiting; trips made {}; listed {} auction(s); {} vendor sale(s); earned {}, paid to guilds {}; no auctioneer on the map: {}; gave up: {}.",
                g_trips.size(), g_wantsTrip.size(), g_tripTotals.trips, g_tripTotals.listed, g_tripTotals.soldToVendor, Money(g_tripTotals.earnedCopper),
                Money(g_tripTotals.guildCopper), g_tripTotals.noDestination, g_tripTotals.timedOut);
            handler->PSendSysMessage("Would sell to vendors: {}; to the auction: {}. Auctions known to the market cache: {} items. Protected items: {}.",
                Money(g_totals.vendorCopper), Money(g_totals.auctionCopper), g_market.size(), g_protected.size());
            handler->PSendSysMessage("Buying: gear upgrades {} (CoaBots.Economy.AuctionBuyGear); shopping trips {}; upgrades bought {} for {}.",
                g_settings.auctionBuyGear ? "on" : "off", g_buyTotals.shoppingTrips, g_buyTotals.gearBought, Money(g_buyTotals.gearCopper));
            handler->PSendSysMessage("Hourly auction cap: {} left of {}; purchases refused over the cap: {}; price ceiling {}x vendor price.",
                g_settings.auctionHourlyCapCopper ? Money(HourlyCapLeft()) : std::string("no cap"), Money(g_settings.auctionHourlyCapCopper),
                g_buyTotals.refusedOverCap, g_settings.auctionMaxVendorMultiple);
            handler->PSendSysMessage("Crafting reagents: guild bank {}, vendors {}, auction {}; taken from guild banks {} item(s); bought {} auction stack(s) for {}, vendor reagents for {}.",
                g_settings.craftFromGuildBank ? "on" : "off", g_settings.craftFromVendor ? "on" : "off", g_settings.craftFromAuction ? "on" : "off",
                g_buyTotals.reagentsFromGuildBank, g_buyTotals.reagentStacksBought, Money(g_buyTotals.reagentAuctionCopper), Money(g_buyTotals.reagentVendorCopper));
            handler->PSendSysMessage("Crafting for the market {} (CoaBots.Economy.CraftForMarket): {} item(s) crafted.", g_settings.craftForMarket ? "on" : "off",
                g_buyTotals.craftedForMarket);
            handler->PSendSysMessage("Listing from anywhere {} (ListFromAnywhere): {} listing(s); trade goods to the auction {} (SellTradeGoods); auctions run {} min.",
                g_settings.listFromAnywhere ? "on" : "off", g_listedFromAnywhere, g_settings.sellTradeGoods ? "on" : "off", g_settings.listingMinutes);
            return;
        }

        if (sub == "goldorder" && tok.size() >= 3)
        {
            Player* requester = ObjectAccessor::FindPlayerByLowGUID(uint32(std::strtoul(tok[1].c_str(), nullptr, 10)));
            if (!requester)
            {
                handler->PSendSysMessage("No online player with guid {}.", tok[1]);
                return;
            }
            PlaceGoldOrder(requester, uint32(std::strtoul(tok[2].c_str(), nullptr, 10)), handler);
            return;
        }

        if (sub == "stock" && tok.size() >= 2)
        {
            Player* requester = ObjectAccessor::FindPlayerByLowGUID(uint32(std::strtoul(tok[1].c_str(), nullptr, 10)));
            if (!requester)
            {
                handler->PSendSysMessage("No online player with guid {}.", tok[1]);
                return;
            }
            std::vector<std::string> lines = GetStockLines(requester);
            handler->PSendSysMessage("{} reply line(s) for the guild bank:", lines.size());
            for (std::string const& l : lines)
                handler->SendSysMessage(l);
            return;
        }

        if (sub == "stocklimit" && tok.size() >= 4)
        {
            Player* requester = ObjectAccessor::FindPlayerByLowGUID(uint32(std::strtoul(tok[1].c_str(), nullptr, 10)));
            if (!requester)
            {
                handler->PSendSysMessage("No online player with guid {}.", tok[1]);
                return;
            }
            SetStockLimit(requester, uint32(std::strtoul(tok[2].c_str(), nullptr, 10)), std::strtoll(tok[3].c_str(), nullptr, 10), handler);
            return;
        }

        if (sub == "trips")
        {
            uint32 const now = GameTime::GetGameTimeMS().count();
            if (g_trips.empty())
                handler->SendSysMessage("No bot is on a trip to town right now.");
            for (auto const& [guid, t] : g_trips)
            {
                Player* bot = ObjectAccessor::FindPlayer(guid);
                if (!bot)
                    continue;
                handler->PSendSysMessage("  {} (guid {}): {:.0f} yd from the auctioneers, {} s on the way{}{}{}", bot->GetName(), guid.GetCounter(),
                    std::hypot(bot->GetPositionX() - t.x, bot->GetPositionY() - t.y), (now - t.startedAt) / 1000, bot->IsInFlight() ? ", flying" : "",
                    bot->IsMounted() ? ", mounted" : "", BotWorldBehavior::HasActiveErrand(guid) ? ", doing another errand first" : "");
            }
            return;
        }

        if (sub == "shop" && tok.size() >= 2)
        {
            Player* bot = sBotMgr->FindBotPlayer(uint32(std::strtoul(tok[1].c_str(), nullptr, 10)));
            if (!bot)
            {
                handler->PSendSysMessage("BotMgr: no online bot with guid {}.", tok[1]);
                return;
            }
            uint64 const budget = ShoppingBudget(bot);
            std::vector<GearPick> picks = FindGearUpgrades(bot, HouseFor(bot, 0), budget);
            handler->PSendSysMessage("{} may spend {} on one visit; {} slot(s) have an upgrade on offer:", bot->GetName(), Money(budget), picks.size());
            for (GearPick const& p : picks)
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(p.entry);
                handler->PSendSysMessage("  slot {}: {} for {} (+{:.1f} score)", uint32(p.slot), proto ? proto->Name1 : "?", Money(p.buyout), p.gain);
            }
            return;
        }

        if (sub == "crafts" && tok.size() >= 2)
        {
            Player* bot = sBotMgr->FindBotPlayer(uint32(std::strtoul(tok[1].c_str(), nullptr, 10)));
            if (!bot)
            {
                handler->PSendSysMessage("BotMgr: no online bot with guid {}.", tok[1]);
                return;
            }
            std::vector<MarketCraft> plan = PlanMarketCrafts(bot);
            handler->PSendSysMessage("{} could craft {} profitable recipe(s) from its own materials:", bot->GetName(), plan.size());
            for (MarketCraft const& c : plan)
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(c.product);
                handler->PSendSysMessage("  {} x{} (spell {}), {} profit per craft", proto ? proto->Name1 : "?", c.casts, c.spellId, Money(c.profitPerCast));
            }
            return;
        }

        if (sub == "plan" && tok.size() >= 2)
        {
            Player* bot = sBotMgr->FindBotPlayer(uint32(std::strtoul(tok[1].c_str(), nullptr, 10)));
            if (!bot)
            {
                handler->PSendSysMessage("BotMgr: no online bot with guid {}.", tok[1]);
                return;
            }
            g_marketAgeMs = MARKET_REFRESH_MS; // the plan below should see current prices
            RefreshMarket(0);
            std::vector<Line> plan = PlanSale(bot);
            handler->PSendSysMessage("{} would sell {} line(s):", bot->GetName(), plan.size());
            uint64 total = 0;
            for (Line const& l : plan)
            {
                total += l.TotalCopper();
                handler->PSendSysMessage("  {} x{} -> {} ({}), {} each ({}), {}", l.name, l.count, l.verdict == Verdict::Auction ? "auction" : "vendor", l.reason,
                    Money(l.unitCopper), l.priceSource, Money(l.TotalCopper()));
            }
            handler->PSendSysMessage("Total: {}.", Money(total));
            return;
        }

        if ((sub == "protect" || sub == "unprotect") && tok.size() >= 2)
        {
            uint32 const entry = uint32(std::strtoul(tok[1].c_str(), nullptr, 10));
            if (!entry || !sObjectMgr->GetItemTemplate(entry))
            {
                handler->PSendSysMessage("No item with id {}.", tok[1]);
                return;
            }
            if (sub == "protect")
            {
                CharacterDatabase.DirectExecute(Acore::StringFormat("INSERT IGNORE INTO mod_coa_bot_protected_items (item_entry) VALUES ({})", entry));
                g_protected.insert(entry);
                handler->PSendSysMessage("Item {} is protected: bots will never sell it.", entry);
            }
            else
            {
                CharacterDatabase.DirectExecute(Acore::StringFormat("DELETE FROM mod_coa_bot_protected_items WHERE item_entry = {}", entry));
                g_protected.erase(entry);
                handler->PSendSysMessage("Item {} is no longer protected.", entry);
            }
            return;
        }

        if (sub == "journal")
        {
            uint32 n = tok.size() >= 2 ? std::min<uint32>(50, uint32(std::strtoul(tok[1].c_str(), nullptr, 10))) : 15;
            if (!n)
                n = 15;
            QueryResult result = CharacterDatabase.Query(Acore::StringFormat(
                "SELECT at, bot_name, item_entry, item_count, verdict, reason, unit_copper, executed FROM mod_coa_bot_trade_journal ORDER BY id DESC LIMIT {}", n));
            if (!result)
            {
                handler->SendSysMessage("The journal is empty.");
                return;
            }
            do
            {
                Field* f = result->Fetch();
                handler->PSendSysMessage("{} {}: item {} x{} -> {} ({}), {} each{}", f[0].Get<std::string>(), f[1].Get<std::string>(), f[2].Get<uint32>(),
                    f[3].Get<uint32>(), f[4].Get<std::string>(), f[5].Get<std::string>(), Money(f[6].Get<uint32>()), f[7].Get<uint8>() ? "" : " (plan)");
            } while (result->NextRow());
            return;
        }

        handler->SendSysMessage("Usage: .botcmd economy [status | trips | goldorder <player guid> <gold> | stock <player guid> | stocklimit <player guid> <item> <keep, -1 clears> | plan <bot guid> | shop <bot guid> | crafts <bot guid> | journal [n] | protect <item id> | unprotect <item id>]");
    }

    bool IsProtectedItem(uint32 itemEntry)
    {
        return g_protected.count(itemEntry) != 0;
    }

    bool IsLive()
    {
        return g_settings.enable && !g_settings.dryRun;
    }

    bool HasSaleTrip(Player const* bot)
    {
        if (!bot || !IsLive())
            return false;
        ObjectGuid const guid = bot->GetGUID();
        return g_trips.count(guid) || g_wantsTrip.count(guid);
    }

    uint32 MoneyReserveCopper()
    {
        return g_settings.moneyReserveCopper;
    }

    void RecordCleanupVendorSale(Player* bot, uint32 itemEntry, uint32 count, uint32 unitCopper, char const* reason)
    {
        if (!bot || !g_settings.enable || !count)
            return;
        EnsureTables();
        Line line;
        line.entry = itemEntry;
        line.count = count;
        line.reason = reason && reason[0] ? reason : "bag-cleanup";
        line.verdict = Verdict::Vendor;
        line.unitCopper = unitCopper;
        line.priceSource = "bag-cleanup";
        JournalDone(bot, line, count, unitCopper, "vendor", "bag-cleanup");
    }

    bool IsReservedReagent(Player* bot, uint32 itemEntry)
    {
        BotMgr::CraftOrderState const* order = bot ? sBotMgr->GetCraftOrder(bot->GetGUID()) : nullptr;
        SpellInfo const* recipe = order ? sSpellMgr->GetSpellInfo(order->spellId) : nullptr;
        if (!recipe)
            return false;
        for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
            if (recipe->Reagent[i] > 0 && uint32(recipe->Reagent[i]) == itemEntry)
                return true;
        return false;
    }

    bool IsReservedItem(Player* bot, Item const* item)
    {
        if (!bot || !item)
            return false;
        BotEconomyPolicy::Holding holding;
        holding.reagentOfOwnCraftOrder = IsReservedReagent(bot, item->GetEntry());
        if (auto purchases = g_purchases.find(bot->GetGUID()); purchases != g_purchases.end())
            if (auto bought = purchases->second.find(item->GetGUID()); bought != purchases->second.end())
                holding.boughtRecently = BotEconomyPolicy::StillBefore(GameTime::GetGameTimeMS().count(), bought->second);
        return BotEconomyPolicy::MustKeep(holding);
    }

    void CollectMailFrom(Player* bot, ObjectGuid sender)
    {
        if (!bot || !sender)
            return;
        CollectMail(bot, [&](Mail const& m) { return m.messageType == MAIL_NORMAL && m.sender == sender.GetCounter(); });
    }

    std::vector<ReagentShortfall> MissingReagents(Player* crafter, SpellInfo const* recipe, uint32 crafts)
    {
        std::vector<ReagentShortfall> missing;
        if (!crafter || !recipe)
            return missing;
        crafts = std::max<uint32>(1, crafts);
        for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
        {
            if (recipe->Reagent[i] <= 0 || !recipe->ReagentCount[i])
                continue;
            uint32 const entry = uint32(recipe->Reagent[i]);
            uint32 const need = recipe->ReagentCount[i] * crafts;
            uint32 const have = crafter->GetItemCount(entry, false);
            if (have < need)
                missing.push_back({ entry, need - have });
        }
        return missing;
    }

    std::vector<ReagentShortfall> SourceReagents(Player* crafter, SpellInfo const* recipe, uint32 crafts, uint64& budgetLeftCopper)
    {
        constexpr uint32 CASTS_PER_SOURCING = 5;
        crafts = std::min(std::max<uint32>(1, crafts), CASTS_PER_SOURCING);
        if (!crafter || !recipe || !CraftSourcingEnabled())
            return MissingReagents(crafter, recipe, crafts);
        EnsureTables();

        for (ReagentShortfall const& shortfall : MissingReagents(crafter, recipe, crafts))
        {
            uint32 const entry = shortfall.entry;
            uint32 const target = crafter->GetItemCount(entry, false) + shortfall.count;
            auto stillShort = [&]() -> uint32
            {
                uint32 const have = crafter->GetItemCount(entry, false);
                return have >= target ? 0 : target - have;
            };

            if (g_settings.craftFromGuildBank && crafter->GetGuild())
            {
                uint32 const before = crafter->GetItemCount(entry, false);
                sBotMgr->GuildWithdrawItem(crafter->GetGUID().GetCounter(), entry, shortfall.count, nullptr);
                uint32 const after = crafter->GetItemCount(entry, false);
                if (after > before)
                    g_buyTotals.reagentsFromGuildBank += after - before;
            }
            if (uint32 const left = stillShort(); left && g_settings.craftFromVendor)
                BuyFromVendor(crafter, entry, left, budgetLeftCopper, "craft-reagent");
            if (uint32 const left = stillShort(); left && g_settings.craftFromAuction && IsLive())
                BuyFromAuction(crafter, entry, left, budgetLeftCopper, "craft-reagent");
        }
        crafter->SaveToDB(false, false);
        return MissingReagents(crafter, recipe, 1);
    }

    std::string DescribeReagents(std::vector<ReagentShortfall> const& reagents)
    {
        std::string out;
        for (ReagentShortfall const& r : reagents)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(r.entry);
            if (!out.empty())
                out += ", ";
            out += Acore::StringFormat("{}x {}", r.count, proto ? proto->Name1 : std::to_string(r.entry));
        }
        return out;
    }

    bool CraftSourcingEnabled()
    {
        return g_settings.craftFromGuildBank || g_settings.craftFromVendor || g_settings.craftFromAuction;
    }

    uint64 CraftReagentBudgetCopper()
    {
        return g_settings.craftBudgetCopper;
    }

    uint32 CraftOrderTimeoutMs()
    {
        return g_settings.craftTimeoutMin * 60 * 1000;
    }

    uint32 CraftMaxCastFailures()
    {
        return g_settings.craftMaxCastFailures;
    }
}
