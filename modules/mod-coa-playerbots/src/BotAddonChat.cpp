/*
 * mod-coa-playerbots
 *
 * Server-side half of the bot-control addon protocol (see docs/addon-protocol.md and the
 * client AddOn, docs/addon-client.md). A real player's UI addon sends a self-whisper carrying
 * an addon message ("COABOT\t<VERB>:<botGuidLow>[:<arg>]") -- Player::Whisper() already runs
 * every outgoing whisper through PlayerScript::OnPlayerCanUseChat(..., Player* receiver)
 * before actually delivering it (see mod-ascension-compat's CoABugReport.cpp for another,
 * earlier use of exactly this same self-whisper-as-addon-channel trick), so this only needs to
 * recognize the prefix, dispatch the command, and return false to consume it -- no new opcode
 * or core change needed.
 */

#include "engine/BotDebugLog.h"
#include "BotAI.h"
#include "BotMovement.h"
#include "BotEconomy.h"
#include "BotFormations.h"
#include "BotMgr.h"
#include "BotProgression.h"
#include "Chat.h"
#include "ClassSpecRoles.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "ItemTemplate.h"
#include "map/BotMapService.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QueryResult.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "Unit.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace
{
constexpr char PROTOCOL_PREFIX[] = "COABOT\t";
constexpr std::size_t PROTOCOL_PREFIX_LEN = sizeof(PROTOCOL_PREFIX) - 1;

std::vector<std::string> SplitColon(std::string const& body)
{
    std::vector<std::string> parts;
    std::istringstream stream(body);
    std::string part;
    while (std::getline(stream, part, ':'))
        parts.push_back(part);
    return parts;
}

// Name<->code tables for the gear-preference verbs (GETGEAR/SETGEARPREF) -- wire format uses
// plain lowercase names, matching every other verb's convention (role/formation names), rather
// than raw ItemSubclassArmor/ItemSubclassWeapon numbers the addon would have no way to interpret.
struct GearTypeEntry
{
    char const* name;
    uint32 subclass; // raw ItemSubclassArmor / ItemSubclassWeapon value
};

constexpr GearTypeEntry ARMOR_TYPES[] = {
    { "cloth",   ITEM_SUBCLASS_ARMOR_CLOTH },
    { "leather", ITEM_SUBCLASS_ARMOR_LEATHER },
    { "mail",    ITEM_SUBCLASS_ARMOR_MAIL },
    { "plate",   ITEM_SUBCLASS_ARMOR_PLATE },
};

constexpr GearTypeEntry WEAPON_TYPES[] = {
    { "sword",  ITEM_SUBCLASS_WEAPON_SWORD },
    { "mace",   ITEM_SUBCLASS_WEAPON_MACE },
    { "axe",    ITEM_SUBCLASS_WEAPON_AXE },
    { "fist",   ITEM_SUBCLASS_WEAPON_FIST },
    { "dagger", ITEM_SUBCLASS_WEAPON_DAGGER },
    { "staff",  ITEM_SUBCLASS_WEAPON_STAFF },
};

bool GearSubclassByName(GearTypeEntry const* table, std::size_t count, std::string const& name, uint32& outSubclass)
{
    for (std::size_t i = 0; i < count; ++i)
    {
        if (name == table[i].name)
        {
            outSubclass = table[i].subclass;
            return true;
        }
    }
    return false;
}

char const* GearNameBySubclass(GearTypeEntry const* table, std::size_t count, uint32 subclass)
{
    for (std::size_t i = 0; i < count; ++i)
        if (table[i].subclass == subclass)
            return table[i].name;
    return "auto";
}

// Mandatory per docs/addon-protocol.md's "Server-side authorization" section: the guid must
// really be one of our tracked bot sessions (not an arbitrary character), AND the commanding
// player must actually be grouped with it -- never someone else's bot. Both checks fail
// closed (return nullptr) rather than reporting why, matching the doc's "silently drop"
// policy -- an addon bug or a stale/renamed bot shouldn't be able to flood chat with errors.
Player* ResolveAuthorizedBot(Player* commander, ObjectGuid::LowType botGuidLow)
{
    Player* bot = sBotMgr->FindBotPlayer(botGuidLow);
    if (!bot)
        return nullptr;

    Group* botGroup = bot->GetGroup();
    if (!botGroup || botGroup != commander->GetGroup())
        return nullptr;

    return bot;
}

// Server -> client half of the protocol -- same self-whisper trick in reverse (see
// mod-ascension-compat's CoABugReport.cpp for the precedent this is copied from):
// build a CHAT_MSG_WHISPER/LANG_ADDON packet addressed from the player to themself and hand
// it directly to their own session, since a bot's own null-socket session can't send it and
// there's no other recipient involved. Body gets the same "COABOT\t" prefix as an outgoing
// client message so one client-side CHAT_MSG_ADDON handler covers both directions.
// Largest reply body that keeps "COABOT\t" + body at or under COABOT_MAX_REPLY_BYTES (250). The
// 3.3.5 client's addon-whisper path is only safe up to ~255 bytes; a longer line can crash the
// client before any addon sees it (seen with the 2.2.0 ROSTER line), so nothing longer ever leaves.
constexpr std::size_t MAX_REPLY_BODY = COABOT_MAX_REPLY_BYTES - PROTOCOL_PREFIX_LEN;

void SendCoaBotReply(Player* recipient, std::string const& body)
{
    if (body.size() > MAX_REPLY_BODY)
    {
        // Last line of defence: every builder below is sized to fit, so this is a bug worth logging.
        LOG_ERROR("module.coa-playerbots", "BotAddonChat: reply '{}...' is {} bytes (cap {}); cut to fit.",
            body.substr(0, 24), body.size() + PROTOCOL_PREFIX_LEN, COABOT_MAX_REPLY_BYTES);
        SendCoaBotReply(recipient, CoaBotWireText(body, MAX_REPLY_BODY, ""));
        return;
    }
    WorldPacket packet;
    ChatHandler::BuildChatPacket(packet, CHAT_MSG_WHISPER, LANG_ADDON, recipient->GetGUID(), recipient->GetGUID(),
        std::string(PROTOCOL_PREFIX) + body, 0, recipient->GetName(), recipient->GetName(), 0, false);
    recipient->SendDirectMessage(&packet);
}

// Free text (quest titles, objective names) goes last on its reply line so the client can take
// everything after the fixed fields verbatim, colons included. Pipes and line breaks are swapped
// for spaces (a stray '|' would start a client escape sequence) and the text is cut on a UTF-8
// character boundary so the whole body stays under the addon-whisper length cap.
std::string WireText(std::string const& text, std::size_t maxBytes)
{
    return CoaBotWireText(text, maxBytes, "|");
}

// Room left for the trailing free-text field of a reply whose fixed part is `head`.
std::size_t WireRoom(std::string const& head)
{
    return CoaBotWireRoom(head);
}

// PROFS:<guid>:<skillId>:<value>:<max>|... -- the first line keeps the old verb (older AltBots
// read it as the whole list); entries that don't fit go on PROFSMORE lines of the same shape,
// which newer AltBots merge in. 16 professions fit on one line in practice; paging only kicks in
// for absurd skill values. PROFALLOWED stays one line (16 ids x <= 4 digits).
void SendProfessionLists(Player* commander, Player* bot, ObjectGuid::LowType botGuidLow)
{
    uint32 catalogCount = 0;
    BotProgression::ProfessionInfo const* catalog = BotProgression::ProfessionCatalog(catalogCount);
    std::string allowed;
    std::vector<std::string> learned;
    for (uint32 i = 0; i < catalogCount; ++i)
    {
        uint32 skillId = catalog[i].skillId;
        if (!allowed.empty())
            allowed += ",";
        allowed += std::to_string(skillId);
        if (!bot->HasSkill(skillId))
            continue;
        learned.push_back(std::to_string(skillId) + ":" +
            std::to_string(uint32(bot->GetSkillValue(skillId))) + ":" +
            std::to_string(uint32(bot->GetMaxSkillValue(skillId))));
    }
    std::string const guid = std::to_string(botGuidLow);
    SendCoaBotReply(commander, "PROFALLOWED:" + guid + ":0:" + allowed);

    std::string head = "PROFS:" + guid + ":";
    std::string body;
    for (std::string const& entry : learned)
    {
        if (!body.empty() && head.size() + body.size() + 1 + entry.size() > MAX_REPLY_BODY)
        {
            SendCoaBotReply(commander, head + body);
            head = "PROFSMORE:" + guid + ":";
            body.clear();
        }
        if (!body.empty())
            body += "|";
        body += entry;
    }
    SendCoaBotReply(commander, head + body);
}

// GETQUESTS reply: QUESTHDR, then QUEST + its QOBJ lines per quest-log entry, then QUESTEND.
void SendBotQuestLog(Player* commander, Player* bot, ObjectGuid::LowType botGuidLow)
{
    std::string const guid = std::to_string(botGuidLow);

    std::vector<uint32> questIds;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (questId && sObjectMgr->GetQuestTemplate(questId))
            questIds.push_back(questId);
    }

    SendCoaBotReply(commander, "QUESTHDR:" + guid + ":" + std::to_string(questIds.size()) + ":" +
        std::to_string(uint32(bot->GetLevel())));

    QuestStatusMap& statusMap = bot->getQuestStatusMap();
    for (uint32 questId : questIds)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        QuestStatus status = bot->GetQuestStatus(questId);
        uint32 statusCode = 0;
        if (status == QUEST_STATUS_COMPLETE)
            statusCode = 1;
        else if (status == QUEST_STATUS_FAILED)
            statusCode = 2;

        int32 level = bot->GetQuestLevel(quest);
        if (level <= 0)
            level = int32(bot->GetLevel());

        std::string const qid = std::to_string(questId);
        std::string head = "QUEST:" + guid + ":" + qid + ":" + std::to_string(level) + ":" +
            std::to_string(statusCode) + ":";
        SendCoaBotReply(commander, head + WireText(quest->GetTitle(), WireRoom(head)));

        QuestStatusData const* data = nullptr;
        QuestStatusMap::const_iterator itr = statusMap.find(questId);
        if (itr != statusMap.end())
            data = &itr->second;

        auto sendObjective = [&](char type, uint32 id, uint32 have, uint32 need, std::string const& name)
        {
            if (have > need)
                have = need;
            std::string objHead = "QOBJ:" + guid + ":" + qid + ":" + std::string(1, type) + ":" +
                std::to_string(id) + ":" + std::to_string(have) + ":" + std::to_string(need) + ":";
            SendCoaBotReply(commander, objHead + WireText(name, WireRoom(objHead)));
        };

        // Kill / use-object objectives (>0 creature entry, <0 gameobject entry).
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            int32 npcOrGo = quest->RequiredNpcOrGo[i];
            uint32 need = quest->RequiredNpcOrGoCount[i];
            if (!npcOrGo || !need)
                continue;

            uint32 have = data ? data->CreatureOrGOCount[i] : 0;
            std::string name = quest->ObjectiveText[i];
            char type = 'c';
            uint32 entry = 0;
            if (npcOrGo > 0)
            {
                entry = uint32(npcOrGo);
                if (name.empty())
                    if (CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(entry))
                        name = tmpl->Name;
            }
            else
            {
                type = 'g';
                entry = uint32(-npcOrGo);
                if (name.empty())
                    if (GameObjectTemplate const* tmpl = sObjectMgr->GetGameObjectTemplate(entry))
                        name = tmpl->name;
            }
            if (name.empty())
                name = std::string(type == 'c' ? "Creature " : "Object ") + std::to_string(entry);
            sendObjective(type, entry, have, need, name);
        }

        // Item objectives: what the bot is carrying (bank included), capped at the requirement.
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            uint32 itemId = quest->RequiredItemId[i];
            uint32 need = quest->RequiredItemCount[i];
            if (!itemId || !need)
                continue;

            uint32 have = bot->GetItemCount(itemId, true);
            std::string name;
            if (ItemTemplate const* tmpl = sObjectMgr->GetItemTemplate(itemId))
                name = tmpl->Name1;
            if (name.empty())
                name = "Item " + std::to_string(itemId);
            sendObjective('i', itemId, have, need, name);
        }

        // Player kills (PvP quests).
        if (uint32 needKills = quest->GetPlayersSlain())
            sendObjective('p', 0, data ? data->PlayerCount : 0, needKills, "Players slain");

        // Exploration / scripted-event requirement: one done-or-not line.
        if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT))
        {
            std::string name;
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT && name.empty(); ++i)
                if (!quest->RequiredNpcOrGo[i] && !quest->ObjectiveText[i].empty())
                    name = quest->ObjectiveText[i];
            if (name.empty())
                name = "Explore or complete the event";
            bool done = (data && data->Explored) || statusCode == 1;
            sendObjective('e', 0, done ? 1 : 0, 1, name);
        }
    }

    SendCoaBotReply(commander, "QUESTEND:" + guid + ":" + std::to_string(questIds.size()));
}

