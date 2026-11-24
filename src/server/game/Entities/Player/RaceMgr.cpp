/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "RaceMgr.h"
#include "AccountMgr.h"
#include "DatabaseEnv.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Util.h"
#include "WorldPacket.h"
#include "WorldSession.h"

uint8  RaceMgr::_maxRaces = 0;

uint32 RaceMgr::_playableRaceMask = 0;
uint32 RaceMgr::_allianceRaceMask = 0;
uint32 RaceMgr::_hordeRaceMask = 0;

std::array<uint8, 33> RaceMgr::_baseRace = {};

RaceMgr::RaceMgr()
{
}

RaceMgr::~RaceMgr()
{
}

RaceMgr* RaceMgr::instance()
{
    static RaceMgr instance;
    return &instance;
}

void RaceMgr::LoadRaces()
{
    SetMaxRaces(0 + 1);
    _playableRaceMask = 0;
    _hordeRaceMask    = 0;
    _allianceRaceMask = 0;

    LoadBaseRaces();

    for (auto const& raceEntry : sChrRacesStore)
    {
        if (!raceEntry)
            continue;

        uint8 alliance = raceEntry->alliance;
        uint8 raceId = raceEntry->RaceID;

        if (raceEntry->Flags & CHRRACES_FLAGS_NOT_PLAYABLE)
            continue;

        if (GetMaxRaces() <= raceId)
            SetMaxRaces(raceId + 1);

        uint32 raceBit = (1 << (raceId - 1));

        _playableRaceMask |= raceBit;

        if (alliance == ALLIANCE_HORDE)
            _hordeRaceMask |= raceBit;
        else if (alliance == ALLIANCE_ALLIANCE)
            _allianceRaceMask |= raceBit;
    }
}

void RaceMgr::LoadBaseRaces()
{
    _baseRace.fill(0);

    QueryResult result = WorldDatabase.Query("SELECT Race, BaseRace FROM coa_custom_race");
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        uint8 race = fields[0].Get<uint8>();
        uint8 baseRace = fields[1].Get<uint8>();
        if (!race || race >= _baseRace.size() || !baseRace || baseRace >= _baseRace.size() || race == baseRace
            || !sChrRacesStore.LookupEntry(race) || !sChrRacesStore.LookupEntry(baseRace))
        {
            LOG_ERROR("sql.sql", "coa_custom_race: invalid race {} or base race {}, ignoring.", race, baseRace);
            continue;
        }

        _baseRace[race] = baseRace;
    } while (result->NextRow());
}

uint32 RaceMgr::GetRuleRaceMask(uint8 race)
{
    if (!race || race >= _baseRace.size())
        return 0;

    uint32 mask = uint32(1) << (race - 1);
    if (uint8 baseRace = _baseRace[race])
        mask |= uint32(1) << (baseRace - 1);

    return mask;
}

bool RaceMgr::IsCustomRace(uint8 race)
{
    return race && race < _baseRace.size() && _baseRace[race];
}
