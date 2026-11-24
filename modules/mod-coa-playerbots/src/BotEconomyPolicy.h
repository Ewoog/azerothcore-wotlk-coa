/*
 * mod-coa-playerbots
 *
 * The bot economy's pure decision rules: which items a bot must keep, how much it may pay, when it gives up.
 * BotEconomy.cpp, BotMaintenance.cpp, BotAI.cpp and BotMgr.cpp apply them to live items; tests/economy_policy_tests.cpp
 * checks them without a server.
 */

#ifndef COA_PLAYERBOTS_BOT_ECONOMY_POLICY_H
#define COA_PLAYERBOTS_BOT_ECONOMY_POLICY_H

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace BotEconomyPolicy
{
    // What a bag item is to the bot, as far as selling, destroying or banking it goes.
    struct Holding
    {
        bool reagentOfOwnCraftOrder = false;
        bool boughtRecently = false;
        bool gatherOrderItem = false;
        bool protectedByOwner = false;
    };

    constexpr bool MustKeep(Holding const& h)
    {
        return h.reagentOfOwnCraftOrder || h.boughtRecently || h.gatherOrderItem || h.protectedByOwner;
    }

    // Game-time milliseconds wrap after 49 days; the difference stays right across the wrap.
    constexpr bool StillBefore(std::uint32_t now, std::uint32_t until)
    {
        return std::int32_t(now - until) < 0;
    }

    constexpr bool WithinPriceCeiling(std::uint64_t buyout, std::uint32_t itemCount, std::uint32_t vendorSellPrice, std::uint32_t maxMultiple)
    {
        return !maxMultiple || !vendorSellPrice || buyout <= std::uint64_t(vendorSellPrice) * maxMultiple * itemCount;
    }

    // 0 cap = no cap.
    constexpr std::uint64_t CapLeft(std::uint64_t cap, std::uint64_t spent)
    {
        return !cap ? std::numeric_limits<std::uint64_t>::max() : spent < cap ? cap - spent : 0;
    }

    constexpr bool IsUpgrade(float candidateScore, float wornScore, std::uint32_t minUpgradePercent)
    {
        return candidateScore > 0.0f && candidateScore > wornScore * (1.0f + minUpgradePercent / 100.0f);
    }

    // 0 limit = never give up.
    constexpr bool GiveUpAfterFailures(std::uint32_t failures, std::uint32_t limit)
    {
        return limit && failures >= limit;
    }

    // Vendors sell `buyCount` items for `buyPrice`; a part batch costs its share, rounded up.
    constexpr std::uint64_t VendorCost(std::uint32_t buyPrice, std::uint32_t buyCount, std::uint32_t items)
    {
        std::uint64_t const batch = buyCount ? buyCount : 1;
        return (std::uint64_t(buyPrice) * items + batch - 1) / batch;
    }

    // With no listing to compare against, an item is priced at `multiplier` times what a vendor pays for it, and never
    // below the price vendors would sell it for.
    constexpr std::uint64_t EstimateUnitPrice(std::uint32_t vendorSellPrice, std::uint32_t vendorBuyPrice, std::uint32_t multiplier)
    {
        std::uint64_t const fromSell = std::uint64_t(vendorSellPrice) * (multiplier ? multiplier : 1);
        std::uint64_t const estimate = fromSell > vendorBuyPrice ? fromSell : vendorBuyPrice;
        return estimate ? estimate : 1;
    }

    // Trade goods a bot puts up for sale: what it carries above its own keep count, after the guild bank's shortfall
    // (an officer's stock limit not yet met) has been paid in.
    constexpr std::uint32_t TradeGoodsForSale(std::uint32_t carried, std::uint32_t keep, std::uint32_t guildShortfall)
    {
        std::uint32_t const held = keep + guildShortfall;
        return carried > held ? carried - held : 0;
    }

    enum class ListingStop : std::uint8_t
    {
        Done,           // every item of the line is listed
        VendorBetter,   // the listing would not beat the vendor price or the deposit
        NoListingsLeft, // the bot has as many auctions running as it may
        Failed,         // the deposit or the auction house refused
    };

    // What is left of an auction line after listing goes to a vendor only when a vendor is the better buyer or the bags
    // are full; otherwise it waits in the bags for the next listing.
    constexpr bool VendorTheRest(ListingStop stop, bool bagsFull)
    {
        return stop == ListingStop::VendorBetter || stop == ListingStop::Done || bagsFull;
    }

    enum class PlannedSale : std::uint8_t
    {
        NotPlanned,
        Vendor,
        Auction,
    };

    // Whether bag cleanup may sell a stack to a vendor itself. With the live economy, stacks it plans for the auction
    // house stay for a listing, and an item class the economy judges (trade goods, gear, consumables) that it left out
    // of the plan is one it wants kept. Everything else (stale quest items, white vendor trash, known recipes) falls
    // back to the cleanup rules, which the economy never looks at.
    constexpr bool CleanupMayVendor(bool economyLive, bool protectedByOwner, PlannedSale planned, bool economyJudgesClass)
    {
        if (!economyLive)
            return true;
        if (protectedByOwner)
            return false;
        if (planned == PlannedSale::Vendor)
            return true;
        if (planned == PlannedSale::Auction)
            return false;
        return !economyJudgesClass;
    }

    // How much food (and, separately, drink) a bot keeps for its own eating: `base`, plus `perTenLevels` for every ten
    // levels, and never less than what it buys up to on a trip to town.
    constexpr std::uint32_t SupplyKeep(std::uint32_t level, std::uint32_t base, std::uint32_t perTenLevels,
        std::uint32_t buyTarget)
    {
        std::uint32_t const scaled = base + level / 10 * perTenLevels;
        return scaled > buyTarget ? scaled : buyTarget;
    }

    // One food or drink item the bot carries, all of its stacks together.
    struct SupplyStock
    {
        std::uint32_t entry = 0;
        std::uint32_t requiredLevel = 0;
        std::uint32_t count = 0;
    };

    // What is left over once the bot has kept `keep` items of one kind, the best it may eat first (highest required
    // level, then the biggest pile). Items above its level are neither kept nor surplus: it will grow into them.
    // Returns the surplus per item, leaving out items with none.
    inline std::vector<SupplyStock> SupplySurplus(std::vector<SupplyStock> held, std::uint32_t level,
        std::uint32_t keep)
    {
        std::sort(held.begin(), held.end(), [](SupplyStock const& a, SupplyStock const& b)
        {
            if (a.requiredLevel != b.requiredLevel)
                return a.requiredLevel > b.requiredLevel;
            if (a.count != b.count)
                return a.count > b.count;
            return a.entry < b.entry;
        });
        std::vector<SupplyStock> surplus;
        std::uint32_t left = keep;
        for (SupplyStock const& stock : held)
        {
            if (stock.requiredLevel > level)
                continue;
            std::uint32_t const kept = stock.count < left ? stock.count : left;
            left -= kept;
            if (stock.count > kept)
                surplus.push_back({ stock.entry, stock.requiredLevel, stock.count - kept });
        }
        return surplus;
    }

    // A recipe is worth crafting for the market when the product fetches marginPercent more than its reagents are worth.
    constexpr bool CraftPays(std::uint64_t productValue, std::uint64_t reagentValue, std::uint32_t marginPercent)
    {
        return productValue > 0 && productValue * 100 >= reagentValue * (100 + marginPercent) && productValue > reagentValue;
    }
}

#endif // COA_PLAYERBOTS_BOT_ECONOMY_POLICY_H