void HandleCoaBotMessage(Player* commander, std::string const& body);
}

std::string CoaBotWireText(std::string const& text, std::size_t maxBytes, char const* bad)
{
    std::string out;
    out.reserve(text.size() < maxBytes ? text.size() : maxBytes);
    for (char c : text)
    {
        bool replace = c == '\n' || c == '\r' || c == '\0';
        for (char const* b = bad; b && *b && !replace; ++b)
            replace = (c == *b);
        out += replace ? ' ' : c;
    }
    if (out.size() > maxBytes)
    {
        // Cut on a UTF-8 character boundary (never in the middle of a multi-byte name).
        std::size_t cut = maxBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
            --cut;
        out.resize(cut);
    }
    return out;
}

std::size_t CoaBotWireRoom(std::string const& head)
{
    std::size_t const maxBody = COABOT_MAX_REPLY_BYTES - (sizeof("COABOT\t") - 1);
    return head.size() >= maxBody ? 0 : maxBody - head.size();
}

void CoaBotSendAddonReply(Player* recipient, std::string const& body)
{
    if (recipient && recipient->GetSession())
        SendCoaBotReply(recipient, body);
}

namespace
{
void HandleCoaBotMessage(Player* commander, std::string const& body)
{
    std::vector<std::string> parts = SplitColon(body);
    if (parts.size() < 2)
    {
        LOG_INFO("module.coa-playerbots", "BotAddonChat: '{}' sent malformed COABOT body '{}' (fewer than 2 colon-parts) -- dropped.",
            commander->GetName(), body);
        return;
    }

    std::string const& verb = parts[0];

    // QUICKFILL acts on the commander's own group as a whole, not one specific bot -- no
    // botGuidLow/authorization gate applies (the wire format's colon-part is a "0" placeholder
    // to satisfy the >= 2 parts check above, see docs/addon-protocol.md).
    if (verb == "QUICKFILL")
    {
        LOG_INFO("module.coa-playerbots", "BotAddonChat: '{}' -> QUICKFILL.", commander->GetName());
        // A real ChatHandler (not nullptr) so the player actually sees *why* nothing happened
        // when the bot pool is empty or the group's already full -- QuickFillGroup already had
        // this feedback built in for the .botcmd path, it just never reached the addon path.
        ChatHandler handler(commander->GetSession());
        sBotMgr->QuickFillGroup(commander, &handler);
        return;
    }

    // GETBOTS:<page>:<role|any>:<class id|0>:<min level>:<max level>:<guild only 0/1>:<name part> - the "Browse" tab's list of
    // online bots (BOTPAGE / BOTS replies). INVITEBOT:<guid> and GUILDINVITEBOT:<guid> invite one of the listed bots.
    if (verb == "GETBOTS")
    {
        BotMgr::BotListQuery query;
        query.page = parts.size() > 1 ? std::strtoul(parts[1].c_str(), nullptr, 10) : 0;
        if (parts.size() > 2)
        {
            if (parts[2] == "tank")
                query.role = int32(BotRole::Tank);
            else if (parts[2] == "healer")
                query.role = int32(BotRole::Healer);
            else if (parts[2] == "dps")
                query.role = int32(BotRole::Dps);
        }
        query.classId = parts.size() > 3 ? std::strtoul(parts[3].c_str(), nullptr, 10) : 0;
        query.minLevel = parts.size() > 4 ? std::strtoul(parts[4].c_str(), nullptr, 10) : 0;
        query.maxLevel = parts.size() > 5 ? std::min<uint32>(255, std::strtoul(parts[5].c_str(), nullptr, 10)) : 255;
        query.guildOnly = parts.size() > 6 && parts[6] == "1";
        query.name = parts.size() > 7 ? parts[7] : std::string();
        for (std::string const& line : sBotMgr->GetBotList(commander, query))
            SendCoaBotReply(commander, line);
        return;
    }
    if ((verb == "INVITEBOT" || verb == "GUILDINVITEBOT") && parts.size() >= 2)
    {
        ObjectGuid::LowType target = std::strtoul(parts[1].c_str(), nullptr, 10);
        ChatHandler handler(commander->GetSession());
        if (verb == "INVITEBOT")
            sBotMgr->InviteBotToGroup(commander, target, &handler);
        else
            sBotMgr->InviteBotToGuild(commander, target, &handler);
        return;
    }

    // GETALTS:0 lists the characters on the commander's own account, so AltBots can drop saved
    // alts that no longer exist (after a character or database wipe) and learn new ones without
    // a login on each. Replies: ALTCHAR:<guid>:<class>:<level>:<race>:<companion online 0/1>:<name>
    // per character, then ALTCHAREND:<count>.
    if (verb == "GETALTS")
    {
        WorldSession* session = commander->GetSession();
        ObjectGuid const commanderGuid = commander->GetGUID();
        session->QueueQueryCallback(CharacterDatabase.AsyncQuery(Acore::StringFormat(
            "SELECT guid, class, level, race, name FROM characters WHERE account = {} ORDER BY name",
            session->GetAccountId())).WithCallback([commanderGuid](QueryResult result)
            {
                Player* requester = ObjectAccessor::FindConnectedPlayer(commanderGuid);
                if (!requester)
                    return;
                uint32 count = 0;
                if (result)
                {
                    do
                    {
                        Field* fields = result->Fetch();
                        ObjectGuid::LowType const guidLow = fields[0].Get<uint32>();
                        bool const online = sBotMgr->FindBotPlayer(guidLow) != nullptr;
                        std::string const head = "ALTCHAR:" + std::to_string(guidLow) + ":" +
                            std::to_string(uint32(fields[1].Get<uint8>())) + ":" +
                            std::to_string(uint32(fields[2].Get<uint8>())) + ":" +
                            std::to_string(uint32(fields[3].Get<uint8>())) + ":" + (online ? "1" : "0") + ":";
                        SendCoaBotReply(requester, head + WireText(fields[4].Get<std::string>(), WireRoom(head)));
                        ++count;
                    } while (result->NextRow());
                }
                SendCoaBotReply(requester, "ALTCHAREND:" + std::to_string(count));
            }));
        return;
    }

    // INVITEROLE:<tank|healer|dps> invites a single bot of that role to the commander's group (the "pocket healer" case;
    // Quick Fill fills the whole party). The commander is the requester, so no bot guid / authorization gate applies.
    if (verb == "INVITEROLE" && parts.size() >= 2)
    {
        BotRole role = BotRole::Dps;
        std::string const& r = parts[1];
        if (r == "tank")
            role = BotRole::Tank;
        else if (r == "healer")
            role = BotRole::Healer;
        else if (r != "dps")
            return;
        LOG_INFO("module.coa-playerbots", "BotAddonChat: '{}' -> INVITEROLE {}.", commander->GetName(), r);
        ChatHandler handler(commander->GetSession());
        sBotMgr->InviteBotForRole(commander, role, &handler);
        return;
    }

    // AUTODUNGEON also acts on the commander's whole group, not one bot -- the second colon-part
    // carries the actual on/off value (1 or 0) instead of the usual "0" placeholder, since this
    // verb needs it. See BotMgr::SetAutoDungeonMode for what this actually changes.
    if (verb == "AUTODUNGEON" && parts.size() >= 2)
    {
        bool enabled = parts[1] == "1";
        LOG_INFO("module.coa-playerbots", "BotAddonChat: '{}' -> AUTODUNGEON {}.", commander->GetName(), enabled ? "on" : "off");
        sBotMgr->SetAutoDungeonMode(commander->GetGUID(), enabled);
        return;
    }

    // FORMATION acts on the commander's whole group, same shape as AUTODUNGEON -- the second
    // colon-part carries the formation name (see BotFormations.h's ParseFormation for accepted
    // names/synonyms). Group-wide state, so restrict to the group leader same as the equivalent
    // .botcmd formation GM command (BotCommand.cpp's HandleBotFormationCommand) -- otherwise any
    // grouped player could silently override the leader's own formation choice.
    if (verb == "FORMATION" && parts.size() >= 3)
    {
        Group* group = commander->GetGroup();
        if (!group || group->GetLeaderGUID() != commander->GetGUID())
        {
            LOG_INFO("module.coa-playerbots", "BotAddonChat: '{}' sent FORMATION but isn't the group leader -- dropped.",
                commander->GetName());
            return;
        }

        BotGroupFormation formation = ParseFormation(parts[2]);
        sBotMgr->SetGroupFormation(commander->GetGUID(), formation);
        LOG_INFO("module.coa-playerbots", "BotAddonChat: '{}' -> FORMATION {}.", commander->GetName(), FormationToString(formation));

        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (member && member != commander && sBotMgr->FindBotPlayer(member->GetGUID().GetCounter()))
            {
                if (member->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
                    BotMovement::Follow(member, MoveOwner::Travel, commander, BotAI::ComputeFollowDistance(member), BotAI::ComputeFollowAngle(member));
            }
        }
        return;
    }

    // GETFORMATION: query, no state change -- lets the addon show the group's currently active
    // formation (e.g. after a UI reload) instead of guessing. Mirrors GETROLES's query shape.
    if (verb == "GETFORMATION")
    {
        Group* group = commander->GetGroup();
        ObjectGuid leaderGuid = group ? group->GetLeaderGUID() : commander->GetGUID();
        SendCoaBotReply(commander, std::string("FORMATION:") + FormationToString(sBotMgr->GetGroupFormation(leaderGuid)));
        return;
    }

    // TELEPORT: "bring bots to me", group-wide, same "0" placeholder convention as QUICKFILL.
    // Out-of-combat-only gating and the per-bot in-combat skip both live in
    // BotMgr::TeleportBotsToPlayer itself.
    if (verb == "TELEPORT")
    {
        LOG_INFO("module.coa-playerbots", "BotAddonChat: '{}' -> TELEPORT.", commander->GetName());
        sBotMgr->TeleportBotsToPlayer(commander, nullptr);
        return;
    }

    // CRAFTORDER also doesn't target one specific bot -- the requester is always the sender
    // (commander), and BotMgr::CraftOrder itself finds the guild-mate bot to craft it. Reuses
    // the second colon-part for itemEntry instead of a bot guid.
    if (verb == "CRAFTORDER" && parts.size() >= 2)
    {
        uint32 itemEntry = std::strtoul(parts[1].c_str(), nullptr, 10);
        uint32 count = parts.size() >= 3 ? std::strtoul(parts[2].c_str(), nullptr, 10) : 1;
        LOG_INFO("module.coa-playerbots", "BotAddonChat: '{}' -> CRAFTORDER item {} x{}.", commander->GetName(), itemEntry, count);
        sBotMgr->CraftOrder(commander->GetGUID().GetCounter(), itemEntry, count ? count : 1, nullptr);
        return;
    }

    // GUILDROSTER: task-board query, also acts on the sender's whole guild rather than one
    // bot -- send as GUILDROSTER:0 (same "0" placeholder convention as QUICKFILL, see its
    // comment above). Replies with one ROSTER:... message per guild bot (many small replies
    // rather than one giant one, since a big guild's roster could exceed the chat length cap).
    if (verb == "GUILDROSTER")
    {
        LOG_INFO("module.coa-playerbots", "BotAddonChat: '{}' -> GUILDROSTER.", commander->GetName());
        for (std::string const& line : sBotMgr->GetGuildRosterInfo(commander))
            SendCoaBotReply(commander, line);
        return;
    }

    // GATHERORDER: same shape as CRAFTORDER -- the requester is always the sender, and
    // BotMgr::GatherOrder itself picks the guild-mate bot. Second colon-part is the itemEntry,
    // third (optional) is count.
    if (verb == "GATHERORDER" && parts.size() >= 2)
    {
        uint32 itemEntry = std::strtoul(parts[1].c_str(), nullptr, 10);
        uint32 count = parts.size() >= 3 ? std::strtoul(parts[2].c_str(), nullptr, 10) : 1;
        LOG_INFO("module.coa-playerbots", "BotAddonChat: '{}' -> GATHERORDER item {} x{}.", commander->GetName(), itemEntry, count);
        sBotMgr->GatherOrder(commander->GetGUID().GetCounter(), itemEntry, count ? count : 1, nullptr);
        return;
    }

    // Task Board order management. These name a guild bot (or a queue id), not a grouped one, so
    // they sit before the grouped-bot authorization gate below; BotMgr checks instead that the
    // sender is in that guild and is a guild master/officer or the player who placed the order.
    //   CANCELORDER:<botGuidLow>     -> ORDERRESULT:cancel:<botGuidLow>:<ok|denied|none>
    //   PRIORITIZEORDER:<botGuidLow> -> ORDERRESULT:top:<botGuidLow>:<ok|denied|none|offline|busy|failed>
    //   CANCELQUEUED:<queueId>       -> ORDERRESULT:cancelqueued:<queueId>:<ok|denied|none>
    //   TOPQUEUED:<queueId>          -> ORDERRESULT:topqueued:<queueId>:<ok|denied|none>
    if (verb == "CANCELORDER" || verb == "PRIORITIZEORDER" || verb == "CANCELQUEUED" || verb == "TOPQUEUED")
    {
        uint32 id = std::strtoul(parts[1].c_str(), nullptr, 10);
        std::string result = "none";
        char const* kind = "cancel";
        if (verb == "CANCELORDER")
            result = sBotMgr->CancelGatherOrder(commander, id);
        else if (verb == "PRIORITIZEORDER")
        {
            kind = "top";
            result = sBotMgr->PrioritizeGatherOrder(commander, id);
        }
        else if (verb == "CANCELQUEUED")
        {
            kind = "cancelqueued";
            result = sBotMgr->CancelQueuedGatherOrder(commander, id);
        }
        else
        {
            kind = "topqueued";
            result = sBotMgr->TopQueuedGatherOrder(commander, id);
        }
        LOG_INFO("module.coa-playerbots", "BotAddonChat: '{}' -> {} {} = {}.", commander->GetName(), verb, id, result);
        SendCoaBotReply(commander, std::string("ORDERRESULT:") + kind + ":" + std::to_string(id) + ":" + result);
        return;
    }

    // GOLDORDER: ask a free guild-mate bot to earn that many gold for the guild bank (0 cancels the
    // guild's gold orders). Guild master or officers only, checked in BotEconomy::PlaceGoldOrder.
    if (verb == "GOLDORDER" && parts.size() >= 2)
    {
        uint32 gold = std::strtoul(parts[1].c_str(), nullptr, 10);
        LOG_INFO("module.coa-playerbots", "BotAddonChat: '{}' -> GOLDORDER {} gold.", commander->GetName(), gold);
        BotEconomy::PlaceGoldOrder(commander, gold, nullptr);
        return;
    }

    // GETSTOCK / SETSTOCK / CLEARSTOCK: the "Stock" tab. GETSTOCK:0 lists the resources in the guild bank with their limits
    // (STOCK: replies); SETSTOCK:<item>:<keep> and CLEARSTOCK:<item> change a limit (guild master or officers only).
    if (verb == "GETSTOCK")
    {
        for (std::string const& line : BotEconomy::GetStockLines(commander))
            SendCoaBotReply(commander, line);
        return;
    }
    // GETECONOMY: the "Economy" tab -- state, gold and 24-hour trade of the sender's guild and group bots, and the latest trades.
    if (verb == "GETECONOMY")
    {
        for (std::string const& line : BotEconomy::GetEconomyLines(commander))
            SendCoaBotReply(commander, line);
        return;
    }
    if (verb == "SETSTOCK" && parts.size() >= 3)
    {
        BotEconomy::SetStockLimit(commander, std::strtoul(parts[1].c_str(), nullptr, 10), std::strtoll(parts[2].c_str(), nullptr, 10), nullptr);
        return;
    }
    if (verb == "CLEARSTOCK" && parts.size() >= 2)
    {
        BotEconomy::SetStockLimit(commander, std::strtoul(parts[1].c_str(), nullptr, 10), -1, nullptr);
        return;
    }

    // GETGATHERCATALOG / GETRECIPECATALOG: catalog queries backing the addon's icon-menu
    // pickers (see docs/addon-protocol.md) -- replies with several small GCAT:/RCAT: messages,
    // same "one guid placeholder, several chunked replies" shape as GUILDROSTER.
    if (verb == "GETGATHERCATALOG")
    {
        LOG_INFO("module.coa-playerbots", "BotAddonChat: '{}' -> GETGATHERCATALOG.", commander->GetName());
        for (std::string const& line : sBotMgr->GetGatherCatalog())
            SendCoaBotReply(commander, line);
        return;
    }
    if (verb == "GETRECIPECATALOG")
    {
        LOG_INFO("module.coa-playerbots", "BotAddonChat: '{}' -> GETRECIPECATALOG.", commander->GetName());
        for (std::string const& line : sBotMgr->GetRecipeCatalog(commander))
            SendCoaBotReply(commander, line);
        return;
    }

    // MAPQUERY / MAPDETAIL / MAPEVENTS: CoA World Map Intelligence
    if (verb == "MAPQUERY")
    {
        for (std::string const& line : sBotMapService->HandleMapQuery(commander, parts))
            SendCoaBotReply(commander, line);
        return;
    }
    if (verb == "MAPDETAIL")
    {
        for (std::string const& line : sBotMapService->HandleMapDetail(commander, parts))
            SendCoaBotReply(commander, line);
        return;
    }
    if (verb == "MAPEVENTS")
    {
        for (std::string const& line : sBotMapService->HandleMapEvents(commander, parts))
            SendCoaBotReply(commander, line);
        return;
    }

    ObjectGuid::LowType botGuidLow = std::strtoul(parts[1].c_str(), nullptr, 10);
    if (!botGuidLow)
    {
        LOG_INFO("module.coa-playerbots", "BotAddonChat: '{}' sent COABOT verb '{}' with unparseable/zero guid '{}' -- dropped.",
            commander->GetName(), verb, parts[1]);
        return;
    }

    Player* bot = ResolveAuthorizedBot(commander, botGuidLow);
    if (!bot)
    {
        LOG_DEBUG("module.coa-playerbots", "BotAddonChat: '{}' sent COABOT verb '{}' for guid {} but it failed authorization "
            "(not a tracked bot session, or not grouped with the sender) -- dropped.",
            commander->GetName(), verb, botGuidLow);
        return;
    }

    LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotAddonChat: '{}' -> bot '{}' verb '{}'{}{}.",
        commander->GetName(), bot->GetName(), verb,
        parts.size() >= 3 ? " arg='" : "", parts.size() >= 3 ? parts[2] + "'" : "");

