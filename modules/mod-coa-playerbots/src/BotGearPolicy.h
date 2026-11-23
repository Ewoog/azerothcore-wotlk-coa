#ifndef COA_BOT_GEAR_POLICY_H
#define COA_BOT_GEAR_POLICY_H

#include <cstdint>

namespace BotGearPolicy
{
constexpr unsigned MaxItemLevel(unsigned level)
{
    return level <= 10 ? 20 : level <= 20 ? 30 : level <= 30 ? 45 :
        level <= 40 ? 55 : level <= 50 ? 65 : level < 60 ? 75 :
        level <= 60 ? 92 : level <= 70 ? 164 : level < 80 ? 200 : 232;
}

constexpr unsigned MaxQuality(unsigned level)
{
    return level <= 10 ? 2 : level < 60 ? 3 : 4;
}

constexpr bool FitsLevel(unsigned level, unsigned requiredLevel, unsigned itemLevel, unsigned quality)
{
    return level && requiredLevel <= level && itemLevel && itemLevel <= MaxItemLevel(level) &&
        quality >= 1 && quality <= MaxQuality(level);
}

constexpr unsigned PvpPower(unsigned spell)
{
    if (spell >= 101700 && spell <= 101799)
        return spell - 101700;
    return spell == 9930954 ? 5 : spell == 9930955 ? 6 : spell == 9930956 ? 7 : spell == 9930957 ? 10 : 0;
}

constexpr unsigned ReachLevel(unsigned level, unsigned levelsAhead, unsigned maxLevel)
{
    return level + levelsAhead < maxLevel ? level + levelsAhead : maxLevel > level ? maxLevel : level;
}

constexpr bool WithinReach(unsigned level, unsigned requiredLevel, unsigned levelsAhead, unsigned maxLevel)
{
    return requiredLevel <= ReachLevel(level, levelsAhead, maxLevel);
}

enum class LootRoll : std::uint8_t { Pass, Need, Greed };

constexpr bool BeatsWorn(float candidateScore, float wornScore)
{
    return candidateScore > 0.0f && candidateScore > wornScore * 1.05f;
}

constexpr LootRoll ChooseLootRoll(bool needEnabled, bool needOffered, bool upgrade, bool wearable)
{
    if (needEnabled && needOffered && upgrade)
        return LootRoll::Need;
    return wearable ? LootRoll::Greed : LootRoll::Pass;
}

constexpr unsigned PvePower(unsigned spell)
{
    return spell >= 101600 && spell <= 101699 ? spell - 101600 : 0;
}
}
#endif
