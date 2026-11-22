/*
 * Conquest of Azeroth item rules for the auction house seller.
 *
 * Released under the GNU General Public License v2 or later, as the rest of
 * mod-ah-bot.
 */

#include "CoaAuctionItems.h"

#include "DBCStores.h"
#include "ItemTemplate.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <string_view>

namespace
{
    constexpr std::string_view WorldforgedMarker = "@Worldforged@";

    bool IsProfessionSkill(uint32 skillId)
    {
        SkillLineEntry const* skill = sSkillLineStore.LookupEntry(skillId);
        return skill && (skill->categoryId == SKILL_CATEGORY_PROFESSION || skill->categoryId == SKILL_CATEGORY_SECONDARY);
    }

    std::unordered_set<uint32> CollectProfessionCraftedItems()
    {
        std::unordered_set<uint32> items;

        for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
        {
            SkillLineAbilityEntry const* ability = sSkillLineAbilityStore.LookupEntry(i);
            if (!ability || !IsProfessionSkill(ability->SkillLine))
                continue;

            SpellInfo const* spell = sSpellMgr->GetSpellInfo(ability->Spell);
            if (!spell)
                continue;

            for (SpellEffectInfo const& effect : spell->GetEffects())
                if (effect.IsEffect(SPELL_EFFECT_CREATE_ITEM) || effect.IsEffect(SPELL_EFFECT_CREATE_ITEM_2))
                    if (effect.ItemType)
                        items.insert(effect.ItemType);
        }

        return items;
    }
}

std::unordered_set<std::uint32_t> const& CoaAuctionItems::ProfessionCraftedItems()
{
    static std::unordered_set<std::uint32_t> const items = CollectProfessionCraftedItems();
    return items;
}

CoaAuctionItems::Candidate CoaAuctionItems::Describe(ItemTemplate const& proto)
{
    Candidate c;
    c.entry = proto.ItemId;
    c.hasClientRow = sItemStore.LookupEntry(proto.ItemId) != nullptr;
    c.worldforged = std::string_view(proto.Description).substr(0, WorldforgedMarker.size()) == WorldforgedMarker;
    c.soulbound = proto.Bonding == BIND_WHEN_PICKED_UP || proto.Bonding == BIND_QUEST_ITEM;
    c.craftedByProfession = ProfessionCraftedItems().count(proto.ItemId) != 0;
    return c;
}
