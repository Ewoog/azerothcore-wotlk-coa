/*
 * mod-coa-playerbots
 *
 * Bot companions for CoA's custom Ascension classes. A bot is a real Player
 * driven by a WorldSession with a nullptr socket instead of a real client
 * connection — see C:\games\source\server\mod-coa-playerbots\docs for why.
 *
 * This module grew out of pilot/ (proved the login mechanism works on CoA's
 * live core). BotMgr carries that proven logic forward; group/loot/guild
 * support and bot AI are added incrementally on top of it, milestone by
 * milestone — see AGENTS.md for what's done and what's next.
 */

#ifndef COA_PLAYERBOTS_BOT_MGR_H
#define COA_PLAYERBOTS_BOT_MGR_H

#include "ObjectGuid.h"
#include "SharedDefines.h"
#include "BotAI.h"
#include "BotFormations.h"
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class ChatHandler;
class Guild;
class Player;
class SpellInfo;
class Roll;
class WorldSession;

class BotMgr
{
public:
    static BotMgr* instance();

    // Logs an existing character in through a fake, null-socket WorldSession
    // instead of a real client connection. Async: the result (success or
    // failure) is only known once the login query completes and is logged,
    // not returned synchronously. onReady, if given, fires once the bot's real Player object
    // exists (right after a successful login) -- for a caller that needs to do one-time setup
    // on a freshly created character (see BotSpawnRandom.cpp's leveled-bot spawner) without
    // adding yet another pending-queue mechanism just for that.
    void SpawnBot(ObjectGuid::LowType charLowGuid, ChatHandler* handler, std::function<void(Player*)> onReady = nullptr);

    // The master switch (CoaBots.Enable, default on): with it off no bot is ever logged in - not by command, not at start,
    // not for a group, battleground or dungeon fill. Read again every few seconds, so a changed config takes effect with
    // the next spawn (bots that are already online stay until the world restarts).
    static bool BotsEnabled();

    // Manual/debug entry point: accepts a pending group invite on a bot's
    // behalf right now, reporting success/failure to handler. Bots normally
    // don't need this called explicitly any more (see Update() below) — kept
    // as an explicit override for testing/debugging.
    void AcceptInvite(ObjectGuid::LowType charLowGuid, ChatHandler* handler);

    // Manual/debug entry point: accepts a pending guild invite on a bot's behalf
    // right now, reporting success/failure to handler.
    void AcceptGuildInvite(ObjectGuid::LowType charLowGuid, ChatHandler* handler);

    // Manual/debug entry point: has a bot in a guild invite another online player
    // (bot or real client, matched by name) to its guild.
    void GuildInvite(ObjectGuid::LowType charLowGuid, std::string const& targetName, ChatHandler* handler);

    // Creates a new guild with this bot as Guild Master.
    void GuildCreate(ObjectGuid::LowType charLowGuid, std::string const& guildName, ChatHandler* handler);

    // Deposits items of itemEntry from bot's inventory into guild bank.
    // count == 0 means deposit all matching items. Returns deposited count.
    uint32 GuildDepositItem(ObjectGuid::LowType charLowGuid, uint32 itemEntry, uint32 count, ChatHandler* handler);

    // Banks every Trade Goods item (ore, herbs, cloth, leather, meat/fish -- anything
    // GatherableCatalog would offer) currently sitting in a guilded bot's bags. No-op if the bot
    // isn't in a guild. Raw materials are worth more in the guild bank than sold to a vendor for a
    // few coppers or left clogging bag space -- called before handing out a new guild task (see
    // GuildGather) so a bot starts each task with its bags already as empty as they can be.
    void DepositLooseResourcesToGuildBank(Player* bot);

    // Every trade-goods item in the guild's bank with its total count over all tabs (0 entries for an unknown guild).
    std::unordered_map<uint32, uint32> GuildBankTradeGoods(uint32 guildId) const;

    // Withdraws items of itemEntry from guild bank into bot's inventory. Returns withdrawn count.
    uint32 GuildWithdrawItem(ObjectGuid::LowType charLowGuid, uint32 itemEntry, uint32 count, ChatHandler* handler);

    // Deposits money (in copper) from bot into guild bank.
    void GuildDepositMoney(ObjectGuid::LowType charLowGuid, uint32 copper, ChatHandler* handler);

    // Withdraws money (in copper) from guild bank to bot.
    void GuildWithdrawMoney(ObjectGuid::LowType charLowGuid, uint32 copper, ChatHandler* handler);

    // Orders bot to gather itemEntry up to targetCount and deposit into guild bank.
    // requesterLowGuid: the player who asked (gets ORDERDONE/ORDERFAIL notices and may cancel
    // the order without being a guild officer). 0 == take it from handler's session, if any.
    void GuildGather(ObjectGuid::LowType charLowGuid, uint32 itemEntry, uint32 targetCount, ChatHandler* handler,
        ObjectGuid::LowType requesterLowGuid = 0);

    // Finds an online guild-mate bot of `requesterCharLowGuid` that knows a recipe spell
    // producing itemEntry (any learned spell with a SPELL_EFFECT_CREATE_ITEM effect targeting
    // it -- see CraftingRecipeIndex in BotMgr.cpp), and orders it to craft `count` of them. If
    // the crafter already holds enough reagents, crafts immediately; otherwise queues a
    // background order (_craftOrders) that retries every tick once reagents show up (e.g. via
    // the crafter's own autonomous gathering AI) -- same "wait for it" shape as
    // _guildGatherOrders. Finished items are mailed to the requester via the real MailDraft
    // path (works whether they're online or not), never deposited to the guild bank -- see
    // docs/addon-protocol.md's CRAFTORDER verb for why that's a deliberate v1 simplification.
    void CraftOrder(ObjectGuid::LowType requesterCharLowGuid, uint32 itemEntry, uint32 count, ChatHandler* handler);

    // Addon-facing counterpart to GuildGather: picks a suitable online guild-mate bot itself
    // (same "auto-pick, don't make the player know a bot's raw guid" idea as CraftOrder) rather
    // than requiring a specific charLowGuid, then places the same background order via the
    // existing GuildGather. Prefers a bot not already busy with a gather/craft order of its own;
    // falls back to the first online guild-mate bot if all are busy (same overwrite tolerance
    // CraftOrder already has for _craftOrders). See docs/addon-protocol.md's GATHERORDER verb.
    void GatherOrder(ObjectGuid::LowType requesterCharLowGuid, uint32 itemEntry, uint32 count, ChatHandler* handler);

