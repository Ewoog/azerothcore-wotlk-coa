/*
 * mod-coa-playerbots
 *
 * The bot economy's pure decision rules: which items a bot must keep, how much it may pay, when it gives up.
 * BotEconomy.cpp, BotMaintenance.cpp, BotAI.cpp and BotMgr.cpp apply them to live items; tests/economy_policy_tests.cpp
 * checks them without a server.
 */

#ifndef COA_PLAYERBOTS_BOT_ECONOMY_POLICY_H
#define COA_PLAYERBOTS_BOT_ECONOMY_POLICY_H

#include <cstdint>
#include <limits>

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

    // A recipe is worth crafting for the market when the product fetches marginPercent more than its reagents are worth.
    constexpr bool CraftPays(std::uint64_t productValue, std::uint64_t reagentValue, std::uint32_t marginPercent)
    {
        return productValue > 0 && productValue * 100 >= reagentValue * (100 + marginPercent) && productValue > reagentValue;
    }
}

#endif // COA_PLAYERBOTS_BOT_ECONOMY_POLICY_H
