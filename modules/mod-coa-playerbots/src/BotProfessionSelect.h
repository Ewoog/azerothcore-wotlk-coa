/*
 * Deterministic primary-profession pairing for random bots.
 *
 * CoA itself does not cap how many professions a character may hold, and the
 * old factory path (GrantShapedProfessions / GrantAllProfessions) taught every
 * profession to every bot, which is why they all looked the same. This header
 * picks a sensible pair from a weighted list, seeded by the character guid so
 * the choice is stable across logins and level-ups. Class only biases the
 * weights; every pair stays in the pool so the population still spreads.
 *
 * Kept free of Trinity types so the same header can be compiled into a
 * standalone distribution test.
 */

#ifndef COA_PLAYERBOTS_BOT_PROFESSION_SELECT_H
#define COA_PLAYERBOTS_BOT_PROFESSION_SELECT_H

#include <cstdint>

namespace BotProfessionSelect
{
    using u32 = std::uint32_t;
    using u16 = std::uint16_t;
    using u8  = std::uint8_t;

    constexpr u32 SKILL_ALCHEMY        = 171;
    constexpr u32 SKILL_BLACKSMITHING  = 164;
    constexpr u32 SKILL_ENCHANTING     = 333;
    constexpr u32 SKILL_ENGINEERING    = 202;
    constexpr u32 SKILL_LEATHERWORKING = 165;
    constexpr u32 SKILL_TAILORING      = 197;
    constexpr u32 SKILL_HERBALISM      = 182;
    constexpr u32 SKILL_MINING         = 186;
    constexpr u32 SKILL_SKINNING       = 393;
    constexpr u32 SKILL_JEWELCRAFTING  = 755;
    constexpr u32 SKILL_INSCRIPTION    = 773;
    constexpr u32 SKILL_WOODCUTTING    = 732;
    constexpr u32 SKILL_WOODWORKING    = 757;
    constexpr u32 SKILL_COOKING        = 185;
    constexpr u32 SKILL_FIRST_AID      = 129;
    constexpr u32 SKILL_FISHING        = 356;

    // Vanilla / WotLK class ids. Custom CoA classes should be mapped through
    // GetLegacyClassForCustomClass before they are passed as legacyClassId.
    // CLASS_TINKER (28) is also recognized via rawClassId so it can lean on
    // Engineering without changing the hunter mapping used elsewhere.
    constexpr u8 CLASS_WARRIOR      = 1;
    constexpr u8 CLASS_PALADIN      = 2;
    constexpr u8 CLASS_HUNTER       = 3;
    constexpr u8 CLASS_ROGUE        = 4;
    constexpr u8 CLASS_PRIEST       = 5;
    constexpr u8 CLASS_DEATH_KNIGHT = 6;
    constexpr u8 CLASS_SHAMAN       = 7;
    constexpr u8 CLASS_MAGE         = 8;
    constexpr u8 CLASS_WARLOCK      = 9;
    constexpr u8 CLASS_DRUID        = 11;
    constexpr u8 CLASS_TINKER       = 28;

    struct PairDef
    {
        u32 skillA;
        u32 skillB;
        u32 baseWeight;
        char const* label;
    };

    // Common, useful pairings. Base weights are what an unclassed / unknown
    // class sees. Class bonuses (see ClassBonus) raise favoured pairings
    // without removing the rest, so a plate warrior can still roll herbalism.
    constexpr PairDef kPairs[] = {
        { SKILL_MINING,         SKILL_BLACKSMITHING,  12, "Mining + Blacksmithing" },
        { SKILL_MINING,         SKILL_ENGINEERING,    12, "Mining + Engineering" },
        { SKILL_MINING,         SKILL_JEWELCRAFTING,  12, "Mining + Jewelcrafting" },
        { SKILL_HERBALISM,      SKILL_ALCHEMY,        12, "Herbalism + Alchemy" },
        { SKILL_HERBALISM,      SKILL_INSCRIPTION,    12, "Herbalism + Inscription" },
        { SKILL_SKINNING,       SKILL_LEATHERWORKING, 12, "Skinning + Leatherworking" },
        { SKILL_TAILORING,      SKILL_ENCHANTING,     12, "Tailoring + Enchanting" },
        { SKILL_WOODCUTTING,    SKILL_WOODWORKING,    12, "Woodcutting + Woodworking" },
        { SKILL_HERBALISM,      SKILL_MINING,          7, "Herbalism + Mining" },
        { SKILL_HERBALISM,      SKILL_SKINNING,        7, "Herbalism + Skinning" },
        { SKILL_MINING,         SKILL_SKINNING,        7, "Mining + Skinning" },
        { SKILL_WOODCUTTING,    SKILL_HERBALISM,       7, "Woodcutting + Herbalism" },
        { SKILL_WOODCUTTING,    SKILL_MINING,          7, "Woodcutting + Mining" },
        { SKILL_WOODCUTTING,    SKILL_SKINNING,        6, "Woodcutting + Skinning" },
        { SKILL_BLACKSMITHING,  SKILL_ENGINEERING,     4, "Blacksmithing + Engineering" },
        { SKILL_ENCHANTING,     SKILL_JEWELCRAFTING,   4, "Enchanting + Jewelcrafting" },
        { SKILL_TAILORING,      SKILL_INSCRIPTION,     4, "Tailoring + Inscription" },
    };