    struct GuildGatherOrder
    {
        uint32 itemEntry = 0;
        uint32 targetCount = 0;
        uint32 gatheredCount = 0;
        uint32 remainingCount = 0;
        uint32 targetMapId = 0;
        float targetX = 0.0f;
        float targetY = 0.0f;
        float targetZ = 0.0f;
        bool hasTargetLocation = false;
        // Player who placed the order (0 for a GM .botcmd order with no player session, or a row
        // saved before the requester_guid column existed). Notified on completion/failure and
        // allowed to cancel it even without officer rank.
        ObjectGuid::LowType requesterLowGuid = 0;
        // RetryGuildGatherLocation calls since the last deposit; GUILD_GATHER_MAX_STALLED_RETRIES
        // of them in a row gives the order up (ORDERFAIL). Not persisted -- a restart starts over.
        uint32 stalledRetries = 0;
        // getMSTime() of the last deposit (or of the bot coming online with the order); drives
        // GUILD_GATHER_NO_PROGRESS_MS. 0 == not started yet. Not persisted.
        uint32 lastProgressMs = 0;

        // --- Not persisted: live state for progress, status text and spot rotation ---------------
        // How many of the ordered item the bot is carrying right now (capped at remainingCount).
        // Counts toward the progress the Task Board shows, so a bot that is gathering but cannot
        // bank (guild bank full) is visibly making progress instead of sitting at 0.
        uint32 heldCount = 0;
        // The last guild-bank deposit attempt found no room (or no rights) for this item. The
        // finished order is then mailed to the requester instead (see MailGatherItems).
        bool bankBlocked = false;
        uint32 lastDepositTryMs = 0;
        // getMSTime() the bot was first seen in a group led by a real player (0 == not grouped).
        // Grouped bots follow their leader, so the order pauses and, after a while, is handed on.
        uint32 groupedSinceMs = 0;
        // Short, human-readable "what is it doing right now" for the Task Board / .botcmd orderstatus.
        std::string status;
        // Spots this order already tried and found empty; the next pick avoids them.
        struct TriedSpot { uint32 mapId; float x; float y; };
        std::vector<TriedSpot> triedSpots;
        // Short number for chat lines and `.botcmd orderstatus` ("order #3"). Handed out when the
        // order is placed or loaded; not persisted, so it can change after a restart.
        uint32 orderNo = 0;
        // The requester was told about the first item counted / a node that gave something else /
        // full bags. One line each per order, so chat is not flooded.
        bool firstProgressTold = false;
        uint32 startProgress = 0; // GatherOrderProgress when placed/loaded: "first progress" is above it
        bool wrongLootTold = false;
        bool bagsFullTold = false;
        // The bot's bags cannot take one more of the ordered item even after making room. Loot
        // that does not fit stays in the node, so nothing is counted until this clears.
        bool bagsFull = false;
        uint32 lastRoomCheckMs = 0;
        // What the bot looted while on this order (nodes, corpses): for orderstatus.
        uint32 lootsOpened = 0;
        uint32 lootsWithItem = 0;
        uint32 lootsLeftBehind = 0;
        std::string lastLoot;
    };
    GuildGatherOrder const* GetGuildGatherOrder(ObjectGuid const& guid) const;
    // Banked + carried, capped at targetCount: what the Task Board shows as progress.
    static uint32 GatherOrderProgress(GuildGatherOrder const& order);
    // BotAI's running commentary for an order ("walking to the spot (240 yd)", "searching for
    // nodes", ...). No-op when the bot has no order or the text is unchanged.
    void SetGatherOrderStatus(ObjectGuid const& guid, std::string const& status);
    // `.botcmd orderstatus`: one line per active/queued order with everything needed to see why
    // it is (not) moving -- position, distance to its spot, skill, bags, bank, retries, idle time.
    void DescribeGatherOrders(ChatHandler* handler) const;
    // BotAI::TakeAllLoot reports every loot window it emptied for a bot with an order: what was in
    // it (entry, count), how many of the ordered item reached the bags, and whether anything had to
    // be left behind (bags full). Feeds orderstatus and the one-time "got something else" line.
    void NoteGatherLoot(Player* bot, std::vector<std::pair<uint32, uint32>> const& looted, uint32 orderItemsStored,
        bool leftBehind);
    // True when bot has an active gather order; *itemEntry gets the ordered item.
    bool GetGatherOrderItem(ObjectGuid const& guid, uint32* itemEntry) const;
    // Makes room in an order bot's bags for one more of the ordered item: clutter sweep, then
    // other Trade Goods to the guild bank, then (bank full) mails them to the requester. Returns
    // true when the item now fits.
    bool MakeRoomForGatherOrder(Player* bot);

    // Where an ordered item can come from, computed once per item from the world DB loot tables
    // and the in-memory spawn/template stores (see BuildGatherSourceInfo in BotMgr.cpp).
    // huntCorpseEntries: creature entries whose corpse loot (creature_loot_template, directly or
    // through one level of reference_loot_template) can yield the item; huntSkinEntries: entries
    // whose skinning loot (skinning_loot_template via creature_template.skinloot, Skinning skill
    // only) can. Only normal-rank creatures with at least one spawn on an open-world continent
    // (maps 0, 1, 530, 571) are listed.
    struct HuntSpawn
    {
        uint16 mapId = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        uint8 minLevel = 1, maxLevel = 1;
    };
    struct GatherSourceInfo
    {
        bool nodeSourced = false;      // loot of a Herbalism/Mining-locked gameobject (chests don't count)
        uint32 nodeSkillId = 0;        // SKILL_HERBALISM / SKILL_MINING
        uint32 nodeRequiredSkill = 0;  // lowest skill value any of those nodes needs
        bool fishSourced = false;      // in fishing_loot_template
        std::unordered_set<uint32> huntCorpseEntries;
        std::unordered_set<uint32> huntSkinEntries;
        std::vector<HuntSpawn> huntSpawns;
        uint8 huntMinLevel = 0;        // lowest minlevel among hunt sources with a spawn (0 = none)
        // gameobject_template entries of the real Herbalism/Mining nodes that can yield the item
        // (chests excluded), and the best loot chance among them (0 = only in equal-chance groups).
        std::vector<uint32> nodeGoEntries;
        float nodeBestChance = 0.0f;
        // Targeted hunting applies: not a node or fish item, and at least one spawned creature drops it.
        bool IsHunt() const { return !nodeSourced && !fishSourced && !huntSpawns.empty(); }
        bool SkinOnly() const { return huntCorpseEntries.empty() && !huntSkinEntries.empty(); }
    };
    GatherSourceInfo const& GetGatherSourceInfo(uint32 itemEntry);

