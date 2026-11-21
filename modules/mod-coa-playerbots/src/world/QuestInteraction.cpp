#include "engine/BotDebugLog.h"
#include "QuestInteraction.h"
#include "BotAI.h"
#include "BotMgr.h"
#include "Chat.h"
#include "CellImpl.h"
#include "Creature.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "IQuestObjectiveHandler.h"
#include "ItemTemplate.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "ObjectiveCommon.h"
#include "Opcodes.h"
#include "Player.h"
#include "QuestDef.h"
#include "QuestKnowledgeBase.h"
#include "QuestPackets.h"
#include "QuestPolicy.h"
#include "StringFormat.h"
#include "Log.h"
#include "WorldPacket.h"
#include "WorldPlanner.h"
#include "WorldSession.h"
#include <cmath>
#include <vector>

using namespace WorldBrainInternal;

namespace
{
    // Reading the quest text and clicking through the dialog: a player doesn't turn in and accept
    // in the same frame they arrive, and neither does a bot.
    constexpr uint32 READ_MIN_MS = 800;
    constexpr uint32 READ_MAX_MS = 2600;
    constexpr uint32 NPC_SEARCH_MS = 20000;
    constexpr float NPC_SEARCH_RADIUS = 50.0f;

    ExecResult FailNpc(Player* bot, BrainState& state, FailureReason reason)
    {
        WorldTask& task = state.task;
        WorldBrainConfig const& cfg = WorldBrainSettings::Get();
        if (task.npcSpawnId)
            state.failures.Remember(FailKind::Npc, task.npcSpawnId, NowMs(), cfg.npcFailMs, uint8(reason));
        task.quest.lastFailure = reason;
        LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "Bot '{}' could not deal with quest npc {} (spawn {}): {}.", bot->GetName(),
            task.npcEntry, task.npcSpawnId, FailureReasonName(reason));
        return ExecResult::Failed;
    }

    WorldObject* ResolveNpc(Player* bot, WorldTask const& task)
    {
        if (task.npcGuid.IsEmpty())
            return nullptr;
        if (task.npcIsGameObject)
            return ObjectAccessor::GetGameObject(*bot, task.npcGuid);
        Creature* creature = ObjectAccessor::GetCreature(*bot, task.npcGuid);
        return creature && creature->IsAlive() ? creature : nullptr;
    }

    QuestRelationBounds InvolvedBounds(Object* giver)
    {
        return giver->GetTypeId() == TYPEID_UNIT ? sObjectMgr->GetCreatureQuestInvolvedRelationBounds(giver->GetEntry())
            : sObjectMgr->GetGOQuestInvolvedRelationBounds(giver->GetEntry());
    }

    QuestRelationBounds OfferedBounds(Object* giver)
    {
        return giver->GetTypeId() == TYPEID_UNIT ? sObjectMgr->GetCreatureQuestRelationBounds(giver->GetEntry())
            : sObjectMgr->GetGOQuestRelationBounds(giver->GetEntry());
    }
}

namespace QuestInteraction
{
    uint32 ActiveQuestCount(Player* bot)
    {
        uint32 count = 0;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            if (bot->GetQuestSlotQuestId(slot))
                ++count;
        return count;
    }

