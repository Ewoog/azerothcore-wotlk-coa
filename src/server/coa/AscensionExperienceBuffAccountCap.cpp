/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */

#include "DatabaseEnv.h"
#include "ExperienceBuffPolicy.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "WorldSession.h"

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace
{
using CharacterLevels = std::unordered_map<ObjectGuid::LowType, uint8>;

CharacterLevels StoredAccountLevels(uint32 accountId)
{
    CharacterLevels levels;
    if (QueryResult result = CharacterDatabase.Query(
        "SELECT `guid`, `level` FROM `characters` WHERE `account` = {} AND `deleteDate` IS NULL", accountId))
    {
        do
        {
            Field* fields = result->Fetch();
            levels[fields[0].Get<uint32>()] = fields[1].Get<uint8>();
        } while (result->NextRow());
    }
    return levels;
}

uint8 HighestOtherLevel(CharacterLevels const& levels, ObjectGuid::LowType self)
{
    uint8 highest = 0;
    for (auto const& [guid, level] : levels)
        if (guid != self)
            highest = std::max(highest, level);
    return highest;
}

void RefreshAccount(Player* trigger)
{
    WorldSession* session = trigger->GetSession();
    if (!session)
        return;

    uint32 const accountId = session->GetAccountId();
    CharacterLevels levels = StoredAccountLevels(accountId);
    levels[trigger->GetGUID().GetCounter()] = trigger->GetLevel();

    std::shared_lock lock(*HashMapHolder<Player>::GetLock());
    std::vector<Player*> online{ trigger };
    for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
    {
        WorldSession* other = player->GetSession();
        if (player == trigger || !other || other->GetAccountId() != accountId)
            continue;

        levels[guid.GetCounter()] = player->GetLevel();
        online.push_back(player);
    }

    for (Player* player : online)
    {
        uint8 const highestOther = HighestOtherLevel(levels, player->GetGUID().GetCounter());
        ExperienceBuffPolicy::SetAccountCapped(player,
            !ExperienceBuffPolicy::IsBelowAccountHighest(player->GetLevel(), highestOther));
    }
}

class coa_experience_buff_account_cap : public PlayerScript
{
public:
    coa_experience_buff_account_cap() : PlayerScript("coa_experience_buff_account_cap",
        { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_LEVEL_CHANGED }) { }

    void OnPlayerLogin(Player* player) override
    {
        RefreshAccount(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        ExperienceBuffPolicy::Forget(player);
    }

    void OnPlayerLevelChanged(Player* player, uint8) override
    {
        RefreshAccount(player);
    }
};
}

void AddSC_AscensionExperienceBuffAccountCap()
{
    new coa_experience_buff_account_cap();
}
