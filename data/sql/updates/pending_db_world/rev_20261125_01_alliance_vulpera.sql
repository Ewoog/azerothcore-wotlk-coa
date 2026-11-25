-- Vulpera become playable on the Alliance as race 21, a copy of Vulpera (19) that plays by the Dwarf rules:
-- Coldridge Valley start, Dwarf classes, stats, starter kit, quests and reputations, and the Night Elf racials.
DELETE FROM `coa_custom_race` WHERE `Race` = 21;
INSERT INTO `coa_custom_race` (`Race`, `BaseRace`, `RacialRace`) VALUES
(21, 3, 4);

-- The installed ChrRaces.dbc has no row 21, so the row carries Vulpera's text and models.
DELETE FROM `chrraces_dbc` WHERE `ID` = 21;
INSERT INTO `chrraces_dbc` (
    `ID`, `Flags`, `FactionID`, `ExplorationSoundID`, `MaleDisplayId`, `FemaleDisplayId`, `ClientPrefix`,
    `BaseLanguage`, `CreatureType`, `ResSicknessSpellID`, `SplashSoundID`, `ClientFilestring`,
    `CinematicSequenceID`, `Alliance`, `Name_Lang_enUS`, `Name_Lang_enGB`, `Name_Lang_koKR`, `Name_Lang_frFR`,
    `Name_Lang_deDE`, `Name_Lang_enCN`, `Name_Lang_zhCN`, `Name_Lang_enTW`, `Name_Lang_zhTW`, `Name_Lang_esES`,
    `Name_Lang_esMX`, `Name_Lang_ruRU`, `Name_Lang_ptPT`, `Name_Lang_ptBR`, `Name_Lang_itIT`, `Name_Lang_Unk`,
    `Name_Lang_Mask`, `Name_Female_Lang_enUS`, `Name_Female_Lang_enGB`, `Name_Female_Lang_koKR`,
    `Name_Female_Lang_frFR`, `Name_Female_Lang_deDE`, `Name_Female_Lang_enCN`, `Name_Female_Lang_zhCN`,
    `Name_Female_Lang_enTW`, `Name_Female_Lang_zhTW`, `Name_Female_Lang_esES`, `Name_Female_Lang_esMX`,
    `Name_Female_Lang_ruRU`, `Name_Female_Lang_ptPT`, `Name_Female_Lang_ptBR`, `Name_Female_Lang_itIT`,
    `Name_Female_Lang_Unk`, `Name_Female_Lang_Mask`, `Name_Male_Lang_enUS`, `Name_Male_Lang_enGB`,
    `Name_Male_Lang_koKR`, `Name_Male_Lang_frFR`, `Name_Male_Lang_deDE`, `Name_Male_Lang_enCN`,
    `Name_Male_Lang_zhCN`, `Name_Male_Lang_enTW`, `Name_Male_Lang_zhTW`, `Name_Male_Lang_esES`,
    `Name_Male_Lang_esMX`, `Name_Male_Lang_ruRU`, `Name_Male_Lang_ptPT`, `Name_Male_Lang_ptBR`,
    `Name_Male_Lang_itIT`, `Name_Male_Lang_Unk`, `Name_Male_Lang_Mask`, `FacialHairCustomization_1`,
    `FacialHairCustomization_2`, `HairCustomization`, `Required_Expansion`
) VALUES
(21, 4, 3, 0, 141254, 141255, 'Vu', 7, 7, 15007, 1096, 'Vulpera', 0, 0, 'Vulpera', '', '', '', '', '', '', '', '',
    '', '', '', '', '', '', '', 16712190, '', '', '', '', '', '', '', '', '', '', '', '', '', '', '', '', 16712172,
    '', '', '', '', '', '', '', '', '', '', '', '', '', '', '', '', 16712172, 'NORMAL', 'EARRINGS', 'NORMAL', 0);

DELETE FROM `playercreateinfo` WHERE `race` = 21;
INSERT INTO `playercreateinfo`
    (`race`, `class`, `map`, `zone`, `position_x`, `position_y`, `position_z`, `orientation`)
SELECT `custom`.`Race`, `base`.`class`, `base`.`map`, `base`.`zone`, `base`.`position_x`, `base`.`position_y`,
    `base`.`position_z`, `base`.`orientation`
FROM `coa_custom_race` AS `custom`
INNER JOIN `playercreateinfo` AS `base` ON `base`.`race` = `custom`.`BaseRace`
WHERE `custom`.`Race` = 21;

DELETE FROM `player_race_stats` WHERE `Race` = 21;
INSERT INTO `player_race_stats` (`Race`, `Strength`, `Agility`, `Stamina`, `Intellect`, `Spirit`)
SELECT `custom`.`Race`, `base`.`Strength`, `base`.`Agility`, `base`.`Stamina`, `base`.`Intellect`, `base`.`Spirit`
FROM `coa_custom_race` AS `custom`
INNER JOIN `player_race_stats` AS `base` ON `base`.`Race` = `custom`.`BaseRace`
WHERE `custom`.`Race` = 21;

DELETE FROM `ascension_race_class_stats` WHERE `Race` = 21;
INSERT INTO `ascension_race_class_stats`
    (`Race`, `Class`, `Level`, `Strength`, `Agility`, `Stamina`, `Intellect`, `Spirit`)
SELECT `custom`.`Race`, `base`.`Class`, `base`.`Level`, `base`.`Strength`, `base`.`Agility`, `base`.`Stamina`,
    `base`.`Intellect`, `base`.`Spirit`
FROM `coa_custom_race` AS `custom`
INNER JOIN `ascension_race_class_stats` AS `base` ON `base`.`Race` = `custom`.`BaseRace`
WHERE `custom`.`Race` = 21;

DELETE FROM `playercreateinfo_item` WHERE `race` = 21;
INSERT INTO `playercreateinfo_item` (`race`, `class`, `itemid`, `amount`, `Note`)
SELECT `custom`.`Race`, `base`.`class`, `base`.`itemid`, `base`.`amount`, `base`.`Note`
FROM `coa_custom_race` AS `custom`
INNER JOIN `playercreateinfo_item` AS `base` ON `base`.`race` = `custom`.`BaseRace`
WHERE `custom`.`Race` = 21;

DELETE FROM `playercreateinfo_action` WHERE `race` = 21;
INSERT INTO `playercreateinfo_action` (`race`, `class`, `button`, `action`, `type`)
SELECT `custom`.`Race`, `base`.`class`, `base`.`button`, `base`.`action`, `base`.`type`
FROM `coa_custom_race` AS `custom`
INNER JOIN `playercreateinfo_action` AS `base` ON `base`.`race` = `custom`.`BaseRace`
WHERE `custom`.`Race` = 21;