    if (verb == "FOLLOW")
        BotAI::SetManualCommand(bot->GetGUID(), BotManualCommand::Follow);
    else if (verb == "STAY")
        BotAI::SetManualCommand(bot->GetGUID(), BotManualCommand::Stay);
    else if (verb == "PULL")
    {
        // The target is whatever the *commanding player* currently has selected, resolved
        // here server-side -- the wire message only ever carries the bot's own guid, per the
        // protocol doc's reasoning (no target-guid resolution needed on the addon side).
        if (Unit* target = commander->GetSelectedUnit())
            BotAI::SetManualCommand(bot->GetGUID(), BotManualCommand::Pull, target->GetGUID());
    }
    else if (verb == "STOPATTACK")
        BotAI::StopAttack(bot->GetGUID());
    else if (verb == "FARM")
    {
        // FARM:<botGuidLow>[:<radius>] -- the creature type is whatever the commanding player has
        // selected, resolved server-side exactly like PULL. Replies with the GETFARM shape.
        Unit* target = commander->GetSelectedUnit();
        Creature* creature = target ? target->ToCreature() : nullptr;
        if (creature && creature->GetMap() == bot->GetMap())
        {
            float radius = parts.size() >= 3 ? float(std::strtod(parts[2].c_str(), nullptr)) : 40.0f;
            if (radius <= 0.0f)
                radius = 40.0f;
            BotAI::StartFarm(bot, creature->GetEntry(), radius);
        }
        uint32 entry = 0, mapId = 0;
        float radius = 0.0f;
        BotAI::GetFarm(bot->GetGUID(), entry, radius, mapId);
        SendCoaBotReply(commander, "FARM:" + std::to_string(botGuidLow) + ":" + std::to_string(entry) + ":" + std::to_string(uint32(radius)));
    }
    else if (verb == "FARMSTOP")
    {
        BotAI::StopFarm(bot->GetGUID());
        SendCoaBotReply(commander, "FARM:" + std::to_string(botGuidLow) + ":0:0");
    }
    else if (verb == "GETFARM")
    {
        uint32 entry = 0, mapId = 0;
        float radius = 0.0f;
        BotAI::GetFarm(bot->GetGUID(), entry, radius, mapId);
        SendCoaBotReply(commander, "FARM:" + std::to_string(botGuidLow) + ":" + std::to_string(entry) + ":" + std::to_string(uint32(radius)));
    }
    else if (verb == "INVENTORY")
    {
        // INVENTORY:<botGuidLow> -- sell junk to a vendor within 20 yards. Never destroys
        // items in the field. Replies INVENTORY:<guid>:<copper>:<sold>:<destroyed>.
        BotAI::InventorySweepResult const result = BotAI::SweepInventory(bot, true, false);
        SendCoaBotReply(commander, "INVENTORY:" + std::to_string(botGuidLow) + ":" +
            std::to_string(result.copperEarned) + ":" + std::to_string(result.stacksSold) + ":" +
            std::to_string(result.stacksDestroyed));
        ChatHandler(commander->GetSession()).SendSysMessage(BotAI::DescribeInventorySweep(bot, result));
    }
    else if (verb == "SETROLE" && parts.size() >= 3)
        sBotMgr->SetRole(botGuidLow, parts[2], nullptr);
    else if (verb == "LEARNSPEC" && parts.size() >= 3)
        sBotMgr->LearnSpecialization(botGuidLow, std::strtoul(parts[2].c_str(), nullptr, 10), nullptr);
    else if (verb == "GETROLES")
    {
        // Query, not a command: lets the addon grey out role buttons a bot's class can never
        // actually hold (see SETROLE's silent-refusal note above) without needing its own copy
        // of the classId->role table -- the addon only ever sees the bot's underlying WoW
        // class, not its Ascension classId, so it has no way to derive this locally.
        static char const* const ROLE_NAMES[] = { "dps", "tank", "healer", "support" };
        uint32 mask = BotAI::GetAvailableRolesMask(bot->getClass());
        std::string roles;
        for (uint32 i = 0; i < 4; ++i)
        {
            if (mask & (1u << i))
            {
                if (!roles.empty())
                    roles += ",";
                roles += ROLE_NAMES[i];
            }
        }
        // Also tell the addon the bot's *current* effective role (not just which ones its
        // class could hold) -- otherwise a bot left on "auto" always just shows the literal
        // label "Auto" with no indication of what it's actually playing as right now.
        BotRole currentRole = BotAI::GetRole(bot->GetGUID());
        char const* currentRoleStr = ROLE_NAMES[currentRole == BotRole::Tank ? 1 : currentRole == BotRole::Healer ? 2 : currentRole == BotRole::Support ? 3 : 0];

        uint32 currentSpecId = bot->GetPlayerSetting("core.ascension_active_spec", 0).value;
        char const* currentSpecName = BotAI::GetSpecName(bot->getClass(), currentSpecId);
        std::string rolesHead = "ROLES:" + std::to_string(botGuidLow) + ":" + roles + ":" + currentRoleStr + ":" +
            std::to_string(currentSpecId) + ":";
        SendCoaBotReply(commander, rolesHead + WireText(currentSpecName ? currentSpecName : "", WireRoom(rolesHead)));
    }
    else if (verb == "GETSPECS")
    {
        static char const* const ROLE_NAMES[] = { "dps", "tank", "healer", "support" };
        for (BotAI::SpecInfo const& spec : BotAI::GetAllSpecs(bot->getClass()))
        {
            std::string specHead = "SPEC:" + std::to_string(botGuidLow) + ":" + std::to_string(spec.specId) + ":" +
                ROLE_NAMES[uint32(spec.role)] + ":";
            SendCoaBotReply(commander, specHead + WireText(spec.name ? spec.name : "", WireRoom(specHead)));
        }
    }
    else if (verb == "SETGEARPREF" && parts.size() >= 4)
    {
        bool weapon = parts[2] == "weapon";
        if (parts[2] != "armor" && !weapon)
            return;

        if (parts[3] == "auto")
        {
            sBotMgr->SetGearPreference(bot, weapon, 0, true);
            return;
        }

        uint32 subclass = 0;
        bool found = weapon
            ? GearSubclassByName(WEAPON_TYPES, (sizeof(WEAPON_TYPES) / sizeof(WEAPON_TYPES[0])), parts[3], subclass)
            : GearSubclassByName(ARMOR_TYPES, (sizeof(ARMOR_TYPES) / sizeof(ARMOR_TYPES[0])), parts[3], subclass);
        if (!found)
        {
            LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotAddonChat: '{}' sent SETGEARPREF with unrecognized {} type '{}' -- dropped.",
                commander->GetName(), weapon ? "weapon" : "armor", parts[3]);
            return;
        }