    constexpr u32 kPairCount = sizeof(kPairs) / sizeof(kPairs[0]);

    // Bump this if the pair table or bonus rules change and existing bots
    // should receive a new roll (their guid seed would otherwise stay put).
    constexpr u32 kSeedSalt = 0xC0A50001u;

    // MurmurHash3 / splitmix-style avalanche. A short xorshift left sequential
    // character guids (how cloned bots are numbered) stuck in a narrow band, so
    // whole batches rolled the same pairing.
    inline u32 Mix(u32 x)
    {
        std::uint64_t z = std::uint64_t(x) + 0x9E3779B97F4A7C15ULL;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z = z ^ (z >> 31);
        u32 out = u32(z);
        return out ? out : 1u;
    }

    inline bool PairMatches(u32 skillA, u32 skillB, u32 lhs, u32 rhs)
    {
        return (skillA == lhs && skillB == rhs) || (skillA == rhs && skillB == lhs);
    }

    inline u32 ClassBonus(u8 legacyClassId, u8 rawClassId, u32 skillA, u32 skillB)
    {
        u32 bonus = 0;
        if (rawClassId == CLASS_TINKER && PairMatches(skillA, skillB, SKILL_MINING, SKILL_ENGINEERING))
            bonus += 24;

        switch (legacyClassId)
        {
            case CLASS_WARRIOR:
            case CLASS_PALADIN:
            case CLASS_DEATH_KNIGHT:
                if (PairMatches(skillA, skillB, SKILL_MINING, SKILL_BLACKSMITHING) ||
                    PairMatches(skillA, skillB, SKILL_MINING, SKILL_ENGINEERING) ||
                    PairMatches(skillA, skillB, SKILL_MINING, SKILL_JEWELCRAFTING) ||
                    PairMatches(skillA, skillB, SKILL_BLACKSMITHING, SKILL_ENGINEERING))
                    bonus += 16;
                break;
            case CLASS_HUNTER:
                if (PairMatches(skillA, skillB, SKILL_MINING, SKILL_ENGINEERING) ||
                    PairMatches(skillA, skillB, SKILL_SKINNING, SKILL_LEATHERWORKING) ||
                    PairMatches(skillA, skillB, SKILL_WOODCUTTING, SKILL_WOODWORKING))
                    bonus += 16;
                break;
            case CLASS_ROGUE:
            case CLASS_DRUID:
                if (PairMatches(skillA, skillB, SKILL_SKINNING, SKILL_LEATHERWORKING) ||
                    PairMatches(skillA, skillB, SKILL_HERBALISM, SKILL_ALCHEMY) ||
                    PairMatches(skillA, skillB, SKILL_HERBALISM, SKILL_INSCRIPTION) ||
                    PairMatches(skillA, skillB, SKILL_WOODCUTTING, SKILL_WOODWORKING))
                    bonus += 16;
                break;
            case CLASS_PRIEST:
            case CLASS_MAGE:
            case CLASS_WARLOCK:
                if (PairMatches(skillA, skillB, SKILL_TAILORING, SKILL_ENCHANTING) ||
                    PairMatches(skillA, skillB, SKILL_HERBALISM, SKILL_ALCHEMY) ||
                    PairMatches(skillA, skillB, SKILL_HERBALISM, SKILL_INSCRIPTION))
                    bonus += 16;
                break;
            case CLASS_SHAMAN:
                if (PairMatches(skillA, skillB, SKILL_SKINNING, SKILL_LEATHERWORKING) ||
                    PairMatches(skillA, skillB, SKILL_MINING, SKILL_BLACKSMITHING) ||
                    PairMatches(skillA, skillB, SKILL_HERBALISM, SKILL_ALCHEMY))
                    bonus += 16;
                break;
            default:
                break;
        }
        return bonus;
    }

