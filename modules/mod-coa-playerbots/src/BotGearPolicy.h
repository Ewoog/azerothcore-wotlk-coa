#ifndef COA_BOT_GEAR_POLICY_H
#define COA_BOT_GEAR_POLICY_H

#include <algorithm>
#include <cstdint>
#include <vector>

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

struct HeldUpgrade
{
    unsigned group;
    unsigned capacity;
    float score;
    std::uint64_t id;
};

constexpr unsigned HeldUpgradeCapacity(bool pairedSlots)
{
    return pairedSlots ? 2 : 1;
}

inline std::vector<std::uint64_t> KeptUpgrades(std::vector<HeldUpgrade> held)
{
    std::sort(held.begin(), held.end(), [](HeldUpgrade const& a, HeldUpgrade const& b)
    {
        if (a.group != b.group)
            return a.group < b.group;
        if (a.score != b.score)
            return a.score > b.score;
        return a.id < b.id;
    });
    std::vector<std::uint64_t> kept;
    unsigned taken = 0;
    for (std::size_t i = 0; i < held.size(); ++i)
    {
        if (i == 0 || held[i].group != held[i - 1].group)
            taken = 0;
        if (taken < held[i].capacity)
        {
            kept.push_back(held[i].id);
            ++taken;
        }
    }
    return kept;
}

inline bool OutranksHeld(std::vector<HeldUpgrade> const& held, unsigned group, unsigned capacity, float score)
{
    unsigned const better = unsigned(std::count_if(held.begin(), held.end(),
        [&](HeldUpgrade const& h) { return h.group == group && h.score >= score; }));
    return better < capacity;
}
}
#endif