    // Task Board management (docs/addon-protocol.md CANCELORDER / PRIORITIZEORDER / CANCELQUEUED /
    // TOPQUEUED). Each returns a short result token for the addon reply ("ok", "denied", "none").
    // Allowed when `commander` is in the bot's (or queued order's) guild and is either a guild
    // master/officer (rank 0/1) or the player who placed the order.
    // Cancels the bot's gather order, or its crafting order when it has no gather order.
    std::string CancelGatherOrder(Player* commander, ObjectGuid::LowType botLowGuid);
    std::string CancelCraftOrder(Player* commander, ObjectGuid::LowType botLowGuid);
    std::string PrioritizeGatherOrder(Player* commander, ObjectGuid::LowType botLowGuid);
    std::string CancelQueuedGatherOrder(Player* commander, uint32 queueId);
    std::string TopQueuedGatherOrder(Player* commander, uint32 queueId);

    // Restarts the 30-minute no-progress clock so a repair or ammo trip does not fail the order.
    void PauseGuildGatherWatchdog(ObjectGuid const& guid);

    // Gather orders placed while every eligible guild bot is busy wait here instead of
    // overwriting a running order; the front of the vector is assigned first (TOPQUEUED moves an
    // entry there). Persisted in mod_coa_bot_guild_gather_queue.
    struct QueuedGatherOrder
    {
        uint32 queueId = 0;
        uint32 guildId = 0;
        ObjectGuid::LowType requesterLowGuid = 0;
        uint32 itemEntry = 0;
        uint32 count = 0;
    };

    struct CraftOrderState
    {
        ObjectGuid requesterGuid;
        uint32 spellId = 0;
        uint32 itemEntry = 0;
        uint32 remainingCount = 0;
        bool awaitingCastResult = false;
        uint32 itemCountBeforeCast = 0;
        // Reagent sourcing (BotEconomy::SourceReagents): when the order was placed, when the crafter next looks
        // for missing reagents, what it may still spend on them, and whether the requester was told what is missing.
        uint32 placedAtMs = 0;
        uint32 nextSourcingAtMs = 0;
        uint64 reagentBudgetLeftCopper = 0;
        bool toldMissing = false;
        // Failed casts in a row (CoaBots.Crafting.MaxCastFailures ends the order) and when the next try may start.
        uint32 failedCasts = 0;
        uint32 nextCastAtMs = 0;
        uint32 orderedCount = 0;
    };
    CraftOrderState const* GetCraftOrder(ObjectGuid const& guid) const;

    // Re-resolves and re-teleports an in-progress guild gather order to a fresh location for the
    // same item, called from BotAI.cpp's UpdateSoloWorld when a targeted node search has come up
    // empty nearby for too long (GUILD_GATHER_RETELEPORT_MS) -- the original spot's node was
    // already consumed (respawn pending) or claimed by someone else first. No-op if the bot has no
    // such order, no active session, or the item has no resolvable gather location at all.
    // Every GUILD_GATHER_MAX_STALLED_RETRIES-th call without a deposit in between, or a call
    // that finds no source at all, gives the order up instead (FailGuildGatherOrder).
    // `rush` (PRIORITIZEORDER) teleports straight to the new spot even on the same map and does
    // not count as a stalled retry.
    void RetryGuildGatherLocation(ObjectGuid const& guid, bool rush = false);
    void LoadGuildGatherOrders();
    // Erases the order (memory + DB) and tells its requester why (ORDERFAIL:...:<reason>).
    void FailGuildGatherOrder(ObjectGuid const& botGuid, std::string const& reason);
    void SaveGuildGatherOrder(ObjectGuid const& guid, GuildGatherOrder const& order);
    void DeleteGuildGatherOrder(ObjectGuid const& guid);

    // One pre-formatted "ROSTER:botGuidLow:name:class:level:task:prof1=skill1,prof2=skill2"
    // body per online bot in `commander`'s guild -- the addon task-board query (see
    // docs/addon-protocol.md's GUILDROSTER verb). One string per bot rather than one combined
    // message, same reasoning as everything else on this wire channel: WoW chat messages have
    // a real length cap, and a big guild's full roster could exceed it in a single body.
    std::vector<std::string> GetGuildRosterInfo(Player* commander) const;

    // Addon catalog queries (see docs/addon-protocol.md's GETGATHERCATALOG/GETRECIPECATALOG
    // verbs) -- both return pre-chunked "GCAT:category:entry,name|entry,name|..." /
    // "RCAT:profession:entry,name|..." reply bodies (several per call, chat-length-safe, same chunking
    // reasoning as GetGuildRosterInfo) so the addon can build icon-menu pickers instead of
    // making the player type a raw item id. GetGatherCatalog is guild-agnostic (any online bot
    // can gather any of these once granted every profession, see GrantAllProfessions) and its
    // underlying data is cached process-wide after the first call. GetRecipeCatalog is scoped
    // to `commander`'s guild -- only items an online guild-mate bot can *actually* craft right
    // now (Player::HasSpell on a real SPELL_EFFECT_CREATE_ITEM spell, same definition of
    // "knows a recipe" CraftOrder itself uses) are offered, per the user's explicit ask to only
    // show recipes bots really have.
    std::vector<std::string> GetGatherCatalog() const;
    std::vector<std::string> GetRecipeCatalog(Player* commander) const;

    // Diagnostic-only, not a wire-protocol verb: reports, per profession, how many distinct
    // craftable items any CURRENTLY ONLINE bot in the whole population actually knows a recipe
    // for. Added because this realm's `spell_dbc`/`skilllineability_dbc` SQL export tables turned
    // out to be a small, stale subset (4486 rows, IDs up to ~80000) that doesn't cover this
    // fork's real custom spell content (confirmed live: a working craft order used spell 807053,
    // absent from that table entirely) -- so a SQL-only check of "does any bot know a real
    // Blacksmithing/Leatherworking/Tailoring recipe" is unreliable. This asks the same live,
    // authoritative in-memory `sSpellMgr`/`Player::HasSpell` data `CraftOrder` itself already
    // trusts, instead.
    void DumpRecipeCoverage(ChatHandler* handler) const;

