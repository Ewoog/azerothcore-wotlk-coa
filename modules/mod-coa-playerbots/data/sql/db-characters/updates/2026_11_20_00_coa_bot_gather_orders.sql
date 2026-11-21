-- mod-coa-playerbots: guild gather order requester + gather order queue (acore_characters).
-- Idempotent: safe to run more than once, and safe whether or not the worldserver already
-- created these tables (BotMgr::LoadGuildGatherOrders does the same at startup).

CREATE TABLE IF NOT EXISTS `mod_coa_bot_guild_gather_orders` (
  `bot_guid` INT UNSIGNED NOT NULL PRIMARY KEY,
  `item_entry` INT UNSIGNED NOT NULL,
  `target_count` INT UNSIGNED NOT NULL,
  `gathered_count` INT UNSIGNED NOT NULL,
  `remaining_count` INT UNSIGNED NOT NULL,
  `target_map` INT UNSIGNED NOT NULL,
  `target_x` FLOAT NOT NULL,
  `target_y` FLOAT NOT NULL,
  `target_z` FLOAT NOT NULL,
  `has_target_location` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `requester_guid` INT UNSIGNED NOT NULL DEFAULT 0
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Tables created by an older module build lack requester_guid: add it only if missing.
SET @coa_has_requester := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'mod_coa_bot_guild_gather_orders' AND COLUMN_NAME = 'requester_guid');
SET @coa_sql := IF(@coa_has_requester = 0,
    'ALTER TABLE `mod_coa_bot_guild_gather_orders` ADD COLUMN `requester_guid` INT UNSIGNED NOT NULL DEFAULT 0',
    'DO 0');
PREPARE coa_stmt FROM @coa_sql;
EXECUTE coa_stmt;
DEALLOCATE PREPARE coa_stmt;

CREATE TABLE IF NOT EXISTS `mod_coa_bot_guild_gather_queue` (
  `queue_id` INT UNSIGNED NOT NULL PRIMARY KEY,
  `position` INT UNSIGNED NOT NULL,
  `guild_id` INT UNSIGNED NOT NULL,
  `requester_guid` INT UNSIGNED NOT NULL DEFAULT 0,
  `item_entry` INT UNSIGNED NOT NULL,
  `item_count` INT UNSIGNED NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
