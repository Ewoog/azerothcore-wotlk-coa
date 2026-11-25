# Playable custom races

Goblin, Zandalari Troll, Worgen, Kul Tiran, Vulpera and Pandaren are playable, Vulpera on both sides. The client
already carries their models, customization options and starting outfits (`patch-CHA.MPQ`); this change opens them
on the server and on the character create screen.

| Race | ID | Side | Base race | Faction template | Language |
|---|---|---|---|---|---|
| Goblin | 9 | Horde | Orc (2) | 2, Orgrimmar | Orcish |
| Zandalari Troll | 12 | Horde | Troll (8) | 116, Darkspear | Orcish |
| Vulpera | 19 | Horde | Orc (2) | 2, Orgrimmar | Orcish |
| Worgen | 13 | Alliance | Human (1) | 1, Stormwind | Common |
| Kul Tiran | 16 | Alliance | Human (1) | 1, Stormwind | Common |
| Pandaren | 20 | Alliance | Dwarf (3) | 3, Ironforge | Common |
| Vulpera (Alliance) | 21 | Alliance | Dwarf (3) | 3, Ironforge | Common |

A race belongs to one side, so Alliance Vulpera are race 21, a copy of Vulpera (19). The installed `ChrRaces.dbc`
has no row 21: `rev_20261125_01_alliance_vulpera.sql` adds it with Vulpera's models and text, and the client archive
tool copies Vulpera's `ChrRaces`, `CharSections`, `CharHairGeosets` and `CharacterFacialHairStyles` rows to race 21.
Race 21 has the Night Elf racials like Horde Vulpera, and its create screen and character backgrounds are the
Dwarf ones.

## Server

`rev_20261123_01_custom_playable_races.sql` fills the world `chrraces_dbc` override (playable, can mount, side,
faction and language; every other field and all text keep the installed `ChrRaces.dbc`). It copies the base race's
`playercreateinfo` start, `player_race_stats`, `ascension_race_class_stats`, starter items and action bars, and
records the base race in `coa_custom_race`.

`RaceMgr::GetRuleRaceMask` returns the race's own bit plus its base race's bit. Every race-mask rule uses it
(`Unit::getRaceMask`, `SkillRaceClassInfo`, `playercreateinfo_skills`/`_spell_custom`/`_cast_spell`, base
reputations and account reputation), so a custom race takes the quests, items, skills, racials and starting
reputations of its base race. Race masks are 32 bits; `coa_custom_race` accepts races 1 to 32.

Racials are the exception: `coa_custom_race.RacialRace` names the stock race whose racial kit the race gets, and
`coa_racial_skill_line` lists the racial skill lines. For those lines `RaceMgr::GetSkillRaceMask` uses the race's own
bit plus its racial race's bit instead of its base race's. Goblin has the Gnome kit, Zandalari Troll the Troll kit,
Worgen the Undead kit, Kul Tiran the Tauren kit, Vulpera the Night Elf kit and Pandaren the Draenei kit. Existing
characters lose their base race's racial skill and learn the new one at their next login.

Companion bots of these races need `CoACustomRaces.Enable = 1` (bots of these races may log in) and
`CoACustomRaces.BotsEnable = 1` (spawners may create them) in `mod_coa_playerbots.conf`. Only races the effective
`ChrRaces` marks playable are offered.

## Client

The client reads its own race list, so every player needs three updated archives:

- `patch-M.MPQ`: `ChrRaces.dbc` with the same playable flag, side, faction and language as the server rows,
  `UICameraAppearanceChrRaces.dbc` rows that reuse the base race's create-screen cameras, base race starting outfits
  in `CharStartOutfit.dbc`, and the race 21 copies of Vulpera's customization rows.
- `patch-T.MPQ`: `CharBaseInfo.dbc` with the base race's classes appended for each custom race, so the native
  `IsRaceClassValid` accepts the pairs that `playercreateinfo` allows.
- `patch-B.MPQ`: `Interface\GlueXML\CoACustomRaces.lua` and a `CharacterCreate.xml` that loads it and adds race
  buttons 12 to 17. The overlay raises `MAX_RACES` to 17, lays the buttons out in an Alliance and a Horde column,
  uses the races' `Interface\Icons` portraits, shows the base race's background model, and tolerates races without
  flavor text. It also gives the new races their base race's glue music, fog and lights.
- `patch-B.MPQ` also gets `Interface\FrameXML\CoACustomRaces.lua`, loaded from `FrameXML.toc`, which gives the new
  races their base race's character and transmog backgrounds and adds them to `Enum.Race`.

Most helm models have no variant for the new races' client prefixes and would render as a placeholder cube, so the
server keeps Show Helm off for every race listed in `coa_custom_race`.

Build them from the installed client with the archive tool, then install the three files over the client's copies
(keep the originals for rollback):

```powershell
python tools/package_custom_races_client.py `
  --client-data "F:/CoA/client/Data" `
  --output "C:/Staging/custom-races" `
  --mpqcli "F:/CoA/tools/mpqcli-windows-x86_64.exe"
```

The tool refuses a client that already has these edits, so build from the original archives (on the live PC,
`F:\CoA\patches\client-backup-20261123`). It reads every member back and writes `custom-races-client.json` with the
hashes. `server-dbc/` holds the `CharStartOutfit.dbc` and `CharSections.dbc` the server needs in its `DataDir/dbc`;
the server checks a new character's appearance against `CharSections`. A client without the archives does not list
the races.