    // Manual/debug entry point: has a bot invite another online player (bot or real client,
    // matched by name) to its group, by calling the real WorldSession::HandleGroupInviteOpcode
    // handler directly with a minimal packet -- same "call the real thing" pattern as
    // DoAcceptInvite. There's no plain ".invite" GM command in this codebase (only
    // ".guild invite" exists) and a real client only ever sends this via its UI, so this is
    // the only way to form a test group between two bots without a real client's cooperation.
    void Invite(ObjectGuid::LowType charLowGuid, std::string const& targetName, ChatHandler* handler);

    // Cleanly logs a bot out: LogoutPlayer(true) (saves + removes the Player
    // from world, same path a real disconnect takes) then frees the
    // WorldSession. Without this there was previously no way to remove a bot
    // short of restarting the whole server.
    void DespawnBot(ObjectGuid::LowType charLowGuid, ChatHandler* handler);

    // Server shutdown: save every companion and take it out of the world while the database is still accepting
    // work. Companions live in this manager, not in the world's session list, so the normal "save and kick all
    // players" pass never reaches them. Returns how many were saved.
    uint32 LogoutAllBots();

    // Despawns every online bot and permanently deletes every bot character row (any account
    // matching CoaBots.RandomSpawn.AccountPrefix, online or not) via the real
    // Player::DeleteFromDB, so nothing gets left behind for a name/guid collision on the next
    // spawn batch. For clearing out an old naked/broken population before respawning fresh
    // ones with .botcmd spawnrandom or spawnleveled -- does not create any replacement bots
    // itself.
    void PurgeAllBots(ChatHandler* handler);
    void PurgeCustomRaceBots(ChatHandler* handler);

    // Logs every online bot out and forgets the queued automatic logins, WITHOUT deleting anything: the bot characters
    // stay in the database and come back with the next auto-login or spawnbot. Returns how many were logged out.
    uint32 DespawnAllBots();

    // Diagnostic-only, not bot-specific: dumps every current aura (spell id,
    // name, whether it carries SPELL_AURA_PREVENT_REGENERATE_POWER) on any
    // online player found by low guid -- bot or real client. Added to chase
    // the out-of-combat regen bug; not part of the bot-companion feature set.
    void ListAuras(ObjectGuid::LowType charLowGuid, ChatHandler* handler);

    // Diagnostic-only, not bot-specific: runs an arbitrary chat command
    // (e.g. ".localspec 52") as the given online player (bot or real
    // client), by feeding it through that player's own WorldSession's
    // ChatHandler -- same trick as the direct-handler calls elsewhere in
    // this file, but for chat commands specifically. Needed because some
    // gameplay systems (e.g. Ascension's custom talent trees) are
    // implemented as SEC_PLAYER chat commands a client addon sends on the
    // player's behalf, not as normal opcodes.
    void RunChatCommand(ObjectGuid::LowType charLowGuid, std::string const& command, ChatHandler* handler);

    // Diagnostic-only, not bot-specific: reports Player::HasSpell() for a
    // fixed set of spell ids on any online player by low guid. Added to
    // settle exactly which prerequisite spell a stuck Ascension talent
    // entry is missing, instead of inferring it from aura presence (auras
    // don't exist for every learned spell) or from chat messages (easy to
    // miss in a busy log).
    void HasSpells(ObjectGuid::LowType charLowGuid, std::vector<uint32> const& spellIds, ChatHandler* handler);

    // Debug/testing entry point: finds the nearest non-friendly unit within range and
    // has the bot attack it via the same Unit::Attack() path a real client's attack
    // action would use. Needed because there's no way to click "attack" for a bot from
    // the RA console -- BotAI.cpp then takes over once combat starts (chase/cast/etc.).
    void AttackNearestHostile(ObjectGuid::LowType charLowGuid, float range, ChatHandler* handler);

    // Debug/testing entry point: kills a bot outright via the real Unit::Kill() death-processing
    // path (same one any normal combat death goes through -- loot generation, death state
    // transition, everything), for exercising BotAI's death-handling flow (grace period,
    // release spirit, corpse-run, self-res) on demand. Added because there's no reliable way
    // to manufacture a controlled bot death from the RA console otherwise: .die needs a real
    // client selection, and reducing HP via .modify was inconsistent (sometimes the bot's own
    // damage output or regen wins the race before a mob's next hit lands).
    void Kill(ObjectGuid::LowType charLowGuid, ChatHandler* handler);

    // Assigns a bot's AI role ("dps", "tank", or "healer" -- case-insensitive) for
    // BotAI::Update() to dispatch on. See BotRole in BotAI.h for what changes per role, and
    // docs/roles.md for why this is manual rather than read from the bot's actual Ascension
    // spec right now.
    void SetRole(ObjectGuid::LowType charLowGuid, std::string const& roleName, ChatHandler* handler);

    // Diagnostic: reports what BotAI's own offensive/taunt/heal spellbook filters find for
    // this bot right now. See BotAI::ReportSpellbookRoleSignals for why this exists (mapping
    // a class's numeric Ascension SpecId to a real role empirically).
    void CheckRole(ObjectGuid::LowType charLowGuid, ChatHandler* handler);

    // Gives a bot a real Ascension specialization: switches spec and spends every
    // non-automatic ("paid") talent entry in that spec plus the shared class tree at its
    // highest affordable rank -- the ones AECost/TECost-gated behind `.localtalent`, a
    // SEC_PLAYER chat command that (like all of them) doesn't work on a null-socket bot
    // session (see RunChatCommand's doc comment). This is a companion, not a stripped-down
    // pet substitute (docs/architecture.md's scope decision) -- a real player's group
    // companion has their talents spent, so a bot should too.
    //
    // Both the spec switch and each entry's rank are set through
    // AscensionClassServiceBridge::SwitchSpecialization()/SetTalentRank() (see
    // docs/core-patches.md's "Patch 3") -- the same budget-checked, mutual-exclusion-aware
    // path a real player's `.localspec`/`.localtalent` commands use. Earlier versions of
    // this function wrote the "core.ascension_active_spec" PlayerSetting directly and
    // learnSpell()'d every paid entry unconditionally: that bypassed
    // AscensionClassService's per-level class/spec point budget entirely (so a bot could
    // end up over-invested) and never touched the in-memory active-spec map
    // GetActiveSpecialization() actually reads, which silently blocked every spec-gated
    // automatic talent grant regardless of what the PlayerSetting said. A SetTalentRank
    // failure (most commonly: the budget is already spent at this level) is logged and
    // skipped rather than forced through. Automatic (free) entries are still left to
    // mod-ascension-compat's own SynchronizeProgression, now correctly triggered by the
    // real spec switch. specId 0 grants only the shared tree (no spec chosen). See
    // docs/roles.md for known class -> SpecId -> role mappings.
    void LearnSpecialization(ObjectGuid::LowType charLowGuid, uint32 specId, ChatHandler* handler);