    bool WouldAccept(Player* bot, BrainState& state, Quest const* quest, char const*& reason)
    {
        WorldBrainConfig const& cfg = WorldBrainSettings::Get();
        uint32 questId = quest->GetQuestId();
        QuestKnowledge const* info = QuestKB::Get(questId);
        if (!info)
        {
            reason = "unknown quest";
            return false;
        }
        if (!info->supported)
        {
            reason = info->unsupportedReason;
            return false;
        }
        if (info->elite && !cfg.acceptElite)
        {
            reason = "elite";
            return false;
        }
        if (state.failures.Has(FailKind::Quest, questId, NowMs()))
        {
            reason = "suspended";
            return false;
        }
        if (bot->GetQuestStatus(questId) != QUEST_STATUS_NONE)
        {
            reason = "already taken";
            return false;
        }

        int32 level = int32(bot->GetLevel());
        int32 questLevel = quest->GetQuestLevel() > 0 ? quest->GetQuestLevel() : level;
        if (questLevel + cfg.questLevelBelow < level)
        {
            reason = "too low";
            return false;
        }
        if (questLevel > level + cfg.questLevelAbove)
        {
            reason = "too high";
            return false;
        }
        if (ActiveQuestCount(bot) >= cfg.maxActiveQuests)
        {
            reason = "quest log full";
            return false;
        }
        if (!bot->CanTakeQuest(quest, false) || !bot->CanAddQuest(quest, false))
        {
            reason = "not eligible";
            return false;
        }
        int32 money = quest->GetRewOrReqMoney(bot->GetLevel());
        if (money < 0 && bot->GetMoney() < uint32(-money))
        {
            reason = "costs money";
            return false;
        }
        if (!TakeableByThisBuild(*info))
        {
            reason = "an objective this build cannot do";
            return false;
        }
        for (ObjectiveDef const& def : info->objectives)
        {
            if (!WorldPlanner::ObjectiveReachable(bot, state, *info, def))
            {
                reason = "objective out of reach";
                return false;
            }
        }
        if (!WorldPlanner::EnderReachable(bot, *info))
        {
            reason = "handed in elsewhere";
            return false;
        }
        return true;
    }

    bool TakeableByThisBuild(QuestKnowledge const& info)
    {
        if (!info.supported)
            return false;
        for (ObjectiveDef const& def : info.objectives)
        {
            // Handed out on accept: nothing to do for it.
            if (def.providedByQuest)
                continue;
            if (!def.supported || !ObjectiveHandlers::CanExecute(def))
                return false;
        }
        return true;
    }

    bool Workable(Player* bot, uint32 questId, QuestKnowledge const& info, char const** why)
    {
        uint32 open = 0;
        uint32 executable = 0;
        for (ObjectiveDef const& def : info.objectives)
        {
            if (ObjectiveCommon::IsDone(bot, questId, def))
                continue;
            ++open;
            if (ObjectiveHandlers::CanExecute(def))
                ++executable;
        }
        bool workable = QuestPolicy::Workable(info.completable, open, executable);
        if (!workable && why)
            *why = !info.completable ? info.completionBlocker
                 : "an open objective this build cannot do (no handler, or a quest-provided item that is gone)";
        return workable;
    }

