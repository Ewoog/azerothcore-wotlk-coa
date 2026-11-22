#include "BotEconomyPolicy.h"
#include <cassert>
#include <iostream>

int main()
{
    using namespace BotEconomyPolicy;

    Holding loose;
    assert(!MustKeep(loose));
    Holding reagent;
    reagent.reagentOfOwnCraftOrder = true;
    assert(MustKeep(reagent));
    Holding bought;
    bought.boughtRecently = true;
    assert(MustKeep(bought));
    Holding gather;
    gather.gatherOrderItem = true;
    assert(MustKeep(gather));
    Holding owner;
    owner.protectedByOwner = true;
    assert(MustKeep(owner));

    assert(StillBefore(1000, 2000));
    assert(!StillBefore(2000, 2000));
    assert(!StillBefore(3000, 2000));
    assert(StillBefore(0xFFFFFF00u, 0x00000100u));
    assert(!StillBefore(0x00000100u, 0xFFFFFF00u));

    assert(WithinPriceCeiling(2000, 1, 100, 20));
    assert(!WithinPriceCeiling(2001, 1, 100, 20));
    assert(WithinPriceCeiling(40000, 20, 100, 20));
    assert(WithinPriceCeiling(999999, 1, 0, 20));
    assert(WithinPriceCeiling(999999, 1, 100, 0));

    assert(CapLeft(0, 123456) > 1000000000000ull);
    assert(CapLeft(5000, 1000) == 4000);
    assert(CapLeft(5000, 5000) == 0);
    assert(CapLeft(5000, 9000) == 0);

    assert(IsUpgrade(111.0f, 100.0f, 10));
    assert(!IsUpgrade(110.0f, 100.0f, 10));
    assert(IsUpgrade(1.0f, 0.0f, 10));
    assert(!IsUpgrade(0.0f, 0.0f, 10));
    assert(!IsUpgrade(104.0f, 100.0f, 5));
    assert(IsUpgrade(100.0f * 1.06f, 100.0f, 5));

    assert(!GiveUpAfterFailures(9, 10));
    assert(GiveUpAfterFailures(10, 10));
    assert(!GiveUpAfterFailures(1000, 0));

    assert(VendorCost(100, 5, 5) == 100);
    assert(VendorCost(100, 5, 1) == 20);
    assert(VendorCost(100, 3, 1) == 34);
    assert(VendorCost(100, 0, 2) == 200);
    assert(VendorCost(0, 1, 10) == 0);

    assert(CraftPays(125, 100, 25));
    assert(!CraftPays(124, 100, 25));
    assert(CraftPays(1, 0, 25));
    assert(!CraftPays(0, 0, 25));
    assert(!CraftPays(100, 100, 0));

    std::cout << "Economy policy: keep rules, purchase window, price ceiling, hourly cap, upgrade margin, give-up limit, "
                 "vendor cost and craft margin passed.\n";
}