    // One-button dungeon group fill: brings `commander`'s group up to 5 (tank + healer + 3
    // dps), inviting online bots to cover whichever of those roles it's currently missing.
    // Guildmates of `commander` are preferred over other bots; among equally-eligible
    // candidates, closest character level then closest average item level wins. Invites are
    // issued through `commander`'s own real WorldSession::HandleGroupInviteOpcode (same packet
    // shape as BotMgr::Invite, just fired from the real player's session instead of a bot's) --
    // no new accept-side code needed, since a bot's pending GetGroupInvite() is already
    // auto-accepted every tick by the existing BotMgr::Update() loop (DoAcceptInvite), which
    // also handles the teleport-to-leader. `commander` must be a real (non-bot) player; see
    // docs/addon-protocol.md's QUICKFILL verb.
    void QuickFillGroup(Player* commander, ChatHandler* handler);

    // Invites ONE bot of the given role to the commander's group - the "pocket healer" case Quick Fill is too much for.
    // Chosen like Quick Fill chooses: an online, ungrouped, living bot of the commander's faction, guildmates first, then
    // the closest level and gear. A bot that already plays the role wins; otherwise a bot whose class can play it is
    // switched to it. Does nothing when the group is full. Reports through `handler` when given.
    void InviteBotForRole(Player* commander, BotRole role, ChatHandler* handler);

    // The browsable list of bots behind the addon's "Browse" tab, free of the /who list's length limit. The filters are all
    // optional: `role` (-1 = any, else a BotRole), a class id (0 = any), a level range, guildmates only, and a part of the name.
    struct BotListQuery
    {
        uint32 page = 0;
        int32 role = -1;
        uint32 classId = 0;
        uint32 minLevel = 0;
        uint32 maxLevel = 255;
        bool guildOnly = false;
        std::string name;
    };
    // Reply bodies: `BOTPAGE:<page>:<total matches>:<pages>` then `BOTS:<guid>,<name>,<class>,<level>,<role>,<guildmate 0/1>|...`.
    // Only online bots the commander could invite are listed: of their faction, not in a group, not the commander.
    std::vector<std::string> GetBotList(Player* commander, BotListQuery const& query) const;
    // Invite one listed bot to the commander's group / to the commander's guild (the guild's own invite rights apply).
    void InviteBotToGroup(Player* commander, ObjectGuid::LowType botGuid, ChatHandler* handler);
    void InviteBotToGuild(Player* commander, ObjectGuid::LowType botGuid, ChatHandler* handler);

    // If `bot` is a tracked bot already in another group, leave that group so a new invite can
    // succeed. Returns false (and leaves the group alone) when the bot is in combat, in a
    // battleground, or on a dungeon/raid map. Returns true when the bot has no group, or left it.
    bool TryReleaseBotFromGroup(Player* inviter, Player* bot);

    // Toggles autonomous dungeon/raid play for `leader`'s whole group: while enabled, a Tank
    // bot idle inside an instance (no target, leader not already fighting) scans for the
    // nearest hostile pack and pulls it on its own -- see BotAI.cpp's TryAutoPullInInstance
    // for why this deliberately does not attempt to encode per-dungeon boss order or mechanics
    // (relies on the instance's own real gating -- locked doors/gameobjects that only open
    // after a prerequisite boss dies -- the same way a human group is naturally kept in the
    // intended order without needing to be told it explicitly). Keyed by leader guid so it
    // covers the group as a whole regardless of which bot is currently idle; persists across
    // a wipe/regroup until explicitly turned off.
    void SetAutoDungeonMode(ObjectGuid leaderGuid, bool enabled);
    bool IsAutoDungeonModeEnabled(ObjectGuid leaderGuid) const;
    void MarkBossCleared(ObjectGuid leaderGuid, uint32 bossEntry);

    // A creature a bot (or a bot's group) killed. PostKillEvent may be called from any map thread
    // (the reward-kill hook runs inside Map::Update, and with MapUpdate.Threads > 1 several maps do
    // that at once); everything it records is applied on the world thread by Update(), where all the
    // other bot state lives, so none of it needs its own locking.
    struct KillEvent
    {
        ObjectGuid killer;
        ObjectGuid victim;
        ObjectGuid bossLeader; // set (with bossEntry) when the victim was a dungeon boss
        uint32 bossEntry = 0;
    };
    void PostKillEvent(KillEvent const& event);
    bool IsBossCleared(ObjectGuid leaderGuid, uint32 bossEntry) const;
    void ClearBosses(ObjectGuid leaderGuid);

    void SetGroupFormation(ObjectGuid leaderGuid, BotGroupFormation formation);
    BotGroupFormation GetGroupFormation(ObjectGuid leaderGuid) const;

    // Out-of-combat "bring bots to me": teleports every bot in `commander`'s group to
    // `commander`'s exact location, same two-phase TeleportTo()-then-queue-ack sequencing
    // DoAcceptInvite/TryFollowLeaderAcrossMaps already use (see FinishPendingTeleport's
    // comment for why the ack can't fire same-tick) -- FinishPendingTeleport's existing
    // post-landing re-follow picks formation/follow back up with no extra code here. Refuses
    // (no-op) if `commander` is in combat; skips (rather than aborts) any individual bot that
    // is itself in combat, so pulling the group together never yanks one bot out of a fight it's
    // already in. See docs/addon-protocol.md's TELEPORT verb.
    void TeleportBotsToPlayer(Player* commander, ChatHandler* handler);

    // Per-bot gear preference (which basic armor/weapon subclass to favor when rolling on and
    // auto-equipping loot), persisted via PlayerSetting the same way "core.ascension_active_spec"
    // is (survives a relog, in-memory only until then). Armor preference is stored as a raw
    // ItemSubclassArmor value with 0 (ITEM_SUBCLASS_ARMOR_MISC, never itself a preference choice)
    // doubling as the "auto/any" sentinel. Weapon preference is stored as (subclass + 1) since
    // ITEM_SUBCLASS_WEAPON_AXE is itself 0 and would otherwise collide with an "unset" sentinel;
    // 0 means "auto/any". See docs/addon-protocol.md's GETGEAR/SETGEARPREF verbs.
    uint32 GetGearPreference(Player* bot, bool weapon) const;
    void SetGearPreference(Player* bot, bool weapon, uint32 subclass, bool automatic = false);