        sBotMgr->SetGearPreference(bot, weapon, subclass);
        LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "BotAddonChat: '{}' -> SETGEARPREF bot '{}' {} = {}.",
            commander->GetName(), bot->GetName(), weapon ? "weapon" : "armor", parts[3]);
    }
    else if (verb == "GETGEAR")
    {
        for (std::string const& line : sBotMgr->GetEquippedGearInfo(bot))
            SendCoaBotReply(commander, line);

        std::string legalArmor;
        for (uint32 subclass : sBotMgr->GetLegalArmorSubclasses(bot))
        {
            if (!legalArmor.empty())
                legalArmor += ",";
            legalArmor += GearNameBySubclass(ARMOR_TYPES, (sizeof(ARMOR_TYPES) / sizeof(ARMOR_TYPES[0])), subclass);
        }

        std::string legalWeapon;
        for (uint32 subclass : sBotMgr->GetLegalWeaponSubclasses(bot))
        {
            if (!legalWeapon.empty())
                legalWeapon += ",";
            legalWeapon += GearNameBySubclass(WEAPON_TYPES, (sizeof(WEAPON_TYPES) / sizeof(WEAPON_TYPES[0])), subclass);
        }

        uint32 armorPref = sBotMgr->GetGearPreference(bot, false);
        uint32 weaponPref = sBotMgr->GetGearPreference(bot, true);
        std::string armorPrefName = armorPref == 0 ? "auto" : GearNameBySubclass(ARMOR_TYPES, (sizeof(ARMOR_TYPES) / sizeof(ARMOR_TYPES[0])), armorPref);
        std::string weaponPrefName = weaponPref == 0 ? "auto" : GearNameBySubclass(WEAPON_TYPES, (sizeof(WEAPON_TYPES) / sizeof(WEAPON_TYPES[0])), weaponPref - 1);

        SendCoaBotReply(commander, "GEARPREFS:" + std::to_string(botGuidLow) + ":" + legalArmor + ":" + legalWeapon +
            ":" + armorPrefName + ":" + weaponPrefName);
        SendCoaBotReply(commander, "GEARLOCKS:" + std::to_string(botGuidLow) + ":" +
            std::to_string(BotAI::GetLockedEquipMask(bot->GetGUID())));
    }
    else if (verb == "GETBAGS")
    {
        // GETBAGS:<botGuidLow> -- occupied backpack and bag slots only, no item names (the
        // client resolves those with GetItemInfo). Replies are paged so each body stays under
        // the 255-character addon-whisper limit. Shape:
        //   BAGMETA:<guid>:<backpackSlots>:<bag19>:<bag20>:<bag21>:<bag22>
        //   BAGS:<guid>:<page>:<pages>:<bag>:<slot>:<itemId>:<count>|...
        //   BAGSEND:<guid>:<itemCount>
        uint8 backpackSlots = 0;
        uint8 bagSizes[4] = {};
        BotAI::CollectBagSizes(bot, backpackSlots, bagSizes);
        SendCoaBotReply(commander, "BAGMETA:" + std::to_string(botGuidLow) + ":" +
            std::to_string(uint32(backpackSlots)) + ":" +
            std::to_string(uint32(bagSizes[0])) + ":" + std::to_string(uint32(bagSizes[1])) + ":" +
            std::to_string(uint32(bagSizes[2])) + ":" + std::to_string(uint32(bagSizes[3])));

        std::vector<BotAI::BagSlotInfo> items;
        BotAI::CollectBagSlots(bot, items);

        // "COABOT\t" is prepended by SendCoaBotReply, so keep the verb body comfortably
        // under 255. A single item chunk is at most "255:38:4294967295:4294967295|" (34).
        constexpr std::size_t MAX_BODY = 220;
        std::string const prefix = "BAGS:" + std::to_string(botGuidLow) + ":";
        std::vector<std::string> pages;
        std::string page;
        for (BotAI::BagSlotInfo const& info : items)
        {
            std::string chunk = std::to_string(uint32(info.bag)) + ":" + std::to_string(uint32(info.slot)) + ":" +
                std::to_string(info.entry) + ":" + std::to_string(info.count) + "|";
            // Reserve room for the prefix plus "99:99:" page numbers.
            if (!page.empty() && prefix.size() + 6 + page.size() + chunk.size() > MAX_BODY)
            {
                pages.push_back(page);
                page.clear();
            }
            page += chunk;
        }
        if (!page.empty() || pages.empty())
            pages.push_back(page);

        uint32 const pageCount = uint32(pages.size());
        for (uint32 i = 0; i < pageCount; ++i)
        {
            // Skip a completely empty first page when there were no items -- BAGSEND still fires.
            if (pages[i].empty() && items.empty())
                continue;
            SendCoaBotReply(commander, prefix + std::to_string(i + 1) + ":" + std::to_string(pageCount) + ":" + pages[i]);
        }
        SendCoaBotReply(commander, "BAGSEND:" + std::to_string(botGuidLow) + ":" + std::to_string(items.size()));
    }
    else if (verb == "EQUIP" && parts.size() >= 4)
    {
        // EQUIP:<botGuidLow>:<bag>:<slot> -- bag 255 is the backpack, 19-22 an equipped bag.
        uint8 bag = uint8(std::strtoul(parts[2].c_str(), nullptr, 10));
        uint8 slot = uint8(std::strtoul(parts[3].c_str(), nullptr, 10));
        BotAI::GearActionResult result = BotAI::EquipBagItem(bot, bag, slot);
        char const* token = BotAI::GearActionToken(result);
        SendCoaBotReply(commander, std::string("EQUIPRESULT:") + std::to_string(botGuidLow) + ":" +
            (result == BotAI::GearActionResult::Ok ? "ok:" : "err:") + token);
    }
    else if (verb == "UNEQUIP" && parts.size() >= 3)
    {
        // UNEQUIP:<botGuidLow>:<equipSlot> -- moves the piece into bags if there is room and
        // locks the emptied slot so auto-upgrade does not immediately refill it.
        uint8 equipSlot = uint8(std::strtoul(parts[2].c_str(), nullptr, 10));
        BotAI::GearActionResult result = BotAI::UnequipToBags(bot, equipSlot);
        char const* token = BotAI::GearActionToken(result);
        SendCoaBotReply(commander, std::string("UNEQUIPRESULT:") + std::to_string(botGuidLow) + ":" +
            (result == BotAI::GearActionResult::Ok ? "ok:" : "err:") + token);
    }
    else if (verb == "UNLOCK" && parts.size() >= 3)
    {
        // UNLOCK:<botGuidLow>:<slot|all> -- clears a manual equipment lock so TryUpgradeGearOnce
        // may fill or replace that slot again. "all" clears every lock on this bot.
        if (parts[2] == "all")
            BotAI::UnlockAllEquipSlots(bot->GetGUID());
        else
            BotAI::UnlockEquipSlot(bot->GetGUID(), uint8(std::strtoul(parts[2].c_str(), nullptr, 10)));
        SendCoaBotReply(commander, "UNLOCKRESULT:" + std::to_string(botGuidLow) + ":ok:ok");
        SendCoaBotReply(commander, "GEARLOCKS:" + std::to_string(botGuidLow) + ":" +
            std::to_string(BotAI::GetLockedEquipMask(bot->GetGUID())));
    }
    else if (verb == "GETQUESTS")
    {
        // GETQUESTS:<botGuidLow> -- the bot's quest log with objective progress:
        //   QUESTHDR:<guid>:<questCount>:<botLevel>
        //   QUEST:<guid>:<questId>:<level>:<status 0 in progress|1 complete|2 failed>:<title>
        //   QOBJ:<guid>:<questId>:<c|g|i|p|e>:<entry>:<have>:<need>:<name>
        //   QUESTEND:<guid>:<questCount>
        SendBotQuestLog(commander, bot, botGuidLow);
    }
    else if (verb == "GETPROFS")
    {
        // GETPROFS:<botGuidLow>
        //   PROFALLOWED:<guid>:<limit>:<skillId>,...   limit 0 = CoA's uncapped list
        //   PROFS:<guid>:<skillId>:<value>:<max>|...   (PROFSMORE:<guid>:... if it would not fit)
        SendProfessionLists(commander, bot, botGuidLow);
    }
    else if (verb == "SETPROF" && parts.size() >= 3)
    {
        // SETPROF:<botGuidLow>:<skillId>              learn
        // SETPROF:<botGuidLow>:none:<skillId>         unlearn (wipes the skill)
        // SETPROF:<botGuidLow>:<skillId>:0            unlearn (alternate)
        // SETPROF:<botGuidLow>:<1|2>:<skillId|none>   same learn/unlearn; the slot is ignored
        //                                             because CoA does not cap primary professions.
        bool unlearn = false;
        uint32 skillId = 0;
        if (parts[2] == "none")
        {
            unlearn = true;
            if (parts.size() >= 4)
                skillId = uint32(std::strtoul(parts[3].c_str(), nullptr, 10));
        }
        else if (parts.size() >= 4 && (parts[2] == "1" || parts[2] == "2"))
        {
            if (parts[3] == "none")
                unlearn = true; // no slot-to-skill mapping; treat as invalid unless a third id follows
            else
                skillId = uint32(std::strtoul(parts[3].c_str(), nullptr, 10));
        }
        else
        {
            skillId = uint32(std::strtoul(parts[2].c_str(), nullptr, 10));
            if (parts.size() >= 4 && parts[3] == "0")
                unlearn = true;
        }

        char const* token = "invalid";
        if (unlearn && !skillId)
            token = "invalid";
        else if (unlearn)
            token = BotProgression::UnlearnProfession(bot, skillId);
        else
            token = BotProgression::LearnProfession(bot, skillId);

        bool ok = token && std::string(token) == "ok";
        // A player chose this set by hand; login auto-reroll must not overwrite it.
        if (ok)
            BotProgression::MarkProfessionsManual(bot);
        // PROFRESULT:<guid>:<ok|err>:<token>:<skillId>:<learn|unlearn>:<value>:<max>
        // (the last three are new; older addons read only the first five fields).
        SendCoaBotReply(commander, std::string("PROFRESULT:") + std::to_string(botGuidLow) + ":" +
            (ok ? "ok:" : "err:") + (token ? token : "invalid") + ":" + std::to_string(skillId) + ":" +
            (unlearn ? "unlearn" : "learn") + ":" + std::to_string(uint32(bot->GetSkillValue(skillId))) + ":" +
            std::to_string(uint32(bot->GetMaxSkillValue(skillId))));
        LOG_INFO("module.coa-playerbots", "BotAddonChat: '{}' -> SETPROF bot '{}' {} skill {} = {}.",
            commander->GetName(), bot->GetName(), unlearn ? "unlearn" : "learn", skillId, token ? token : "invalid");
        // Refresh the addon list after every attempt, so the buttons always show the bot's real state.
        SendProfessionLists(commander, bot, botGuidLow);
    }
}

class coa_bot_addon_chat_script : public PlayerScript
{
public:
    coa_bot_addon_chat_script() : PlayerScript("coa_bot_addon_chat_script", { PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT }) { }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& message, Player* receiver) override
    {
        if (!sBotMgr->BotsEnabled())
            return true;

        if (type != CHAT_MSG_WHISPER || language != LANG_ADDON || receiver != player)
            return true;
        if (message.compare(0, PROTOCOL_PREFIX_LEN, PROTOCOL_PREFIX) != 0)
            return true;

        HandleCoaBotMessage(player, message.substr(PROTOCOL_PREFIX_LEN));
        return false; // consume -- a self-whisper carrying our protocol never needs real delivery
    }
};
}

void AddSC_coa_bot_addon_chat_script()
{
    new coa_bot_addon_chat_script();
}
