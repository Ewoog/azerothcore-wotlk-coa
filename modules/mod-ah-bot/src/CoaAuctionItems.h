/*
 * Conquest of Azeroth item rules for the auction house seller.
 *
 * Released under the GNU General Public License v2 or later, as the rest of
 * mod-ah-bot.
 */

#ifndef COA_AUCTION_ITEMS_H
#define COA_AUCTION_ITEMS_H

#include <cstdint>
#include <unordered_set>

struct ItemTemplate;

namespace CoaAuctionItems
{
    // Highest item entry shipped with the 3.3.5a client; everything above it
    // was added by Conquest of Azeroth.
    constexpr std::uint32_t LastStockItemEntry = 56806;

    // Runtime level-scaled copies of stock gear (src/server/coa/AscensionItemScalingPolicy.h).
    constexpr std::uint32_t FirstScaledEntry = 4400001;
    constexpr std::uint32_t LastScaledEntry  = 4912583;

    struct Candidate
    {
        std::uint32_t entry = 0;
        bool hasClientRow = false;
        bool worldforged = false;
        bool soulbound = false;
        bool craftedByProfession = false;
    };

    constexpr bool IsScaledEntry(std::uint32_t entry)
    {
        return entry >= FirstScaledEntry && entry <= LastScaledEntry;
    }

    // Stock items may be sold; Conquest of Azeroth items only when a
    // profession recipe makes them. Scaled copies, Worldforged items,
    // soulbound items and items the client has no Item.dbc row for never are.
    constexpr bool Sellable(Candidate const& c)
    {
        if (!c.hasClientRow || c.worldforged || c.soulbound || IsScaledEntry(c.entry))
            return false;

        return c.entry <= LastStockItemEntry || c.craftedByProfession;
    }

    // Items created by a spell taught on a primary or secondary profession
    // skill line, read once from the loaded DBC and spell stores.
    std::unordered_set<std::uint32_t> const& ProfessionCraftedItems();

    Candidate Describe(ItemTemplate const& proto);
}

#endif