    // Whether `itemEntry` (an armor or weapon item) is one this bot should Greed-roll on / treat
    // as a valid upgrade candidate, given its real equip proficiency (CanEquipNewItem) and its
    // gear preference above. Non-armor/non-weapon items, and armor/weapon subclasses outside the
    // 4 basic armor types or 6 basic one-handed/staff weapon types this feature covers, always
    // return true (unaffected -- see docs/addon-protocol.md's TELEPORT verb's neighbor entries
    // for why this deliberately doesn't try to cover shields/rings/trinkets/ranged weapons/etc).
    bool MatchesGearPreference(Player* bot, uint32 itemEntry) const;

    // Which of the 4 basic armor subclasses (cloth/leather/mail/plate) and which of the 6 basic
    // weapon subclasses (axe/mace/sword/staff/fist/dagger) this bot's class can actually equip at
    // all, probed live via the real CanEquipNewItem proficiency check against one representative
    // real item per subclass (same item ids/technique GearUpBot already uses) -- no
    // classId-to-proficiency table exists for Ascension's custom classes, so this has to ask the
    // engine rather than look anything up. Returns raw ItemSubclassArmor/ItemSubclassWeapon
    // values. See docs/addon-protocol.md's GETGEAR verb.
    std::vector<uint32> GetLegalArmorSubclasses(Player* bot) const;
    std::vector<uint32> GetLegalWeaponSubclasses(Player* bot) const;

    // One "GEAR:botGuidLow:slot:itemEntry:itemName:itemLevel" line per currently-equipped item (skips empty
    // slots and the cosmetic-only shirt/tabard slots, which have no "type" concept relevant to
    // gear preference) -- the addon-facing gear inspector. See docs/addon-protocol.md's GETGEAR
    // verb.
    std::vector<std::string> GetEquippedGearInfo(Player* bot) const;

    // Called from a new PLAYERHOOK_ON_LOGIN hook whenever a REAL (non-bot) player logs in.
    // Group membership itself already survives a restart natively -- Player::_LoadGroup()
    // reattaches `player` to its pre-existing Group (loaded at world boot by
    // GroupMgr::LoadGroups()) purely from the persisted group_member table, no action needed
    // here for that part. What doesn't happen automatically is the bots' own login: an offline
    // groupmate just stays offline forever unless something calls SpawnBot for it. This walks
    // `player`'s (already-reattached) group's full member list -- including offline members,
    // via Group::GetMemberSlots(), not just the online GroupReference list -- and SpawnBots
    // any member that (a) isn't already online and (b) actually belongs to a bot-hosting
    // account (checked via IsBotAccountId, matched against the same
    // CoaBots.RandomSpawn.AccountPrefix config key BotSpawnRandom.cpp uses to create these
    // accounts) -- that account check is the safety rail that stops this from ever
    // auto-logging-in some other real player's alt just because it was left grouped.
    void RestoreGroupBotsOnLogin(Player* player);

    // Repairs unsuitable equipment using the same policy as newly spawned bots.
    void GearUpBot(Player* bot, ChatHandler* handler);

    // Public counterpart to the private FindBotSession, for callers (BotAddonChat.cpp) that
    // need to confirm a guid is really one of our tracked bots and get its Player* -- e.g. to
    // reject an addon-message command referencing a guid that isn't actually a bot session.
    // Returns nullptr for anything not currently in _botSessions with a live Player.
    Player* FindBotPlayer(ObjectGuid::LowType charLowGuid) const;

    // All currently-online bots with a live Player (skips a session mid-login with no Player
    // yet). For callers that need to pick one out of the whole roster rather than look up one
    // specific guid -- e.g. BotBattlegroundFill.cpp finding an idle bot of a given faction to
    // queue, without needing BotMgr to track a separate faction-indexed roster of its own.
    std::vector<Player*> GetOnlineBots() const;

    // Finds an existing offline bot character created under CoaBots.RandomSpawn.AccountPrefix,
    // matching team and level constraints. Returns 0 if none found.
    ObjectGuid::LowType FindEligibleOfflineBot(TeamId team, uint8 minLevel = 0, uint8 maxLevel = 80,
        std::unordered_set<ObjectGuid::LowType> const& excludedGuids = {}) const;

    // Unified protection predicate (PB-002): true if the bot is in a guild, has a guild invite,
    // is in an online or persisted group/raid, is on a real player's friends list, or is executing
    // an active player/guild order.
    bool IsProtectedBot(ObjectGuid::LowType charLowGuid) const;
    bool IsProtectedBot(Player const* bot) const;

    // Called every world tick via a WorldScript hook. Auto-accepts any
    // pending group invite and auto-rolls Greed on any pending loot roll for
    // every active bot (both checked every tick, not throttled — a human
    // player expects a near-instant response to either), and logs a
    // periodic (throttled) heartbeat so a human watching the log can confirm
    // bots are still alive without polling in-game.
    void Update(uint32 diff);

    // Public hook for any OTHER real-engine call that can itself trigger a TeleportTo() this
    // module didn't initiate directly -- Player::RepopAtGraveyard() (called from the real
    // WorldSession::HandleRepopRequestOpcode, itself called from BotAI::UpdateDeathHandling's
    // "call the real thing" release-spirit flow) is the confirmed case: releasing at a
    // graveyard teleports the ghost there internally, same as the module's own explicit
    // teleports elsewhere (DoAcceptInvite, TryFollowLeaderAcrossMaps,
    // TryReturnGhostToCorpseMap), but nothing was queuing *that* one for its ack. Left
    // unacked, a bot gets permanently stuck with IsBeingTeleportedNear()/Far() == true forever
    // (nothing else ever clears it for a null-socket session) -- confirmed live: bots that
    // died stopped cross-map-following their leader afterward, while one that never died kept
    // working. Callers outside BotMgr.cpp (BotAI.cpp's death handling) use this instead of
    // reaching into _pendingTeleportAck directly.
    void QueueTeleportAck(WorldSession* session);