    uint32 PickRewardIndex(Player* bot, Quest const* quest)
    {
        if (quest->GetRewChoiceItemsCount() <= 1)
            return 0;

        BotRole role = BotAI::GetRole(bot->GetGUID());
        uint32 bestIndex = 0;
        float bestScore = -1e9f;

        for (uint32 i = 0; i < QUEST_REWARD_CHOICES_COUNT; ++i)
        {
            uint32 itemId = quest->RewardChoiceItemId[i];
            if (!itemId)
                continue;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
            if (!proto)
                continue;

            // Unusable (wrong armour type, wrong weapon, class-locked): only worth its vendor price.
            float score = float(proto->SellPrice) / 100.0f;
            if (proto->InventoryType != INVTYPE_NON_EQUIP && bot->CanUseItem(proto) == EQUIP_ERR_OK)
            {
                uint8 slot = bot->FindEquipSlot(proto, NULL_SLOT, true);
                if (slot != NULL_SLOT)
                {
                    float candidate = BotAI::ScoreItemForBot(bot, proto, role);
                    float current = 0.0f;
                    if (Item* equipped = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                        if (ItemTemplate const* equippedProto = equipped->GetTemplate())
                            current = BotAI::ScoreItemForBot(bot, equippedProto, role);
                    // Usable items always beat vendor trash; among them the biggest upgrade wins.
                    score = 100000.0f + (candidate - current);
                }
            }

            if (score > bestScore)
            {
                bestScore = score;
                bestIndex = i;
            }
        }
        return bestIndex;
    }

    GiverResult ProcessGiver(Player* bot, BrainState& state, Object* giver)
    {
        GiverResult result;

        QuestRelationBounds involved = InvolvedBounds(giver);
        for (auto itr = involved.first; itr != involved.second; ++itr)
        {
            uint32 questId = itr->second;
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest || bot->GetQuestStatus(questId) == QUEST_STATUS_NONE || bot->GetQuestRewardStatus(questId))
                continue;

            // Report/deliver quests only become complete here: the client's quest dialog does
            // exactly this when it opens on the ender.
            if (bot->CanCompleteQuest(questId))
                bot->CompleteQuest(questId);
            if (bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
            {
                result.why += Acore::StringFormat(" [quest {} '{}' is not complete, status {}]", questId, quest->GetTitle(), uint32(bot->GetQuestStatus(questId)));
                continue;
            }

            uint32 reward = PickRewardIndex(bot, quest);
            // Full bags are the common reason a finished quest cannot be handed in: make room first.
            if (!bot->CanRewardQuest(quest, reward, false) && bot->GetFreeInventorySpace() < 3)
                BotAI::FreeBagSlots(bot, 3);
            if (!bot->CanRewardQuest(quest, reward, false))
            {
                ++result.rewardBlocked;
                result.why += Acore::StringFormat(" [quest {} '{}' cannot be rewarded: choice {} of {}, {} free bag slots]", questId, quest->GetTitle(),
                    reward, quest->GetRewChoiceItemsCount(), bot->GetFreeInventorySpace());
                continue;
            }

            bot->RewardQuest(quest, reward, giver, true);
            ++result.turnedIn;
            Count(state.metrics, &WorldMetrics::questsTurnedIn);
            state.loopDetector.OnProgress(NowMs());
            state.questSuspensions.erase(questId);
            LOG_INFO("module.coa-playerbots.quests", "QuestTurnIn: '{}' (level {}) handed in quest {} '{}' (quest level {}) to {} {}, reward choice {}, money {}.",
                bot->GetName(), uint32(bot->GetLevel()), questId, quest->GetTitle(), quest->GetQuestLevel(),
                giver->GetTypeId() == TYPEID_UNIT ? "creature" : "object", giver->GetEntry(), reward, quest->GetRewOrReqMoney());
            LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "Bot '{}' QuestTurnedIn {} ('{}'), reward choice {}.", bot->GetName(), questId,
                quest->GetTitle(), reward);
        }

        QuestRelationBounds offered = OfferedBounds(giver);
        for (auto itr = offered.first; itr != offered.second; ++itr)
        {
            uint32 questId = itr->second;
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;

            char const* reason = "";
            if (!WouldAccept(bot, state, quest, reason))
            {
                LOG_TRACE("module.coa-playerbots.quest", "Bot '{}' passes on quest {} ('{}'): {}.", bot->GetName(), questId,
                    quest->GetTitle(), reason);
                continue;
            }

            bot->AddQuestAndCheckCompletion(quest, giver);
            if (uint32 spell = quest->GetSrcSpell())
                bot->CastSpell(bot, spell, true);
            ++result.accepted;
            Count(state.metrics, &WorldMetrics::questsAccepted);
            state.loopDetector.OnProgress(NowMs());
            LOG_INFO("module.coa-playerbots.quests", "QuestAccepted: '{}' (level {}) took quest {} '{}' (quest level {}) from {} {}.", bot->GetName(),
                uint32(bot->GetLevel()), questId, quest->GetTitle(), quest->GetQuestLevel(), giver->GetTypeId() == TYPEID_UNIT ? "creature" : "object", giver->GetEntry());
            LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "Bot '{}' QuestAccepted {} ('{}', level {}).", bot->GetName(), questId,
                quest->GetTitle(), quest->GetQuestLevel());
        }

