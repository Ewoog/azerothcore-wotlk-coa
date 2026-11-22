import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SELLER = ROOT / "modules/mod-ah-bot/src"
SCALING = ROOT / "src/server/coa"
MAIN = r"""
#include "CoaAuctionItems.h"
#include "AscensionItemScalingPolicy.h"
#include <cstdio>

using CoaAuctionItems::Candidate;
using CoaAuctionItems::Sellable;

static_assert(CoaAuctionItems::FirstScaledEntry == ItemScaling::FirstScaledEntry);
static_assert(CoaAuctionItems::LastScaledEntry == ItemScaling::LastScaledEntry);

constexpr Candidate Stock(std::uint32_t entry)
{
    Candidate c;
    c.entry = entry;
    c.hasClientRow = true;
    return c;
}

constexpr Candidate With(Candidate c, bool Candidate::*field)
{
    c.*field = true;
    return c;
}

constexpr Candidate Without(Candidate c, bool Candidate::*field)
{
    c.*field = false;
    return c;
}

static_assert(Sellable(Stock(2589)));
static_assert(Sellable(Stock(56806)));
static_assert(!Sellable(Stock(56807)));
static_assert(!Sellable(Stock(560478)));
static_assert(Sellable(With(Stock(560478), &Candidate::craftedByProfession)));
static_assert(Sellable(With(Stock(2589), &Candidate::craftedByProfession)));
static_assert(!Sellable(With(Stock(4400001), &Candidate::craftedByProfession)));
static_assert(!Sellable(With(Stock(4912583), &Candidate::craftedByProfession)));
static_assert(!Sellable(Stock(4600000)));
static_assert(!Sellable(With(Stock(2589), &Candidate::worldforged)));
static_assert(!Sellable(With(With(Stock(700000), &Candidate::craftedByProfession), &Candidate::worldforged)));
static_assert(!Sellable(With(Stock(2589), &Candidate::soulbound)));
static_assert(!Sellable(With(With(Stock(700000), &Candidate::craftedByProfession), &Candidate::soulbound)));
static_assert(!Sellable(Without(Stock(2589), &Candidate::hasClientRow)));
static_assert(!Sellable(Without(With(Stock(700000), &Candidate::craftedByProfession), &Candidate::hasClientRow)));

int main()
{
    std::puts("ok");
    return 0;
}
"""


class AuctionSellerItemRules(unittest.TestCase):
    def test_seller_item_rules(self):
        compiler = shutil.which(os.environ.get("CXX", "cl.exe" if os.name == "nt" else "c++"))
        assert compiler, "Enable a C++20 compiler (VS Developer PowerShell on Windows)."
        with tempfile.TemporaryDirectory(prefix="coa-auction-seller-") as directory:
            out = Path(directory)
            (out / "main.cpp").write_text(MAIN, encoding="utf-8")
            executable = out / ("rules.exe" if os.name == "nt" else "rules")
            includes = [SELLER, SCALING]
            if Path(compiler).stem.lower() == "cl":
                flags = ["/nologo", "/std:c++20", "/EHsc", *["/I" + str(p) for p in includes], str(out / "main.cpp"),
                         "/Fe" + str(executable)]
            else:
                flags = ["-std=c++20", *["-I" + str(p) for p in includes], str(out / "main.cpp"), "-o",
                         str(executable)]
            build = subprocess.run([compiler, *flags], cwd=out, capture_output=True, text=True, errors="replace")
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(executable)], check=True, capture_output=True, text=True)
            self.assertEqual(result.stdout.strip(), "ok")


if __name__ == "__main__":
    unittest.main()