    // Checks whether the given account ID belongs to a bot-hosting account.
    static bool IsBotAccountId(uint32 accountId);

    // Called from a new GroupScript::OnRemoveMember hook whenever a real (non-bot) player
    // leaves/is removed from a group that leaves no real player behind -- see that hook's own
    // comment for why bots shouldn't just sit there leaderless. Queued for the next Update()
    // tick rather than acted on immediately: OnRemoveMember fires from inside
    // Group::RemoveMember itself, and having a bot call RemoveMember again on the same Group
    // while it's still unwinding the first removal is exactly the kind of reentrancy the
    // existing _pendingTeleportAck/QueueTeleportAck pattern was already built to avoid elsewhere
    // in this file.
    void QueueBotGroupLeave(ObjectGuid botGuid);

private:
    BotMgr() = default;

    WorldSession* FindBotSession(ObjectGuid::LowType charLowGuid) const;

    // Calls the real WorldSession::HandleGroupAcceptOpcode handler directly
    // (a minimal padding WorldPacket stands in for the network payload it
    // reads and discards) — reuses all of the real validation logic instead
    // of duplicating it. Caller must have already confirmed there's a
    // pending invite (session->GetPlayer()->GetGroupInvite()). Also
    // teleports the bot to the group leader's exact spot on success — the
    // teleport-ack (see FinishPendingTeleport below) and MoveFollow happen
    // later, once the teleport has actually landed.
    void DoAcceptInvite(WorldSession* session);

    // Calls WorldSession::HandleGuildAcceptOpcode with a synthetic CMSG_GUILD_ACCEPT
    // packet, mirroring DoAcceptInvite for groups. Caller should ensure bot has a pending
    // guild invite (bot->GetGuildIdInvited() != 0).
    void DoAcceptGuildInvite(WorldSession* session);

    // A Player-type TeleportTo() (near or far) only *requests* the move --
    // the real position/grid update happens inside the ack handler
    // (HandleMoveTeleportAck / HandleMoveWorldportAck), which a real client
    // sends after its own network round trip. Firing that ack synchronously,
    // in the same tick as TeleportTo() itself, crashes with an IsInGrid()
    // assert in Map::PlayerRelocation -- confirmed live via a crash dump.
    // So: queue it here, and fire it from the *next* Update() tick instead,
    // giving the map's own per-tick processing a chance to settle first,
    // same as the real network delay would.
    void FinishPendingTeleport(WorldSession* session);

    // Checked every tick for every grouped bot not already mid-teleport: if the bot's map
    // and instance id no longer match its group leader's (leader zoned into a dungeon, raid,
    // or battleground -- or back out), queues a teleport to the leader's exact location, using
    // the same TeleportTo()-then-queue-ack-for-next-tick sequencing DoAcceptInvite already
    // established (see FinishPendingTeleport's comment for why the ack can't fire same-tick).
    // A real client walks through a portal themselves; a bot has nothing to walk through, so
    // this is the bot-side equivalent of that action, done automatically instead of requiring
    // a manual command -- see docs/addon-protocol.md and AGENTS.md for the user request this
    // implements. Deliberately unconditional on map *type* (open world vs. instance vs. BG):
    // simplest correct rule, and a real player's own boat/zeppelin transport has no bot-usable
    // equivalent anyway. Battleground entry specifically is a known open item -- teleporting a
    // bot onto a BG map doesn't run it through the BG's own join/team-assignment bookkeeping,
    // see AGENTS.md for what's confirmed vs. still-to-verify there.
    void TryFollowLeaderAcrossMaps(WorldSession* session);

    // Checked every tick for every ghosted bot not already mid-teleport and not in a
    // battleground: if the bot's own corpse is on a different map than the bot currently is
    // (a dungeon's nearest graveyard can be outside the instance entirely -- confirmed live in
    // Deadmines), teleports the bot onto the corpse's map so BotAI's corpse-walk logic has a
    // coordinate space that actually matches. Same TeleportTo()-then-queue-ack-for-next-tick
    // sequencing as TryFollowLeaderAcrossMaps/DoAcceptInvite, for the same reason (see
    // FinishPendingTeleport's comment).
    void TryReturnGhostToCorpseMap(WorldSession* session);

    // Calls the real WorldSession::HandleLootRoll handler directly with a
    // Greed vote for the given roll (itemGUID/itemSlot read straight off the
    // Roll object — no packet-guessing needed). Caller must have already
    // confirmed this bot has a pending, not-yet-answered vote on this roll.
    void DoRollGreed(WorldSession* session, Roll* roll);

    // Same shape as DoRollGreed but casts ROLL_PASS -- used when MatchesGearPreference says this
    // roll's item is an armor/weapon type this bot's class can't wear or doesn't prefer. See
    // Update()'s roll loop and docs/addon-protocol.md's SETGEARPREF verb.
    void DoRollPass(WorldSession* session, Roll* roll);

    // Checked alongside the heartbeat (every 10s): if every currently active bot session is
    // already at the level cap, logs a one-time (edge-triggered) notice that there's no lower-
    // level companion left in the active roster to send out leveling in the world.
    // Deliberately does NOT pick a character and auto-spawn it: unlike everything else in this
    // file, "which characters count as part of our companion roster" isn't something SpawnBot's
    // own account-agnostic design tracks anywhere (there's no roster/pool concept at all -- any
    // guid on any account works), so silently guessing at that pool risks pulling in a character
    // nobody meant to include, and creating a brand new one outright is against this project's
    // own standing rule of never creating characters without being asked. See AGENTS.md's open
    // question on this for what a real answer needs (an explicit config-listed twink pool, at
    // minimum) before this can safely go further than a notice.
    void CheckAllBotsMaxLevel();
    bool _allBotsMaxLevelNotified = false;

    void EnsureBotBankRights(Player* bot, Guild* guild);

