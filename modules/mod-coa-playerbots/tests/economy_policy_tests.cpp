#include "BotEconomyPolicy.h"
#include <cassert>
#include <iostream>
#include <vector>

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

    assert(EstimateUnitPrice(13, 52, 3) == 52);
    assert(EstimateUnitPrice(100, 0, 3) == 300);
    assert(EstimateUnitPrice(0, 0, 3) == 1);
    assert(EstimateUnitPrice(10, 0, 0) == 10);

    assert(TradeGoodsForSale(40, 0, 0) == 40);
    assert(TradeGoodsForSale(40, 10, 0) == 30);
    assert(TradeGoodsForSale(40, 10, 25) == 5);
    assert(TradeGoodsForSale(40, 20, 25) == 0);

    assert(VendorTheRest(ListingStop::VendorBetter, false));
    assert(VendorTheRest(ListingStop::Done, false));
    assert(!VendorTheRest(ListingStop::NoListingsLeft, false));
    assert(!VendorTheRest(ListingStop::Failed, false));
    assert(VendorTheRest(ListingStop::NoListingsLeft, true));

    assert(CleanupMayVendor(false, true, PlannedSale::Auction, true));
    assert(!CleanupMayVendor(true, true, PlannedSale::Vendor, false));
    assert(CleanupMayVendor(true, false, PlannedSale::Vendor, true));
    assert(!CleanupMayVendor(true, false, PlannedSale::Auction, false));
    assert(!CleanupMayVendor(true, false, PlannedSale::NotPlanned, true));
    assert(CleanupMayVendor(true, false, PlannedSale::NotPlanned, false));

    assert(SupplyKeep(1, 10, 5, 20) == 20);
    assert(SupplyKeep(39, 10, 5, 20) == 25);
    assert(SupplyKeep(60, 10, 5, 20) == 40);
    assert(SupplyKeep(80, 10, 5, 0) == 50);

    {
        std::vector<SupplyStock> const held = { { 101, 5, 30 }, { 102, 25, 15 }, { 103, 35, 10 }, { 104, 45, 20 } };
        std::vector<SupplyStock> const surplus = SupplySurplus(held, 40, 30);
        assert(surplus.size() == 1);
        assert(surplus[0].entry == 101 && surplus[0].count == 25);
        assert(SupplySurplus(held, 40, 200).empty());
        std::vector<SupplyStock> const nothingKept = SupplySurplus(held, 40, 0);
        assert(nothingKept.size() == 3);
    }
    {
        std::vector<SupplyStock> const sameLevel = { { 201, 15, 5 }, { 202, 15, 30 } };
        std::vector<SupplyStock> const surplus = SupplySurplus(sameLevel, 20, 20);
        assert(surplus.size() == 2);
        assert(surplus[0].entry == 202 && surplus[0].count == 10);
        assert(surplus[1].entry == 201 && surplus[1].count == 5);
    }

    std::cout << "Economy policy: keep rules, purchase window, price ceiling, hourly cap, upgrade margin, give-up limit, "
                 "vendor cost, craft margin, price estimate, trade goods for sale, unsold listings, cleanup vendoring and "
                 "food and drink surplus passed.\n";
}
