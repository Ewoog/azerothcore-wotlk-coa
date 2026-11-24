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
std::array<uint8, 33> RaceMgr::_racialRace = {};
std::unordered_set<uint32> RaceMgr::_racialSkillLines;

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
    LoadRacialSkillLines();

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
    _racialRace.fill(0);

    QueryResult result = WorldDatabase.Query("SELECT Race, BaseRace, RacialRace FROM coa_custom_race");
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

        uint8 racialRace = fields[2].Get<uint8>();
        if (racialRace && (racialRace >= _racialRace.size() || !sChrRacesStore.LookupEntry(racialRace)))
        {
            LOG_ERROR("sql.sql", "coa_custom_race: invalid racial race {} for race {}, using base race {}.", racialRace,
                race, baseRace);
            racialRace = 0;
        }

        _racialRace[race] = racialRace ? racialRace : baseRace;
    } while (result->NextRow());
}

void RaceMgr::LoadRacialSkillLines()
{
    _racialSkillLines.clear();

    QueryResult result = WorldDatabase.Query("SELECT SkillId FROM coa_racial_skill_line");
    if (!result)
        return;

    do
    {
        uint32 skillId = result->Fetch()[0].Get<uint16>();
        if (!sSkillLineStore.LookupEntry(skillId))
        {
            LOG_ERROR("sql.sql", "coa_racial_skill_line: skill {} does not exist, ignoring.", skillId);
            continue;
        }

        _racialSkillLines.insert(skillId);
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

uint32 RaceMgr::GetSkillRaceMask(uint8 race, uint32 skillId)
{
    if (!IsCustomRace(race) || !_racialSkillLines.contains(skillId))
        return GetRuleRaceMask(race);

    return (uint32(1) << (race - 1)) | (uint32(1) << (_racialRace[race] - 1));
}
