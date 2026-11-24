/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ACORE_EXPERIENCE_BUFF_POLICY_H
#define ACORE_EXPERIENCE_BUFF_POLICY_H

#include "Define.h"

class AuraEffect;
class Player;

// CoA's Potion of Experience (818046) and party Aura of Experience (818059) only pay out while
// the receiving character is below the highest-level character on its account.
namespace ExperienceBuffPolicy
{
    constexpr uint32 SPELL_POTION_OF_EXPERIENCE = 818046;
    constexpr uint32 SPELL_AURA_OF_EXPERIENCE = 818059;

    [[nodiscard]] constexpr bool IsAccountCappedBuff(uint32 spellId)
    {
        return spellId == SPELL_POTION_OF_EXPERIENCE || spellId == SPELL_AURA_OF_EXPERIENCE;
    }

    [[nodiscard]] constexpr bool IsBelowAccountHighest(uint8 level, uint8 highestOtherLevel)
    {
        return level < highestOtherLevel;
    }

    void SetAccountCapped(Player const* player, bool capped);
    void Forget(Player const* player);
    [[nodiscard]] bool IsAccountCapped(Player const* player);

    // The aura effect filter every experience site shares: drops the account-capped buffs for a
    // capped character and the party Aura of Experience while the recruit-a-friend bonus applies.
    [[nodiscard]] bool Applies(Player const* player, AuraEffect const* effect, bool recruitAFriend);
}

#endif