        return result;
    }

    void Abandon(Player* bot, uint32 questId)
    {
        uint16 slot = bot->FindQuestSlot(questId);
        if (slot >= MAX_QUEST_LOG_SIZE)
            return;

        WorldPacket data(CMSG_QUESTLOG_REMOVE_QUEST, 1);
        WorldPackets::Quest::QuestLogRemoveQuest packet(std::move(data));
        packet.Slot = uint8(slot);
        bot->GetSession()->HandleQuestLogRemoveQuest(packet);
        LOG_DEBUG(BotAI::BotDebugLog::LoggerName(bot->GetGUID()), "Bot '{}' abandoned quest {}.", bot->GetName(), questId);
    }

    uint32 PhaseBudgetMs(BrainState const& state)
    {
        WorldBrainConfig const& cfg = WorldBrainSettings::Get();
        switch (state.task.phase)
        {
            case TaskPhase::TravelToArea: return cfg.travelTimeoutMs;
            case TaskPhase::Search:       return NPC_SEARCH_MS;
            case TaskPhase::Approach:     return cfg.approachTimeoutMs;
            default:                      return 0;
        }
    }

    ExecResult UpdateNpcTask(Player* bot, BrainState& state)
    {
        WorldTask& task = state.task;
        WorldBrainConfig const& cfg = WorldBrainSettings::Get();
        uint32 now = NowMs();

        switch (task.phase)
        {
            case TaskPhase::Planning:
            case TaskPhase::Recover:
                SetPhase(bot, state, TaskPhase::TravelToArea, "heading to quest npc");
                [[fallthrough]];
            case TaskPhase::TravelToArea:
            {
                float dist = std::hypot(bot->GetPositionX() - task.x, bot->GetPositionY() - task.y);
                float distZ = std::fabs(bot->GetPositionZ() - task.z);
                if (bot->GetMapId() == task.mapId && dist <= 20.0f && (task.z == 0.0f || distZ <= 15.0f))
                {
                    SetPhase(bot, state, TaskPhase::Search, "at the npc's spot");
                    return ExecResult::Running;
                }
                if (PhaseElapsed(state) > cfg.travelTimeoutMs)
                    return FailNpc(bot, state, FailureReason::Timeout);
                if (!task.taxiRequested && dist > cfg.taxiMinDistance)
                {
                    task.taxiRequested = true;
                    if (WorldExecutor::TryRequestFlight(bot, state, task.x, task.y, task.z))
                        return ExecResult::Running;
                }
                NavStatus status = WorldExecutor::TravelTo(bot, state, WorldGoalSub::Npc, task.x, task.y, task.z, 12.0f);
                if (status == NavStatus::Arrived)
                    SetPhase(bot, state, TaskPhase::Search, "at the npc's spot");
                else if (status == NavStatus::Stuck)
                {
                    Count(state.metrics, &WorldMetrics::movementStuck);
                    return FailNpc(bot, state, FailureReason::Unreachable);
                }
                return ExecResult::Running;
            }
            case TaskPhase::Search:
            {
                WorldObject* npc = task.npcIsGameObject
                    ? static_cast<WorldObject*>(bot->FindNearestGameObject(task.npcEntry, NPC_SEARCH_RADIUS))
                    : static_cast<WorldObject*>(bot->FindNearestCreature(task.npcEntry, NPC_SEARCH_RADIUS, true));
                if (npc)
                {
                    task.npcGuid = npc->GetGUID();
                    SetPhase(bot, state, TaskPhase::Approach, "found the npc");
                }
                else if (PhaseElapsed(state) > NPC_SEARCH_MS)
                    return FailNpc(bot, state, FailureReason::NoTargets);
                return ExecResult::Running;
            }
            case TaskPhase::Approach:
            {
                WorldObject* npc = ResolveNpc(bot, task);
                if (!npc)
                {
                    SetPhase(bot, state, TaskPhase::Search, "npc gone");
                    return ExecResult::Running;
                }
                float dist = bot->GetDistance(npc);
                if (dist <= INTERACTION_DISTANCE - 1.0f)
                {
                    // Administrative stop: close enough to NPC to begin dialogue interaction
                    BotMovement::ForceStopOwner(bot, MoveOwner::Quest);
                    if (bot->isMoving())
                        return ExecResult::Running;

                    WorldExecutor::Dismount(bot, state);
                    bot->SetFacingToObject(npc);
                    task.waitUntilMs = now + RollRange(state, 0x4ead, READ_MIN_MS, READ_MAX_MS);
                    SetPhase(bot, state, TaskPhase::Execute, "talking");
                    return ExecResult::Running;
                }
                if (PhaseElapsed(state) > cfg.approachTimeoutMs)
                    return FailNpc(bot, state, FailureReason::Unreachable);
                NavStatus status = WorldExecutor::TravelTo(bot, state, WorldGoalSub::Npc, npc->GetPositionX(), npc->GetPositionY(),
                    npc->GetPositionZ(), INTERACTION_DISTANCE - 2.0f, npc->GetGUID().GetCounter(), dist > 60.0f);
                if (status == NavStatus::Stuck)
                    return FailNpc(bot, state, FailureReason::Unreachable);
                return ExecResult::Running;
            }
            case TaskPhase::Execute:
            {
                if (now < task.waitUntilMs)
                    return ExecResult::Running;

                Object* giver = task.npcIsGameObject
                    ? static_cast<Object*>(bot->GetGameObjectIfCanInteractWith(task.npcGuid, GAMEOBJECT_TYPE_QUESTGIVER))
                    : static_cast<Object*>(bot->GetNPCIfCanInteractWith(task.npcGuid, UNIT_NPC_FLAG_QUESTGIVER));
                if (!giver)
                {
                    if (task.quest.retryCount++ < 2)
                    {
                        SetPhase(bot, state, TaskPhase::Approach, "not in reach to talk");
                        return ExecResult::Running;
                    }
                    return FailNpc(bot, state, FailureReason::InteractFailed);
                }

                GiverResult result = ProcessGiver(bot, state, giver);
                NoteEvent(state, Acore::StringFormat("at quest npc {}: {} handed in, {} accepted", task.npcEntry, result.turnedIn,
                    result.accepted));

                if (!result.turnedIn && !result.accepted)
                {
                    // Why nothing was handed in: the quest asked for is not one this giver takes, is not complete, or cannot be rewarded.
                    std::string why = result.why;
                    if (task.type == WorldTaskType::QuestTurnIn && task.quest.questId)
                    {
                        QuestRelationBounds taken = InvolvedBounds(giver);
                        bool takes = false;
                        for (auto itr = taken.first; itr != taken.second; ++itr)
                            takes = takes || itr->second == task.quest.questId;
                        if (!takes)
                            why += Acore::StringFormat(" [npc {} does not take quest {} (spawn {}, distance {:.0f})]", task.npcEntry, task.quest.questId,
                                task.npcSpawnId, bot->GetDistance(static_cast<WorldObject*>(giver)));
                    }
                    LOG_INFO("module.coa-playerbots.quests", "QuestTurnInFailed: '{}' (level {}) at {} {}:{}", bot->GetName(), uint32(bot->GetLevel()),
                        giver->GetTypeId() == TYPEID_UNIT ? "creature" : "object", giver->GetEntry(), why.empty() ? " nothing to hand in or accept" : why);
                    if (result.rewardBlocked && task.quest.questId)
                        state.failures.Remember(FailKind::TurnIn, task.quest.questId, now, 120000, uint8(FailureReason::InteractFailed));
                    return FailNpc(bot, state, FailureReason::InteractFailed);
                }

                task.waitUntilMs = now + RollRange(state, 0x60de, 600, 2200);
                SetPhase(bot, state, TaskPhase::Verify, "done talking");
                return ExecResult::Running;
            }
            case TaskPhase::Verify:
                return now < task.waitUntilMs ? ExecResult::Running : ExecResult::Completed;
            default:
                return ExecResult::Failed;
        }
    }

    constexpr float COMPANION_SHARE_RANGE = 40.0f;

    class NearbyQuestCreatureCheck
    {
    public:
        NearbyQuestCreatureCheck(Player* player, uint32 questId, float range, bool turnIn)
            : _player(player), _questId(questId), _range(range), _turnIn(turnIn) { }

        bool operator()(Creature* creature)
        {
            if (!creature->IsAlive())
                return false;
            if (_turnIn ? !creature->hasInvolvedQuest(_questId) : !creature->hasQuest(_questId))
                return false;
            if (!_player->IsWithinDistInMap(creature, _range))
                return false;
            _range = _player->GetDistance(creature);
            return true;
        }

    private:
        Player* _player;
        uint32 _questId;
        float _range;
        bool _turnIn;
    };

    class NearbyQuestGameObjectCheck
    {
    public:
        NearbyQuestGameObjectCheck(Player* player, uint32 questId, float range, bool turnIn)
            : _player(player), _questId(questId), _range(range), _turnIn(turnIn) { }

        bool operator()(GameObject* object)
        {
            if (_turnIn ? !object->hasInvolvedQuest(_questId) : !object->hasQuest(_questId))
                return false;
            if (!_player->IsWithinDistInMap(object, _range))
                return false;
            _range = _player->GetDistance(object);
            return true;
        }

    private:
        Player* _player;
        uint32 _questId;
        float _range;
        bool _turnIn;
    };

    Object* FindNearbyQuestObject(Player* player, uint32 questId, bool turnIn)
    {
        if (Unit* selected = player->GetSelectedUnit())
        {
            if (Creature* creature = selected->ToCreature())
            {
                bool matches = turnIn ? creature->hasInvolvedQuest(questId) : creature->hasQuest(questId);
                if (creature->IsAlive() && matches && player->IsWithinDistInMap(creature, COMPANION_SHARE_RANGE))
                    return creature;
            }
        }

        Creature* creature = nullptr;
        NearbyQuestCreatureCheck creatureCheck(player, questId, COMPANION_SHARE_RANGE, turnIn);
        Acore::CreatureLastSearcher<NearbyQuestCreatureCheck> creatureSearcher(player, creature, creatureCheck);
        Cell::VisitObjects(player, creatureSearcher, COMPANION_SHARE_RANGE);

        GameObject* object = nullptr;
        NearbyQuestGameObjectCheck objectCheck(player, questId, COMPANION_SHARE_RANGE, turnIn);
        Acore::GameObjectLastSearcher<NearbyQuestGameObjectCheck> objectSearcher(player, object, objectCheck);
        Cell::VisitObjects(player, objectSearcher, COMPANION_SHARE_RANGE);

        if (creature && object)
            return player->GetDistance(creature) <= player->GetDistance(object) ? static_cast<Object*>(creature) : static_cast<Object*>(object);
        if (creature)
            return creature;
        if (object)
            return object;
        return player;
    }

    bool IsCompanionShareSource(Player* player)
    {
        // A bot can sit on the commander's own account (alts logged in through the addon), so the
        // account name alone does not tell a bot from the player: also ask BotMgr directly.
        return player && player->IsAlive() && player->IsInWorld() && player->GetSession()
            && !BotMgr::IsBotAccountId(player->GetSession()->GetAccountId())
            && !sBotMgr->FindBotPlayer(player->GetGUID().GetCounter())
            && player->GetGroup();
    }

    template<typename Fn>
    void ForNearbyGroupedBots(Player* player, Fn const& fn)
    {
        Group* group = player->GetGroup();
        if (!group)
            return;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == player || !member->IsAlive() || !member->IsInWorld())
                continue;
            if (member->IsBeingTeleported() || !sBotMgr->FindBotPlayer(member->GetGUID().GetCounter()))
                continue;
            if (member->GetMap() != player->GetMap() || !member->IsWithinDistInMap(player, COMPANION_SHARE_RANGE))
                continue;
            fn(member);
        }
    }

    // Grouped bots follow the commander, so their own quest brain is suspended (BotAI.cpp:
    // WorldBrain::Suspend(..., SuspendReason::Grouped)) and they never walk to an ender on
    // their own. Every hand-in a grouped bot ever makes comes from the commander's actions
    // below. RewardQuest is called directly, so a bot does not need to be in the 5-yard
    // interaction range of the NPC -- only in the same zone of the fight as the commander.
    constexpr float COMPANION_TURNIN_RANGE = 80.0f;

    enum class HandInResult
    {
        Done,
        NotTaken,
        AlreadyRewarded,
        NotComplete,
        BagsFull,
        Blocked,
    };

    // How chatty a hand-in pass is towards the commander.
    enum class HandInReport
    {
        Everything,   // the commander handed in this exact quest: say why a bot could not
        Problems,     // the commander talked to an ender: successes and real blockers only
        SuccessOnly,  // the end-of-RewardQuest safety net: anything else was said already
    };

    bool HasRewardItems(Quest const* quest)
    {
        return quest->GetRewChoiceItemsCount() > 0 || quest->GetRewItemsCount() > 0;
    }

    HandInResult TryHandIn(Player* bot, Quest const* quest, Object* giver, uint32& rewardOut)
    {
        uint32 const questId = quest->GetQuestId();
        if (bot->GetQuestStatus(questId) == QUEST_STATUS_NONE)
            return bot->GetQuestRewardStatus(questId) ? HandInResult::AlreadyRewarded : HandInResult::NotTaken;
        if (bot->GetQuestRewardStatus(questId))
            return HandInResult::AlreadyRewarded;

        // Report/deliver quests become complete when their dialog opens on the ender; this is
        // the same step for a bot that never opens the dialog itself.
        if (bot->CanCompleteQuest(questId))
            bot->CompleteQuest(questId);
        if (bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
            return HandInResult::NotComplete;

        uint32 reward = PickRewardIndex(bot, quest);
        rewardOut = reward;
        if (!bot->CanRewardQuest(quest, reward, false) && HasRewardItems(quest) && bot->GetFreeInventorySpace() < 3)
            BotAI::FreeBagSlots(bot, 3);
        if (!bot->CanRewardQuest(quest, reward, false))
        {
            LOG_INFO("module.coa-playerbots.quests", "QuestSharedTurnInBlocked: '{}' could not hand in quest {} '{}' (choice {}, {} free bag slots).",
                bot->GetName(), questId, quest->GetTitle(), reward, bot->GetFreeInventorySpace());
            return HasRewardItems(quest) ? HandInResult::BagsFull : HandInResult::Blocked;
        }

        bot->RewardQuest(quest, reward, giver, true);
        return HandInResult::Done;
    }

    std::string RewardNote(Quest const* quest, uint32 reward)
    {
        if (quest->GetRewChoiceItemsCount() == 0 || reward >= QUEST_REWARD_CHOICES_COUNT)
            return "";
        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(quest->RewardChoiceItemId[reward]))
            return Acore::StringFormat(", took {}", proto->Name1);
        return "";
    }

    void Tell(Player* player, std::string const& text)
    {
        if (player && player->GetSession())
            ChatHandler(player->GetSession()).SendSysMessage("AltBots: " + text);
    }

    // Completed quests this bot could hand in at `giver` right now (status already complete,
    // or a report/deliver quest that completes on talking to the ender).
    bool HasFinishedQuestFor(Player* bot, Object* giver, Quest const* only)
    {
        auto finished = [bot](uint32 questId)
        {
            QuestStatus const status = bot->GetQuestStatus(questId);
            if (status == QUEST_STATUS_NONE || bot->GetQuestRewardStatus(questId))
                return false;
            return status == QUEST_STATUS_COMPLETE || bot->CanCompleteQuest(questId);
        };
        if (only)
            return finished(only->GetQuestId());
        if (!giver || giver == bot)
            return false;
        QuestRelationBounds involved = InvolvedBounds(giver);
        for (auto itr = involved.first; itr != involved.second; ++itr)
            if (finished(itr->second))
                return true;
        return false;
    }

    // Hands in either `only` (the quest the commander just turned in) or every quest the bot
    // has finished that `giver` ends, for every grouped bot near the commander.
    void HandInForGroup(Player* player, Object* giver, Quest const* only, HandInReport report)
    {
        Group* group = player->GetGroup();
        if (!group || !giver)
            return;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* bot = ref->GetSource();
            if (!bot || bot == player || !bot->IsInWorld() || bot->IsBeingTeleported())
                continue;
            if (!sBotMgr->FindBotPlayer(bot->GetGUID().GetCounter()))
                continue;

            // Out of reach: only worth a word when it actually has something to hand in here.
            bool const sameMap = bot->GetMap() == player->GetMap();
            if (!sameMap || !bot->IsWithinDistInMap(player, COMPANION_TURNIN_RANGE) || !bot->IsAlive())
            {
                if (report != HandInReport::SuccessOnly && HasFinishedQuestFor(bot, giver, only))
                {
                    if (!bot->IsAlive())
                        Tell(player, Acore::StringFormat("{} is dead and cannot turn in here.", bot->GetName()));
                    else if (!sameMap)
                        Tell(player, Acore::StringFormat("{} is not on this map and cannot turn in here.", bot->GetName()));
                    else
                        Tell(player, Acore::StringFormat("{} is too far away ({:.0f} yd, max {:.0f}) to turn in here.", bot->GetName(),
                            bot->GetDistance(player), COMPANION_TURNIN_RANGE));
                }
                continue;
            }

            std::vector<Quest const*> quests;
            if (only)
                quests.push_back(only);
            else
            {
                QuestRelationBounds involved = InvolvedBounds(giver);
                for (auto itr = involved.first; itr != involved.second; ++itr)
                    if (Quest const* quest = sObjectMgr->GetQuestTemplate(itr->second))
                        if (bot->GetQuestStatus(quest->GetQuestId()) != QUEST_STATUS_NONE)
                            quests.push_back(quest);
            }

            for (Quest const* quest : quests)
            {
                uint32 reward = 0;
                HandInResult const result = TryHandIn(bot, quest, giver, reward);
                switch (result)
                {
                    case HandInResult::Done:
                        LOG_INFO("module.coa-playerbots.quests", "QuestSharedTurnIn: '{}' handed in quest {} '{}' with '{}' (reward choice {}).",
                            bot->GetName(), quest->GetQuestId(), quest->GetTitle(), player->GetName(), reward);
                        Tell(player, Acore::StringFormat("{} turned in \"{}\"{}.", bot->GetName(), quest->GetTitle(), RewardNote(quest, reward)));
                        break;
                    case HandInResult::NotComplete:
                        if (report == HandInReport::Everything)
                            Tell(player, Acore::StringFormat("{} has not finished \"{}\" yet.", bot->GetName(), quest->GetTitle()));
                        break;
                    case HandInResult::BagsFull:
                        if (report != HandInReport::SuccessOnly)
                            Tell(player, Acore::StringFormat("{} cannot turn in \"{}\": no bag space for the reward.", bot->GetName(), quest->GetTitle()));
                        break;
                    case HandInResult::Blocked:
                        if (report != HandInReport::SuccessOnly)
                            Tell(player, Acore::StringFormat("{} cannot turn in \"{}\" (not enough money or a weekly limit).", bot->GetName(), quest->GetTitle()));
                        break;
                    default:
                        break;
                }
            }
        }
    }

    void OnCompanionLeaderAccepted(Player* player, Quest const* quest, Object* giver)
    {
        if (!sBotMgr->QuestShareAccept() || !quest || !IsCompanionShareSource(player))
            return;

        Object* source = giver ? giver : FindNearbyQuestObject(player, quest->GetQuestId(), false);
        ForNearbyGroupedBots(player, [&](Player* bot)
        {
            if (bot->GetQuestStatus(quest->GetQuestId()) != QUEST_STATUS_NONE)
                return;
            if (!bot->CanTakeQuest(quest, false) || !bot->CanAddQuest(quest, false))
                return;
            bot->AddQuestAndCheckCompletion(quest, source);
            if (uint32 spell = quest->GetSrcSpell())
                bot->CastSpell(bot, spell, true);
            LOG_INFO("module.coa-playerbots.quests", "QuestSharedAccept: '{}' took quest {} '{}' because '{}' accepted it.",
                bot->GetName(), quest->GetQuestId(), quest->GetTitle(), player->GetName());
        });
    }

    void OnCompanionLeaderTurnedIn(Player* player, Quest const* quest, Object* giver)
    {
        if (!sBotMgr->QuestShareTurnIn() || !quest || !IsCompanionShareSource(player))
            return;

        // With the giver (the reward dialog) this is the main pass and explains failures; without
        // it (the end-of-RewardQuest hook) it only catches paths that skipped the dialog hook.
        Object* source = giver ? giver : FindNearbyQuestObject(player, quest->GetQuestId(), true);
        HandInForGroup(player, source, quest, giver ? HandInReport::Everything : HandInReport::SuccessOnly);
    }

    void OnCompanionLeaderTalkedTo(Player* player, Object* giver)
    {
        if (!sBotMgr->QuestShareTurnIn() || !giver || !IsCompanionShareSource(player))
            return;
        QuestRelationBounds involved = InvolvedBounds(giver);
        if (involved.first == involved.second)
            return;
        // Bots often finish objectives after the commander (their own quest drop, a late kill
        // credit), so every finished quest this ender takes is handed in, not just the commander's.
        HandInForGroup(player, giver, nullptr, HandInReport::Problems);
    }
}
