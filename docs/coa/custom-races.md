# Playable custom races

Goblin, Zandalari Troll, Worgen, Kul Tiran, Vulpera and Pandaren are playable. The client already carries their
models, customization options and starting outfits (`patch-CHA.MPQ`); this change opens them on the server and
on the character create screen.

| Race | ID | Side | Base race | Faction template | Language |
|---|---|---|---|---|---|
| Goblin | 9 | Horde | Orc (2) | 2, Orgrimmar | Orcish |
| Zandalari Troll | 12 | Horde | Troll (8) | 116, Darkspear | Orcish |
| Vulpera | 19 | Horde | Orc (2) | 2, Orgrimmar | Orcish |
| Worgen | 13 | Alliance | Human (1) | 1, Stormwind | Common |
| Kul Tiran | 16 | Alliance | Human (1) | 1, Stormwind | Common |
| Pandaren | 20 | Alliance | Dwarf (3) | 3, Ironforge | Common |

## Server

`rev_20261123_01_custom_playable_races.sql` fills the world `chrraces_dbc` override (playable, can mount, side,
faction and language; every other field and all text keep the installed `ChrRaces.dbc`). It copies the base race's
`playercreateinfo` start, `player_race_stats`, `ascension_race_class_stats`, starter items and action bars, and
records the base race in `coa_custom_race`.

`RaceMgr::GetRuleRaceMask` returns the race's own bit plus its base race's bit. Every race-mask rule uses it
(`Unit::getRaceMask`, `SkillRaceClassInfo`, `playercreateinfo_skills`/`_spell_custom`/`_cast_spell`, base
reputations and account reputation), so a custom race takes the quests, items, skills, racials and starting
reputations of its base race. Race masks are 32 bits; `coa_custom_race` accepts races 1 to 32.

Companion bots of these races need `CoACustomRaces.Enable = 1` (bots of these races may log in) and
`CoACustomRaces.BotsEnable = 1` (spawners may create them) in `mod_coa_playerbots.conf`. Only races the effective
`ChrRaces` marks playable are offered.

## Client

The client reads its own race list, so every player needs three updated archives:

- `patch-M.MPQ`: `ChrRaces.dbc` with the same playable flag, side, faction and language as the server rows, and
  `UICameraAppearanceChrRaces.dbc` rows that reuse the base race's create-screen cameras.
- `patch-T.MPQ`: `CharBaseInfo.dbc` with the base race's classes appended for each custom race, so the native
  `IsRaceClassValid` accepts the pairs that `playercreateinfo` allows.
- `patch-B.MPQ`: `Interface\GlueXML\CoACustomRaces.lua` and a `CharacterCreate.xml` that loads it and adds race
  buttons 12 to 16. The overlay raises `MAX_RACES` to 16, lays the buttons out in an Alliance and a Horde column,
  uses the races' `Interface\Icons` portraits, shows the base race's background model, and tolerates races without
  flavor text.

Build them from the installed client with the archive tool, then install the three files over the client's copies
(keep the originals for rollback):

```powershell
python tools/package_custom_races_client.py `
  --client-data "F:/CoA/client/Data" `
  --output "C:/Staging/custom-races" `
  --mpqcli "F:/CoA/tools/mpqcli-windows-x86_64.exe"
```

The tool refuses a client that already has these edits, reads every member back and writes
`custom-races-client.json` with the hashes. A client without the archives does not list the races.