    std::unordered_map<ObjectGuid, GuildGatherOrder> _guildGatherOrders;
    std::unordered_map<uint32, GatherSourceInfo> _gatherSourceInfo;
    std::vector<QueuedGatherOrder> _gatherQueue;
    uint32 _nextGatherQueueId = 1;
    uint32 _nextGatherOrderNo = 1;
    // Sends a system line to the order's requester when that is an online real player.
    void TellGatherRequester(GuildGatherOrder const& order, std::string const& text);
    void LoadGatherQueue();
    void SaveGatherQueue();
    void ProcessGatherQueue();
    // Picks the best online, ungrouped guild bot for itemEntry (level/skill fit, then not busy).
    // allowBusy=false returns nullptr when every candidate already has a gather/craft order.
    Player* PickGatherBot(uint32 guildId, uint32 itemEntry, bool allowBusy);
    bool ResolveOrderLocation(Player* bot, GuildGatherOrder const& order, bool avoidCurrentTarget,
        uint32& mapId, float& x, float& y, float& z);
    void NotifyGatherOrderDone(Player* bot, GuildGatherOrder const& order, uint32 mailedCount = 0);
    // Mails up to `count` of itemEntry from the bot's bags to the requester (whole stacks, real
    // MailDraft, 12 stacks per mail). Returns how many items went out.
    uint32 MailGatherItems(Player* bot, ObjectGuid::LowType requesterLowGuid, uint32 itemEntry, uint32 count);
    // Points the order at a new spot: walks there when it is close, teleports when it is on
    // another map or further than GATHER_ORDER_TELEPORT_BEYOND_YARDS (or `rush`).
    void SendBotToOrderSpot(Player* bot, GuildGatherOrder& order, bool rush);
    void NotifyGatherOrderFail(ObjectGuid const& botGuid, std::string const& botName, GuildGatherOrder const& order,
        std::string const& reason);
    bool CanManageGatherOrder(Player* commander, uint32 guildId, ObjectGuid::LowType requesterLowGuid) const;

    std::unordered_map<ObjectGuid, CraftOrderState> _craftOrders;
    void ProcessCraftOrders();
    bool WaitForCraftReagents(Player* crafter, SpellInfo const* info, CraftOrderState& order);
    // Counts a failed or empty cast; false when the order is given up (the requester is told why).
    bool NoteCraftFailure(Player* crafter, CraftOrderState& order, std::string const& why);

    std::vector<WorldSession*> _botSessions;
    std::unordered_map<ObjectGuid::LowType, WorldSession*> _botSessionByLowGuid;
    // One detached login per GUID; concurrent callers share its completion.
    std::unordered_map<ObjectGuid::LowType, std::vector<std::function<void(Player*)>>> _pendingBotLogins;
    std::vector<WorldSession*> _pendingTeleportAck;
    std::vector<ObjectGuid> _pendingGroupLeaves;
    std::vector<ObjectGuid::LowType> _pendingAutoLoginQueue;
    uint32 _autoLoginThrottleMs = 0;
    uint32 _heartbeatTimer = 0;
    std::unordered_set<ObjectGuid> _autoDungeonLeaders;
    std::unordered_map<ObjectGuid, std::unordered_set<uint32>> _clearedBosses;
    // How long each bot's AI has been waiting for its next decision (see the combat-AI loop in Update).
    std::unordered_map<ObjectGuid, uint32> _aiWaitMs;
    // The AI cadence settings, re-read every few seconds: the config manager logs a warning on every lookup of a key
    // that is absent from the active config file, and servers updated from older packages do not have these keys.
    uint32 _aiCombatIntervalMs = 100;
    uint32 _aiIdleIntervalMs = 250;
    uint32 _aiConfigAgeMs = 10000;
    // Trading with a player: how long each bot's open trade window has existed, and whether bots take part at all.
    std::unordered_map<ObjectGuid, uint32> _tradeAgeMs;
    bool _tradeEnabled = true;
    bool _autoLootInGroup = false;
    bool _inventorySellWhenNearVendor = true;
    bool _inventorySellOnLeaderVendor = true;
    uint32 _inventoryKeepQuality = 3;
    bool _questShareTurnIn = true;
    bool _questShareAccept = true;
    bool _questShareIgnoreEligibility = true;
    bool _questShareObjectLoot = true;
    bool _gatherWhileFollowing = true;
    float _gatherFollowRadius = 20.0f;
    void UpdateTrades(uint32 diff);
    std::mutex _killEventLock;
    std::vector<KillEvent> _killEvents;
    void ProcessKillEvents();
    std::unordered_map<ObjectGuid, BotGroupFormation> _groupFormations;

public:
    // Called once from a new WorldScript::OnStartup hook, gated behind CoaBots.AutoLoginOnStartup
    // -- queries every character on a bot-hosting account (same COABOTHOST-prefix check
    // IsBotAccountId uses elsewhere) not already online, and queues them for the same gradual,
    // throttled login BotMgr::Update already drains SpawnRandomBots/SpawnLeveledBots's queues
    // with. Also callable directly (e.g. from a GM command) to (re)populate the queue against an
    // already-running server without needing a restart.
    uint32 QueueAllBotsForAutoLogin(uint32 maxCountOverride = 0);
    bool AutoLootInGroup() const { return _autoLootInGroup; }
    bool InventorySellWhenNearVendor() const { return _inventorySellWhenNearVendor; }
    bool InventorySellOnLeaderVendor() const { return _inventorySellOnLeaderVendor; }
    uint32 InventoryKeepQuality() const { return _inventoryKeepQuality; }
    bool QuestShareTurnIn() const { return _questShareTurnIn; }
    bool QuestShareAccept() const { return _questShareAccept; }
    bool QuestShareIgnoreEligibility() const { return _questShareIgnoreEligibility; }
    bool QuestShareObjectLoot() const { return _questShareObjectLoot; }
    bool GatherWhileFollowing() const { return _gatherWhileFollowing; }
    float GatherFollowRadius() const { return _gatherFollowRadius; }
};

#define sBotMgr BotMgr::instance()

// Server -> addon reply on the COABOT channel (defined in BotAddonChat.cpp), so BotMgr can push
// unsolicited notices such as ORDERDONE / ORDERFAIL to a player running the addon.
void CoaBotSendAddonReply(Player* recipient, std::string const& body);

// Every COABOT reply, "COABOT\t" prefix included, stays at or under this many bytes (the client's
// addon-whisper path is only safe to ~255). SendCoaBotReply enforces it as a last resort.
constexpr std::size_t COABOT_MAX_REPLY_BYTES = 250;
// Wire-safe copy of free text: newlines and every char in `bad` become spaces, then it is cut to
// maxBytes on a UTF-8 boundary. Use bad = "|" for a trailing field, ":|" for a middle field, and
// ":|," inside comma lists.
std::string CoaBotWireText(std::string const& text, std::size_t maxBytes, char const* bad);
// Bytes left for a trailing free-text field after `head`.
std::size_t CoaBotWireRoom(std::string const& head);

#endif // COA_PLAYERBOTS_BOT_MGR_H
