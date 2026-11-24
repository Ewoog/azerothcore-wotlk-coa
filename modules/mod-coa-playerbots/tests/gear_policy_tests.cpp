#include "BotGearPolicy.h"
#include <algorithm>
#include <cassert>
#include <iostream>

int main()
{
    using namespace BotGearPolicy;
    assert(!FitsLevel(5, 0, 174, 3));
    assert(!FitsLevel(5, 1, 435, 2));
    assert(FitsLevel(5, 5, 12, 2));
    assert(!FitsLevel(5, 6, 12, 2));
    assert(!FitsLevel(5, 5, 12, 3));
    assert(!FitsLevel(0, 0, 1, 1));
    assert(!FitsLevel(80, 80, 0, 4));
    assert(!FitsLevel(80, 80, 813, 4));
    for (unsigned level = 1; level <= 80; ++level)
    {
        assert(FitsLevel(level, level, MaxItemLevel(level), MaxQuality(level)));
        assert(!FitsLevel(level, level, MaxItemLevel(level) + 1, 1));
        assert(!FitsLevel(level, level + 1, 1, 1));
    }
    assert(MaxItemLevel(60) == 92);
    assert(!FitsLevel(60, 60, 200, 4));
    assert(FitsLevel(60, 60, 92, 4));
    assert(PvpPower(101715) == 15);
    assert(PvePower(101615) == 15);
    assert(PvpPower(101615) == 0);
    assert(PvePower(101715) == 0);
    assert(PvpPower(9930957) == 10);
    assert(BeatsWorn(10.0f, 0.0f));
    assert(!BeatsWorn(0.0f, 0.0f));
    assert(!BeatsWorn(104.0f, 100.0f));
    assert(BeatsWorn(106.0f, 100.0f));
    assert(WithinReach(20, 22, 2, 60));
    assert(!WithinReach(20, 23, 2, 60));
    assert(!WithinReach(20, 21, 0, 60));
    assert(WithinReach(20, 20, 0, 60));
    assert(ReachLevel(59, 2, 60) == 60);
    assert(!WithinReach(60, 61, 2, 60));
    assert(ReachLevel(80, 2, 60) == 80);
    assert(ChooseLootRoll(true, true, true, true) == LootRoll::Need);
    assert(ChooseLootRoll(true, false, true, true) == LootRoll::Greed);
    assert(ChooseLootRoll(false, true, true, true) == LootRoll::Greed);
    assert(ChooseLootRoll(true, true, false, true) == LootRoll::Greed);
    assert(ChooseLootRoll(true, true, false, false) == LootRoll::Pass);
    assert(HeldUpgradeCapacity(false) == 1);
    assert(HeldUpgradeCapacity(true) == 2);
    {
        std::vector<HeldUpgrade> const held = {
            { 5, 1, 40.0f, 1 }, { 5, 1, 55.0f, 2 }, { 5, 1, 50.0f, 3 },
            { 11, 2, 10.0f, 4 }, { 11, 2, 30.0f, 5 }, { 11, 2, 20.0f, 6 },
            { 7, 1, 12.0f, 7 },
        };
        std::vector<std::uint64_t> kept = KeptUpgrades(held);
        std::sort(kept.begin(), kept.end());
        assert((kept == std::vector<std::uint64_t>{ 2, 5, 6, 7 }));
        assert(!OutranksHeld(held, 5, 1, 50.0f));
        assert(!OutranksHeld(held, 5, 1, 55.0f));
        assert(OutranksHeld(held, 5, 1, 56.0f));
        assert(OutranksHeld(held, 11, 2, 25.0f));
        assert(!OutranksHeld(held, 11, 2, 20.0f));
        assert(OutranksHeld(held, 9, 1, 1.0f));
    }
    {
        std::vector<HeldUpgrade> const twins = { { 5, 1, 50.0f, 9 }, { 5, 1, 50.0f, 8 } };
        assert((KeptUpgrades(twins) == std::vector<std::uint64_t>{ 8 }));
        assert(KeptUpgrades({}).empty());
    }
    std::cout << "Gear policy: reported low-level gear regression and all 80 level boundaries passed.\n";
}
