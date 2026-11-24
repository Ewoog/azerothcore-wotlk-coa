/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Player.h"
#include "gtest/gtest.h"

TEST(PlayerInfoDisplayIdTest, KeepsDisplayIdsAboveSixteenBits)
{
    PlayerInfo info;
    info.displayId_m = 141254;
    info.displayId_f = 141255;

    EXPECT_EQ(info.displayId_m, 141254u);
    EXPECT_EQ(info.displayId_f, 141255u);
}