    inline u32 WeightFor(PairDef const& pair, u8 legacyClassId, u8 rawClassId)
    {
        return pair.baseWeight + ClassBonus(legacyClassId, rawClassId, pair.skillA, pair.skillB);
    }

    struct Pick
    {
        u32 skillA;
        u32 skillB;
        u32 pairIndex;
    };

    // Almost-uniform pick in 0 .. total-1 from a guid-stable mixer.
    inline u32 RollBound(u32 guidLow, u32 total)
    {
        if (!total)
            return 0;
        u32 mixed = Mix(guidLow ^ kSeedSalt);
        return u32((std::uint64_t(mixed) * std::uint64_t(total)) >> 32);
    }

    inline Pick ChoosePrimaryPair(u32 guidLow, u8 rawClassId, u8 legacyClassId)
    {
        u32 total = 0;
        u32 weights[kPairCount];
        for (u32 i = 0; i < kPairCount; ++i)
        {
            weights[i] = WeightFor(kPairs[i], legacyClassId, rawClassId);
            total += weights[i];
        }

        u32 roll = RollBound(guidLow, total);
        for (u32 i = 0; i < kPairCount; ++i)
        {
            if (roll < weights[i])
                return { kPairs[i].skillA, kPairs[i].skillB, i };
            roll -= weights[i];
        }
        return { kPairs[0].skillA, kPairs[0].skillB, 0 };
    }

    // 90-100% of the level cap, stable per (guid, skill).
    inline u16 SkillValueFor(u32 guidLow, u32 skillId, u16 cap)
    {
        if (!cap)
            return 1;
        u32 pct = 90 + (Mix(guidLow * 0x9E3779B9u + skillId) % 11);
        u32 value = (u32(cap) * pct) / 100;
        if (value < 1)
            value = 1;
        if (value > cap)
            value = cap;
        return u16(value);
    }

    inline bool IsPrimarySkill(u32 skillId)
    {
        switch (skillId)
        {
            case SKILL_ALCHEMY:
            case SKILL_BLACKSMITHING:
            case SKILL_ENCHANTING:
            case SKILL_ENGINEERING:
            case SKILL_LEATHERWORKING:
            case SKILL_TAILORING:
            case SKILL_HERBALISM:
            case SKILL_MINING:
            case SKILL_SKINNING:
            case SKILL_JEWELCRAFTING:
            case SKILL_INSCRIPTION:
            case SKILL_WOODCUTTING:
            case SKILL_WOODWORKING:
                return true;
            default:
                return false;
        }
    }

    inline bool IsSecondarySkill(u32 skillId)
    {
        return skillId == SKILL_COOKING || skillId == SKILL_FIRST_AID || skillId == SKILL_FISHING;
    }

    inline char const* SkillName(u32 skillId)
    {
        switch (skillId)
        {
            case SKILL_ALCHEMY:        return "Alchemy";
            case SKILL_BLACKSMITHING:  return "Blacksmithing";
            case SKILL_ENCHANTING:     return "Enchanting";
            case SKILL_ENGINEERING:    return "Engineering";
            case SKILL_LEATHERWORKING: return "Leatherworking";
            case SKILL_TAILORING:      return "Tailoring";
            case SKILL_HERBALISM:      return "Herbalism";
            case SKILL_MINING:         return "Mining";
            case SKILL_SKINNING:       return "Skinning";
            case SKILL_JEWELCRAFTING:  return "Jewelcrafting";
            case SKILL_INSCRIPTION:    return "Inscription";
            case SKILL_WOODCUTTING:    return "Woodcutting";
            case SKILL_WOODWORKING:    return "Woodworking";
            case SKILL_COOKING:        return "Cooking";
            case SKILL_FIRST_AID:      return "First Aid";
            case SKILL_FISHING:        return "Fishing";
            default:                   return "Unknown";
        }
    }
}

#endif
