/*
 * mod-coa-playerbots
 *
 * Everything a bot acquires as a character rather than decides as an AI: the abilities and
 * recipes a real player learns from their books, companions, mounts, profession tools and a food
 * supply that keeps pace with its level. Used by fresh-bot setup and again on every level-up, so a
 * bot that levels in the world keeps learning the way a player does instead of being frozen with
 * whatever it was created with.
 *
 * Learning goes through the same paths a player's own clicks do. This realm runs
 * CoA.AutoProgression = 0, so class abilities are not handed out on level-up; a player
 * asks their Book of Ascension ("Restore my available class abilities"), which is the
 * npc_ascension_training_book gossip in mod-ascension-compat. A bot does exactly that: summons the
 * book briefly and selects that gossip option through ScriptMgr. Recipes come from the Book of
 * Artisans, a real trainer (trainer 200001 via creature 57500), taught through the engine's own
 * Trainer::CanTeachSpell rules. Neither path needs a change to the core or to mod-ascension-compat.
 */

#ifndef COA_PLAYERBOTS_BOT_PROGRESSION_H
#define COA_PLAYERBOTS_BOT_PROGRESSION_H

#include "Define.h"
#include <string>
#include <vector>

class Player;

namespace BotProgression
{
    // Class abilities available at the bot's level, via its Book of Ascension.
    void LearnAbilitiesFromBook(Player* bot);

    // Every Book of Artisans recipe the bot's current profession skills and level allow.
    // Returns how many were learned.
    uint32 LearnRecipesFromBook(Player* bot);

    // Book of Artisans and Book of Ascension companions.
    void GrantCompanions(Player* bot);

    // Two ground and two flying mounts for the bot's race and faction. Grant full riding when
    // CoA.MaxRidingFromStart is enabled; preserve existing skills when it is disabled.
    void GrantMounts(Player* bot);

    // The tools each profession needs in the bags, upgrading the enchanting rod with skill.
    void GrantProfessionTools(Player* bot);

    // Puts in the bags the tool items the bot's learned spells require (Spell.dbc TotemCategory): Witch Doctor totems, Guardian standards. Cheap to
    // call again; it only acts on a spell whose requirement the bag does not meet.
    void GrantSpellTools(Player* bot);

    // Counts the spells a bot has learned or forgotten (the core's learn/forget hooks, so a spec swap, a talent reset, a level-up that teaches
    // spells or a spell replaced by another of the same number all count, which the size of the spellbook does not show). 0 until the first
    // change. Safe to call from the world thread while a map thread learns a spell. `ForgetSpellbook` drops the bot's entry on despawn.
    uint32 SpellbookRevision(ObjectGuid botGuid);
    void ForgetSpellbook(ObjectGuid botGuid);

    // Food and drink for the bot's level, taken from what vendors actually sell.
    void ProvisionFood(Player* bot, uint32 stackSize);

    // The vendor-sold food / drink a bot of this level should eat and drink (the best one it may use), 0 when none.
    uint32 PickFood(uint8 level);
    uint32 PickDrink(uint8 level);

    // The plain healing / mana potion of the highest tier a bot of this level may use, 0 when none.
    uint32 PickHealingPotion(uint8 level);
    uint32 PickManaPotion(uint8 level);

    // Tops the bags up to `count` potions of each kind for the bot's level (mana potions only for a mana user). Like the food, handed out free
    // when a bot is made and when it levels; the economy buys them afterwards.
    void ProvisionPotions(Player* bot, uint32 count);

    // Raises profession caps to the new level, then relearns abilities, recipes, tools and food.
    // Talents are applied separately by BotTalentBuilds.
    void OnLevelUp(Player* bot, uint8 newLevel);

    // Addon-driven profession learn/unlearn (GETPROFS / SETPROF). CoA itself does not cap
    // primary professions, and SETPROF is still uncapped so a player's own alts can hold
    // every trade. Random bots are limited to two primaries plus the secondaries (see
    // AssignVariedProfessions and CoaBots.Professions.TwoPrimary). LearnProfession teaches
    // the trainer rank spells up to the level cap (level*6, max 450) and sets the starting
    // skill from CoaBots.Prof.StartSkill (0 = the cap, so gathering of that tier works
    // immediately). UnlearnProfession wipes the skill and those rank spells. Tokens: ok,
    // combat, trade, dead, teleport, invalid, unknown, missing, noskill (the realm's
    // SkillLine.dbc lacks the line, or the skill did not stick). Woodcutting and
    // Woodworking cap at 300 (four ranks); every other profession at 450.
    struct ProfessionInfo
    {
        uint32 skillId;
        char const* name;
        bool secondary;
    };
    ProfessionInfo const* ProfessionCatalog(uint32& count);
    uint16 ProfessionCapForLevel(uint8 level);
    uint16 ProfessionCapForSkill(uint32 skillId, uint8 level);
    char const* LearnProfession(Player* bot, uint32 skillId);
    char const* UnlearnProfession(Player* bot, uint32 skillId);

    // Teach one profession at the level cap, including the trainer rank spells. Used by
    // `.botcmd professiontrainer` (every profession this realm allows).
    void GrantProfessionAtCap(Player* bot, uint32 skillId);

    // Give a random bot a guid-stable pair of primary professions (class-biased weights)
    // plus Cooking, First Aid and Fishing. Extra primaries left over from the old
    // "grant every profession" factory path are unlearned. `force` skips the manual
    // flag and busy checks (fresh setup, or a GM targeting one bot by name).
    // Tokens: ok, manual, combat, trade, dead, teleport, invalid.
    char const* AssignVariedProfessions(Player* bot, bool force);

    // On login: if CoaBots.Professions.RerollLegacy is on, the character is a bot-account
    // bot, it still has the old all-profession set, and SETPROF / professiontrainer has
    // not marked it manual, assign a varied pair once. Player-account alts are left alone.
    void MaybeRerollLegacyProfessions(Player* bot);

    // SETPROF and `.botcmd professiontrainer` set this so auto-reroll will not overwrite
    // a choice a player or GM made. `.botcmd reroll professions <name>` clears it.
    void MarkProfessionsManual(Player* bot);
    bool ProfessionsAreManual(Player* bot);
    bool HasLegacyProfessionSet(Player* bot);

    // `.botcmd profstats`: online bot-account bots, counts per profession and pairing.
    void AppendProfessionStats(std::vector<std::string>& lines);
}


#endif // COA_PLAYERBOTS_BOT_PROGRESSION_H
