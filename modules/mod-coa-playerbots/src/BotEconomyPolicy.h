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

    // A recipe is worth crafting for the market when the product fetches marginPercent more than its reagents are worth.
    constexpr bool CraftPays(std::uint64_t productValue, std::uint64_t reagentValue, std::uint32_t marginPercent)
    {
        return productValue > 0 && productValue * 100 >= reagentValue * (100 + marginPercent) && productValue > reagentValue;
    }
}

#endif // COA_PLAYERBOTS_BOT_ECONOMY_POLICY_H
