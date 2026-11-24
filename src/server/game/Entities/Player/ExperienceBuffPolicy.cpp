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

#include "ExperienceBuffPolicy.h"
#include "Player.h"
#include "SpellAuraEffects.h"

#include <mutex>
#include <shared_mutex>
#include <unordered_set>

namespace
{
    std::shared_mutex CappedLock;
    std::unordered_set<ObjectGuid::LowType> CappedCharacters;
}

void ExperienceBuffPolicy::SetAccountCapped(Player const* player, bool capped)
{
    std::unique_lock lock(CappedLock);
    if (capped)
        CappedCharacters.insert(player->GetGUID().GetCounter());
    else
        CappedCharacters.erase(player->GetGUID().GetCounter());
}

void ExperienceBuffPolicy::Forget(Player const* player)
{
    SetAccountCapped(player, false);
}

bool ExperienceBuffPolicy::IsAccountCapped(Player const* player)
{
    std::shared_lock lock(CappedLock);
    return CappedCharacters.contains(player->GetGUID().GetCounter());
}

bool ExperienceBuffPolicy::Applies(Player const* player, AuraEffect const* effect, bool recruitAFriend)
{
    uint32 const spellId = effect->GetId();
    if (spellId == SPELL_AURA_OF_EXPERIENCE && recruitAFriend)
        return false;
    return !IsAccountCappedBuff(spellId) || !IsAccountCapped(player);
}
