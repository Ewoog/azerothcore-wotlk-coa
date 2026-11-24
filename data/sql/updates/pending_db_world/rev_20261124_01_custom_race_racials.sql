-- Each custom race takes the racial kit of the stock race in RacialRace instead of its base race's kit.
ALTER TABLE `coa_custom_race`
  ADD COLUMN `RacialRace` tinyint unsigned NOT NULL DEFAULT 0 AFTER `BaseRace`;

UPDATE `coa_custom_race` SET `RacialRace` = 7 WHERE `Race` = 9;
UPDATE `coa_custom_race` SET `RacialRace` = 8 WHERE `Race` = 12;
UPDATE `coa_custom_race` SET `RacialRace` = 5 WHERE `Race` = 13;
UPDATE `coa_custom_race` SET `RacialRace` = 6 WHERE `Race` = 16;
UPDATE `coa_custom_race` SET `RacialRace` = 4 WHERE `Race` = 19;
UPDATE `coa_custom_race` SET `RacialRace` = 11 WHERE `Race` = 20;

CREATE TABLE IF NOT EXISTS `coa_racial_skill_line` (
  `SkillId` smallint unsigned NOT NULL,
  PRIMARY KEY (`SkillId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
COMMENT='Skill lines that carry a race''s racial abilities.';

DELETE FROM `coa_racial_skill_line` WHERE `SkillId` IN (101, 124, 125, 126, 220, 733, 753, 754, 756, 760, 11125, 11760);
INSERT INTO `coa_racial_skill_line` (`SkillId`) VALUES
(101),
(124),
(125),
(126),
(220),
(733),
(753),
(754),
(756),
(760),
(11125),
(11760);
