-- AltBots: remembers each of your characters' GUIDs when you log into them, and
-- syncs that list with the server's list for your account (GETALTS), so characters
-- removed by a wipe disappear and new ones show up without a login on each. Then
-- your main can spawn them as CoA Companions bots and invite them.
-- Alts tab: spawn, quest solo, invite or despawn. Commands tab: follow, stay,
-- attack, stop attack, pull, farm and inventory (clean bags) orders for one
-- alt or every alt in your group.
-- Party tab: inspect, role, spec, gear preferences, professions, equipped gear
-- and bags of grouped bots (COABOT GETGEAR/GETBAGS/EQUIP/UNEQUIP/GETPROFS/SETPROF).
-- It only sends .botcmd chat commands, COABOT addon messages to yourself
-- and a normal group invite. No combat automation runs in the client.

AltBotsDB = AltBotsDB or {}

local PREFIX = "COABOT"
local SPAWN_DELAY = 4
local SEND_GAP = 0.25
local ALL_GROUPED = "*"
local pending = {}
local queue = {}
local nextSendAt = 0
local frame = CreateFrame("Frame", "AltBotsEventFrame", UIParent)

local CLASS_COLORS = RAID_CLASS_COLORS or {}

-- Classic paperdoll slot layout (shirt/tabard omitted; GETGEAR skips them).
-- Compact two-row paperdoll so bags keep a readable area underneath.
local PAPERDOLL_ROWS = {
    { 0, 1, 2, 14, 4, 8, 9, 5, 6, 7 },
    { 10, 11, 12, 13, 15, 16, 17 },
}
local GEAR_SLOT_NAMES = {
    [0] = "Head", [1] = "Neck", [2] = "Shoulders", [4] = "Chest", [5] = "Waist",
    [6] = "Legs", [7] = "Feet", [8] = "Wrists", [9] = "Hands", [10] = "Finger 1",
    [11] = "Finger 2", [12] = "Trinket 1", [13] = "Trinket 2", [14] = "Back",
    [15] = "Main Hand", [16] = "Off Hand", [17] = "Ranged",
}

local ARMOR_TYPES = { "auto", "cloth", "leather", "mail", "plate" }
local WEAPON_TYPES = { "auto", "sword", "mace", "axe", "fist", "dagger", "staff" }
local ROLE_ORDER = { "auto", "tank", "healer", "dps", "support" }
local ROLE_LABELS = { auto = "Auto", tank = "Tank", healer = "Healer", dps = "DPS", support = "Support" }

local RESULT_TEXT = {
    ok = "done",
    combat = "is in combat",
    trade = "is trading",
    teleport = "is teleporting",
    dead = "is dead",
    empty = "that slot is empty",
    invalid = "that slot is not valid",
    cannot = "cannot equip that item",
    nospace = "has no bag space",
    level = "too high level",
}

local function Print(msg)
    DEFAULT_CHAT_FRAME:AddMessage("|cFF33CCFFAltBots:|r " .. msg)
end

local function LowGuid(guid)
    if not guid then return nil end
    return tonumber(string.sub(guid, -8), 16)
end

-- Saved data, version 2:
--   AltBotsDB.realms[realm].chars[name] = { guid, classId, classFile, level, seen, source, stale }
--   source is "login" (you logged into it), "server" (the account list from GETALTS) or "manual" (/alt add).
--   stale marks a character that could not be checked since the server looked reset.
-- Version 1 kept AltBotsDB[realm][name] = guid next to the settings; MigrateDB moves that over.
local DB_VERSION = 2
local SETTING_KEYS = {
    version = true, realms = true, farmRadius = true, minimapAngle = true, minimapHidden = true, tab = true,
}
local SYNC_DELAY, SYNC_TIMEOUT, SYNC_REFRESH = 3, 8, 60

-- 3.3.5's UnitClass has no class id, and GETALTS sends ids, so map the stock ones for class colors.
local CLASS_FILE_BY_ID = {
    [1] = "WARRIOR", [2] = "PALADIN", [3] = "HUNTER", [4] = "ROGUE", [5] = "PRIEST", [6] = "DEATHKNIGHT",
    [7] = "SHAMAN", [8] = "MAGE", [9] = "WARLOCK", [11] = "DRUID",
}

local spawned = {}
local altSync = { state = "idle" }
local syncAt, syncScheduled
local OnAltsChanged

local function Realm() return GetRealmName() or "?" end

local function MigrateDB()
    if type(AltBotsDB) ~= "table" then AltBotsDB = {} end
    AltBotsDB.realms = AltBotsDB.realms or {}
    for key, value in pairs(AltBotsDB) do
        if not SETTING_KEYS[key] and type(value) == "table" then
            local realm = AltBotsDB.realms[key] or { chars = {} }
            realm.chars = realm.chars or {}
            for name, guid in pairs(value) do
                if type(name) == "string" and tonumber(guid) and not realm.chars[name] then
                    realm.chars[name] = { guid = tonumber(guid), source = "login" }
                end
            end
            AltBotsDB.realms[key] = realm
            AltBotsDB[key] = nil
        end
    end
    AltBotsDB.version = DB_VERSION
end

local function RealmData()
    if type(AltBotsDB.realms) ~= "table" then MigrateDB() end
    local key = Realm()
    local data = AltBotsDB.realms[key]
    if not data then
        data = { chars = {} }
        AltBotsDB.realms[key] = data
    end
    data.chars = data.chars or {}
    return data
end

local function Alts() return RealmData().chars end

local function AltGuid(name)
    local info = name and Alts()[name]
    return info and info.guid
end

local function StaleCount()
    local n = 0
    for name, info in pairs(Alts()) do
        if info.stale and name ~= UnitName("player") then n = n + 1 end
    end
    return n
end

local function RemoveAlt(name)
    if not name or not Alts()[name] then return false end
    Alts()[name] = nil
    spawned[name] = nil
    return true
end

local function RegisterSelf()
    local name = UnitName("player")
    local low = LowGuid(UnitGUID("player"))
    if not name or not low then return end
    local alts = Alts()
    local old = alts[name]
    for other, info in pairs(alts) do
        if other ~= name and info.guid == low then
            alts[other] = nil
            Print("Removed " .. other .. ": its character id now belongs to " .. name .. ".")
        end
    end
    if old and old.guid ~= low then
        local marked = 0
        for other, info in pairs(alts) do
            if other ~= name and not info.stale then
                info.stale = true
                marked = marked + 1
            end
        end
        if marked > 0 then
            Print(name .. " has a new character id, so the server looks reset. Checking your other " ..
                marked .. " saved character" .. (marked == 1 and "" or "s") .. " against the server...")
        end
    end
    local _, classFile = UnitClass("player")
    alts[name] = {
        guid = low, classFile = classFile, classId = old and old.guid == low and old.classId or nil,
        level = UnitLevel("player"), seen = time(), source = "login",
    }
end

local function GM(cmd) SendChatMessage(cmd, "SAY") end

local function Queue(item) table.insert(queue, item) end

local function SpawnAndInvite(name)
    local low = AltGuid(name)
    if not low then Print("I don't know '" .. tostring(name) .. "' yet. Press Sync on the Alts tab, or log into it once.") return end
    if name == UnitName("player") then Print("That's you.") return end
    Print("Spawning " .. name .. " (guid " .. low .. ")...")
    spawned[name] = true
    Queue({ gm = ".botcmd spawnbot " .. low, after = function()
        pending[name] = { at = GetTime() + SPAWN_DELAY }
    end })
end

local function InviteKnownAlt(name)
    local low = AltGuid(name)
    if low then
        Queue({ addon = "INVITEBOT:" .. tostring(low) })
        Print("Inviting " .. name .. ".")
    else
        InviteUnit(name)
        Print("Invited " .. name .. ".")
    end
end

local function RequestAltSync(quiet)
    altSync = { state = "waiting", sentAt = GetTime(), quiet = quiet, list = {} }
    Queue({ addon = "GETALTS:0" })
end

local function ApplyAltSync(list)
    local data = RealmData()
    local old = data.chars
    local fresh, added, removed = {}, {}, {}
    local me = UnitName("player")
    for _, c in ipairs(list) do
        local prev = old[c.name]
        local same = prev and prev.guid == c.guid
        fresh[c.name] = {
            guid = c.guid, classId = c.classId, level = c.level,
            classFile = CLASS_FILE_BY_ID[c.classId] or (same and prev.classFile) or nil,
            seen = same and prev.seen or nil, source = (same and prev.source == "login") and "login" or "server",
        }
        if not same and c.name ~= me then table.insert(added, c.name) end
        if c.online then spawned[c.name] = true else spawned[c.name] = nil end
    end
    for name, info in pairs(old) do
        if not fresh[name] then
            if info.source == "manual" then
                fresh[name] = info
            else
                table.insert(removed, name)
                spawned[name] = nil
            end
        end
    end
    data.chars = fresh
    data.syncedAt = time()
    data.serverList = true
    table.sort(added)
    table.sort(removed)
    if #removed > 0 then
        Print("Removed " .. #removed .. " character" .. (#removed == 1 and "" or "s") ..
            " that no longer exist on your account: " .. table.concat(removed, ", ") .. ".")
    end
    if #added > 0 then
        Print("Found " .. #added .. " character" .. (#added == 1 and "" or "s") .. " on your account: " ..
            table.concat(added, ", ") .. ".")
    end
    if not altSync.quiet and #removed == 0 and #added == 0 then
        Print("Your saved characters match the server.")
    end
end

local function OnAltSyncTimeout()
    altSync.state = "unsupported"
    RealmData().serverList = false
    local stale = StaleCount()
    if not altSync.quiet then
        Print("The server did not send a character list. It needs the updated bot module for that.")
    end
    if stale > 0 then
        Print(stale .. " saved character" .. (stale == 1 and "" or "s") .. " could not be checked. " ..
            "Remove them on the Alts tab, or type /alt prune to remove them all.")
    end
end


local function SendBot(verb, guidLow, extra)
    if not guidLow then return end
    local body = verb .. ":" .. tostring(guidLow)
    if extra and extra ~= "" then
        body = body .. ":" .. extra
    end
    Queue({ addon = body })
end

local rolesCache, currentRoleCache, specsCache, specsPending = {}, {}, {}, {}
local gearCache, gearPrefsCache, gearLocksCache = {}, {}, {}
local bagMeta, bagItems, bagsComplete = {}, {}, {}
local profCache, profAllowed, profLimit = {}, {}, {}

local BACKPACK_BAG = 255
local BACKPACK_FIRST_SLOT = 23
local WORN_BAGS = { 19, 20, 21, 22 }
local EMPTY_BAG_TEXTURE = "Interface\\PaperDoll\\UI-Backpack-EmptySlot"

local PROFESSION_CATALOG = {
    { skillId = 171, name = "Alchemy",        icon = "Interface\\Icons\\Trade_Alchemy",          secondary = false },
    { skillId = 164, name = "Blacksmithing",  icon = "Interface\\Icons\\Trade_BlackSmithing",    secondary = false },
    { skillId = 333, name = "Enchanting",     icon = "Interface\\Icons\\Trade_Engraving",        secondary = false },
    { skillId = 202, name = "Engineering",    icon = "Interface\\Icons\\Trade_Engineering",      secondary = false },
    { skillId = 165, name = "Leatherworking", icon = "Interface\\Icons\\INV_Misc_ArmorKit_17",   secondary = false },
    { skillId = 197, name = "Tailoring",      icon = "Interface\\Icons\\Trade_Tailoring",        secondary = false },
    { skillId = 182, name = "Herbalism",      icon = "Interface\\Icons\\Trade_Herbalism",        secondary = false },
    { skillId = 186, name = "Mining",         icon = "Interface\\Icons\\Trade_Mining",           secondary = false },
    { skillId = 393, name = "Skinning",       icon = "Interface\\Icons\\INV_Misc_Pelt_Wolf_01",  secondary = false },
    { skillId = 755, name = "Jewelcrafting",  icon = "Interface\\Icons\\INV_Misc_Gem_02",        secondary = false },
    { skillId = 773, name = "Inscription",    icon = "Interface\\Icons\\INV_Inscription_Tradeskill01", secondary = false },
    { skillId = 732, name = "Woodcutting",    icon = "Interface\\Icons\\INV_Axe_10",             secondary = false },
    { skillId = 757, name = "Woodworking",    icon = "Interface\\Icons\\INV_Misc_Statue_07",     secondary = false },
    { skillId = 185, name = "Cooking",        icon = "Interface\\Icons\\INV_Misc_Food_15",       secondary = true },
    { skillId = 129, name = "First Aid",      icon = "Interface\\Icons\\Spell_Holy_SealOfSacrifice", secondary = true },
    { skillId = 356, name = "Fishing",        icon = "Interface\\Icons\\Trade_Fishing",          secondary = true },
}

local PROF_RESULT_TEXT = {
    ok = "done",
    combat = "is in combat",
    trade = "is trading",
    teleport = "is teleporting",
    dead = "is dead",
    invalid = "could not change that profession",
    unknown = "does not recognize that profession",
    missing = "does not know that profession",
    noskill = "could not get that profession (the server has no such skill line)",
}

local function ProfessionName(skillId)
    for _, info in ipairs(PROFESSION_CATALOG) do
        if info.skillId == skillId then return info.name end
    end
    return "profession " .. tostring(skillId)
end
local itemInfoRetryAt, itemInfoTries = 0, 0
local selectedPartyGuid, selectedPartyName, selectedPartyUnit
local bagsRefreshing, bagsRefreshStarted = {}, {}
local delayedRefreshAt, delayedRefreshGuids = nil, nil

local RefreshParty, RefreshPartyBags, RefreshPartyGear, RefreshPartyProfs, RefreshAlts, RefreshCommands
local EnsureWindow, ShowTab, SetStatus, UpdateBagHeader, RequestGearAndBags, LayoutPartyDetail
local BuildQuestPane, RefreshQuests, OnQuestLogArrived
-- Quest tab: questLog[guid] is the last complete GETQUESTS answer, questBuild[guid] the one
-- still arriving, questPending[guid] the GetTime() a request went out (nil once answered).
local questLog, questBuild, questPending = {}, {}, {}
local selectedQuestGuid, selectedQuestName, selectedQuestUnit
local QUEST_TIMEOUT, QUEST_AUTO_REFRESH = 6, 20
local questAutoAt = 0

local function SplitColon(str)
    local parts = {}
    local start = 1
    str = str or ""
    while true do
        local sep = string.find(str, ":", start, true)
        if not sep then
            table.insert(parts, string.sub(str, start))
            break
        end
        table.insert(parts, string.sub(str, start, sep - 1))
        start = sep + 1
    end
    return parts
end

local function FormatCopper(copper)
    copper = tonumber(copper) or 0
    local g = math.floor(copper / 10000)
    local s = math.floor((copper % 10000) / 100)
    local c = copper % 100
    if g > 0 then return g .. "g " .. s .. "s " .. c .. "c" end
    if s > 0 then return s .. "s " .. c .. "c" end
    return c .. "c"
end

local function SlotLocked(guid, slot)
    local mask = gearLocksCache[guid] or 0
    if not slot or slot < 0 or slot > 31 then return false end
    return (math.floor(mask / (2 ^ slot)) % 2) == 1
end

-- GetItemInfo's 5th return is itemMinLevel in 3.3.5.
local function ItemMinLevel(entry)
    if not entry or entry == 0 or not GetItemInfo then return 0 end
    local _, _, _, _, minLevel = GetItemInfo(entry)
    return tonumber(minLevel) or 0
end

local function SelectedBotLevel()
    if selectedPartyUnit and UnitExists(selectedPartyUnit) then
        return UnitLevel(selectedPartyUnit) or 0
    end
    return 0
end

local function ItemTooHighLevel(entry)
    local need = ItemMinLevel(entry)
    local have = SelectedBotLevel()
    return need > 0 and have > 0 and need > have, need, have
end

local function HandleIncoming(body)
    if not body or body == "" then return end
    local parts = SplitColon(body)
    local verb = parts[1]
    local guid = tonumber(parts[2])
    if not verb then return end

    if verb == "ALTCHAR" then
        local g, classId, level, _, online, name = body:match("^ALTCHAR:(%d+):(%d+):(%d+):(%d+):([01]):(.+)$")
        if g and altSync.state == "waiting" then
            table.insert(altSync.list, {
                guid = tonumber(g), classId = tonumber(classId), level = tonumber(level),
                online = online == "1", name = name,
            })
        end
        return

    elseif verb == "ALTCHAREND" then
        if altSync.state ~= "waiting" then return end
        altSync.state = "done"
        ApplyAltSync(altSync.list)
        altSync.list = nil
        if OnAltsChanged then OnAltsChanged() end
        return

    elseif verb == "ROLES" and guid then
        local set = {}
        for roleId in string.gmatch(parts[3] or "", "[^,]+") do
            set[roleId] = true
        end
        rolesCache[guid] = set
        if parts[4] and parts[4] ~= "" then currentRoleCache[guid] = parts[4] end
        if RefreshParty then RefreshParty() end

    elseif verb == "SPEC" and guid then
        local specId = tonumber(parts[3])
        if not specId then return end
        specsCache[guid] = specsCache[guid] or {}
        table.insert(specsCache[guid], { specId = specId, role = parts[4], name = parts[5] or ("Spec " .. specId) })
        specsPending[guid] = nil
        if RefreshParty then RefreshParty() end

    elseif verb == "GEAR" and guid then
        local slot = tonumber(parts[3])
        local entry = tonumber(parts[4])
        if not slot then return end
        gearCache[guid] = gearCache[guid] or {}
        gearCache[guid][slot] = { entry = entry, itemLevel = tonumber(parts[6]) or 0 }
        if RefreshPartyGear then RefreshPartyGear() end

    elseif verb == "GEARPREFS" and guid then
        local legalArmor, legalWeapon = {}, {}
        for t in string.gmatch(parts[3] or "", "[^,]+") do table.insert(legalArmor, t) end
        for t in string.gmatch(parts[4] or "", "[^,]+") do table.insert(legalWeapon, t) end
        gearPrefsCache[guid] = {
            legalArmor = legalArmor, legalWeapon = legalWeapon,
            armorPref = parts[5] or "auto", weaponPref = parts[6] or "auto",
        }
        if RefreshParty then RefreshParty() end

    elseif verb == "GEARLOCKS" and guid then
        gearLocksCache[guid] = tonumber(parts[3]) or 0
        if RefreshPartyGear then RefreshPartyGear() end

    elseif verb == "BAGMETA" and guid then
        bagMeta[guid] = {
            backpack = tonumber(parts[3]) or 16,
            bags = {
                tonumber(parts[4]) or 0, tonumber(parts[5]) or 0,
                tonumber(parts[6]) or 0, tonumber(parts[7]) or 0,
            },
        }
        bagItems[guid] = {}
        bagsComplete[guid] = nil

    elseif verb == "BAGS" and guid then
        bagItems[guid] = bagItems[guid] or {}
        local rest = body:match("^BAGS:[^:]+:[^:]+:[^:]+:(.*)$") or ""
        for chunk in string.gmatch(rest, "[^|]+") do
            local bag, slot, entry, count = chunk:match("^(%d+):(%d+):(%d+):(%d+)$")
            if bag then
                bagItems[guid][(tonumber(bag) * 1000) + tonumber(slot)] = {
                    bag = tonumber(bag), slot = tonumber(slot),
                    entry = tonumber(entry), count = tonumber(count),
                }
            end
        end

    elseif verb == "BAGSEND" and guid then
        local n = tonumber(parts[3]) or 0
        bagsComplete[guid] = n
        bagsRefreshing[guid] = nil
        itemInfoTries = 6
        itemInfoRetryAt = GetTime() + 0.4
        if UpdateBagHeader then UpdateBagHeader() end
        if RefreshPartyBags then RefreshPartyBags() end
        if selectedPartyGuid == guid then
            SetStatus((selectedPartyName or "Bot") .. ": " .. n .. " item" .. (n == 1 and "" or "s") .. " in bags.")
        end

    elseif verb == "INVENTORY" and guid then
        local copper = tonumber(parts[3]) or 0
        local sold = tonumber(parts[4]) or 0
        local who = selectedPartyName or ("guid " .. guid)
        SetStatus(who .. " sold " .. sold .. " stack(s) for " .. FormatCopper(copper) .. ".")
        if RequestGearAndBags then RequestGearAndBags(guid, "Cleaning bags finished.") end

    elseif (verb == "EQUIPRESULT" or verb == "UNEQUIPRESULT") and guid then
        local status, token = parts[3], parts[4]
        local who = selectedPartyName or ("guid " .. guid)
        local msg
        if status == "ok" then
            msg = (verb == "EQUIPRESULT" and "Equipped on " or "Unequipped on ") .. who .. "."
            if RequestGearAndBags then RequestGearAndBags(guid, "Refreshing...") end
        elseif token == "level" then
            msg = "Too high level for " .. who .. "."
        else
            msg = who .. " " .. (RESULT_TEXT[token] or (token or "could not do that")) .. "."
        end
        Print(msg)
        SetStatus(msg)

    elseif verb == "UNLOCKRESULT" and guid then
        SendBot("GETGEAR", guid)
        SetStatus("Auto-upgrade unlocked on " .. (selectedPartyName or "bot") .. ".")

    elseif verb == "PROFALLOWED" and guid then
        profLimit[guid] = tonumber(parts[3]) or 0
        local allowed = {}
        for id in string.gmatch(parts[4] or "", "[^,]+") do
            local n = tonumber(id)
            if n then allowed[n] = true end
        end
        profAllowed[guid] = allowed
        if RefreshPartyProfs then RefreshPartyProfs() end

    elseif (verb == "PROFS" or verb == "PROFSMORE") and guid then
        -- PROFS starts a fresh list; PROFSMORE (newer servers, only when one line would be too
        -- long) adds the rest of the same list.
        local list = (verb == "PROFSMORE" and profCache[guid]) or {}
        local rest = body:match("^" .. verb .. ":[^:]+:(.*)$") or ""
        for chunk in string.gmatch(rest, "[^|]+") do
            local skillId, value, maxValue = chunk:match("^(%d+):(%d+):(%d+)$")
            if skillId then
                list[tonumber(skillId)] = { value = tonumber(value) or 0, maxValue = tonumber(maxValue) or 0 }
            end
        end
        profCache[guid] = list
        if RefreshPartyProfs then RefreshPartyProfs() end

    elseif verb == "PROFRESULT" and guid then
        -- PROFRESULT:<guid>:<ok|err>:<token>:<skillId>[:<learn|unlearn>:<value>:<max>]
        local status, token, skillId = parts[3], parts[4], tonumber(parts[5])
        local action, value, maxValue = parts[6], tonumber(parts[7]), tonumber(parts[8])
        local who = selectedPartyName or ("guid " .. guid)
        local pname = ProfessionName(skillId)
        local msg
        if status == "ok" and action == "learn" then
            msg = who .. " learned " .. pname .. ((value and maxValue and maxValue > 0) and (" (" .. value .. "/" .. maxValue .. ")") or "") .. "."
        elseif status == "ok" and action == "unlearn" then
            msg = who .. " unlearned " .. pname .. "."
        elseif status == "ok" then
            msg = who .. ": " .. pname .. " updated."
        else
            local outcome = (action == "unlearn") and " was not unlearned." or " was not learned."
            msg = who .. " " .. (PROF_RESULT_TEXT[token] or ("could not change professions (" .. tostring(token) .. ")")) ..
                ", so " .. pname .. outcome
        end
        Print(msg)
        SetStatus(msg)

    elseif verb == "QUESTHDR" and guid then
        questBuild[guid] = {
            count = tonumber(parts[3]) or 0, botLevel = tonumber(parts[4]) or 0,
            list = {}, byId = {},
        }

    elseif verb == "QUEST" and guid then
        -- Title is the last field and may contain colons: take the rest of the line.
        local qid, level, status, title = body:match("^QUEST:%d+:(%d+):(%-?%d+):(%d+):(.*)$")
        local log = questBuild[guid]
        if qid and log then
            local q = {
                id = tonumber(qid), level = tonumber(level) or 0, status = tonumber(status) or 0,
                title = (title and title ~= "") and title or ("Quest " .. qid), objectives = {},
            }
            table.insert(log.list, q)
            log.byId[q.id] = q
        end

    elseif verb == "QOBJ" and guid then
        local qid, kind, entry, have, need, name = body:match("^QOBJ:%d+:(%d+):(%a):(%-?%d+):(%d+):(%d+):(.*)$")
        local log = questBuild[guid]
        local q = qid and log and log.byId[tonumber(qid)]
        if q then
            table.insert(q.objectives, {
                kind = kind, entry = tonumber(entry) or 0,
                have = tonumber(have) or 0, need = tonumber(need) or 0, name = name or "",
            })
        end

    elseif verb == "QUESTEND" and guid then
        local log = questBuild[guid] or { count = 0, botLevel = 0, list = {}, byId = {} }
        questBuild[guid] = nil
        log.at = GetTime()
        questLog[guid] = log
        questPending[guid] = nil
        itemInfoTries = 6
        itemInfoRetryAt = GetTime() + 0.4
        if OnQuestLogArrived then OnQuestLogArrived(guid, log) end
    end
end

RequestGearAndBags = function(guid, statusText)
    if not guid then return end
    gearCache[guid] = {}
    bagItems[guid] = {}
    bagsComplete[guid] = nil
    bagsRefreshing[guid] = true
    bagsRefreshStarted[guid] = GetTime()
    SendBot("GETGEAR", guid)
    SendBot("GETBAGS", guid)
    itemInfoTries = 6
    itemInfoRetryAt = GetTime() + 0.5
    if statusText then SetStatus(statusText) end
    if UpdateBagHeader then UpdateBagHeader() end
    if RefreshPartyGear then RefreshPartyGear() end
    if RefreshPartyBags then RefreshPartyBags() end
end

local function RequestAllPartyBags()
    local n = 0
    local raid = GetNumRaidMembers()
    if raid > 0 then
        for i = 1, raid do
            local u = "raid" .. i
            if UnitExists(u) and not UnitIsUnit(u, "player") then
                local g = LowGuid(UnitGUID(u))
                if g then RequestGearAndBags(g); n = n + 1 end
            end
        end
    else
        for i = 1, GetNumPartyMembers() do
            local u = "party" .. i
            if UnitExists(u) then
                local g = LowGuid(UnitGUID(u))
                if g then RequestGearAndBags(g); n = n + 1 end
            end
        end
    end
    SetStatus(n > 0 and ("Refreshing bags for " .. n .. " party member" .. (n == 1 and "" or "s") .. "...") or "No party members to refresh.")
end

local function RequestPartyData(guid)
    if not guid then return end
    specsCache[guid] = {}
    specsPending[guid] = GetTime()
    SendBot("GETROLES", guid)
    SendBot("GETSPECS", guid)
    SendBot("GETPROFS", guid)
    RequestGearAndBags(guid, "Loading " .. (selectedPartyName or "bot") .. "...")
end

local function GroupedNames()
    local set = {}
    local raid = GetNumRaidMembers()
    if raid > 0 then
        for i = 1, raid do
            local n = UnitName("raid" .. i)
            if n then set[n] = true end
        end
    else
        for i = 1, GetNumPartyMembers() do
            local n = UnitName("party" .. i)
            if n then set[n] = true end
        end
    end
    return set
end

local function SortedAltNames()
    local names = {}
    for n in pairs(Alts()) do if n ~= UnitName("player") then table.insert(names, n) end end
    table.sort(names)
    return names
end

local function ResolveAlt(name)
    if not name or name == "" then return nil end
    if Alts()[name] then return name end
    local lower = name:lower()
    for n in pairs(Alts()) do
        if n:lower() == lower then return n end
    end
    return nil
end

local function OrderTargets(choice)
    local out = {}
    if choice == ALL_GROUPED then
        local grouped = GroupedNames()
        for _, n in ipairs(SortedAltNames()) do
            if grouped[n] then table.insert(out, n) end
        end
        if #out == 0 then Print("None of your known alts are in your group.") end
    else
        local n = ResolveAlt(choice)
        if n then table.insert(out, n)
        else Print("I don't know '" .. tostring(choice) .. "' yet. Press Sync on the Alts tab, or log into it once.") end
    end
    return out
end

local function ClampRadius(r)
    r = tonumber(r) or 40
    if r < 10 then r = 10 elseif r > 100 then r = 100 end
    return math.floor(r)
end

local function HaveCreatureTarget()
    if not UnitExists("target") or UnitIsPlayer("target") or UnitIsDead("target") then
        Print("Select a living mob first.")
        SetStatus("Select a living mob first.")
        return false
    end
    return true
end

local function ScheduleBagRefresh(names)
    local guids = {}
    for _, n in ipairs(names or {}) do
        local g = AltGuid(n)
        if g then table.insert(guids, g) end
    end
    if #guids == 0 then return end
    delayedRefreshGuids = guids
    delayedRefreshAt = GetTime() + 1.8
end

local function Order(verb, choice, radius)
    if (verb == "pull" or verb == "farm") and not HaveCreatureTarget() then return end
    local targets = OrderTargets(choice)
    if #targets == 0 then return end
    if verb == "inventory" and choice == ALL_GROUPED then
        Queue({ gm = ".botcmd inventory all" })
        Print(table.concat(targets, ", ") .. ": clean bags.")
        SetStatus("Cleaning bags for grouped alts...")
        ScheduleBagRefresh(targets)
        return
    end
    local grouped = GroupedNames()
    local sent = {}
    for _, n in ipairs(targets) do
        local low = AltGuid(n)
        if verb == "pull" then
            if grouped[n] then
                Queue({ addon = "PULL:" .. low }); table.insert(sent, n)
            else
                Print(n .. " is not in your group, so it can't take a Pull order.")
            end
        elseif verb == "farm" then
            local r = ClampRadius(radius)
            AltBotsDB.farmRadius = r
            Queue({ gm = ".botcmd farm " .. low .. " " .. r }); table.insert(sent, n)
        else
            Queue({ gm = ".botcmd " .. verb .. " " .. low }); table.insert(sent, n)
        end
    end
    if #sent > 0 then
        local labels = { follow = "follow", stay = "stay", attack = "attack nearest", stopattack = "stop attacking",
            pull = "pull your target", farmstop = "stop farming", inventory = "clean bags" }
        local what = labels[verb] or verb
        if verb == "farm" then what = "farm your target (" .. ClampRadius(radius) .. " yd)" end
        Print(table.concat(sent, ", ") .. ": " .. what .. ".")
        SetStatus(table.concat(sent, ", ") .. ": " .. what .. ".")
        if verb == "inventory" then ScheduleBagRefresh(sent) end
    end
end

local function Spawn(name)
    local low = AltGuid(name); if not low then return end
    Queue({ gm = ".botcmd spawnbot " .. low }); spawned[name] = true
end
local function Despawn(name)
    local low = AltGuid(name); if not low then return end
    Queue({ gm = ".botcmd despawn " .. low }); spawned[name] = nil
end

local win, popup, minimapBtn
local ui = {}

SetStatus = function(text)
    if ui.status then ui.status:SetText(text or "") end
end

local function Tip(owner, title, body)
    GameTooltip:SetOwner(owner, "ANCHOR_RIGHT")
    GameTooltip:SetText(title or "", 1, 0.82, 0)
    if body and body ~= "" then
        GameTooltip:AddLine(body, 1, 1, 1, true)
    end
    GameTooltip:Show()
end

local function ShowItemTip(owner, entry, extra)
    if not entry or entry == 0 then return end
    GameTooltip:SetOwner(owner, "ANCHOR_RIGHT")
    local _, link = GetItemInfo(entry)
    if link then
        GameTooltip:SetHyperlink(link)
    else
        GameTooltip:SetHyperlink("item:" .. entry)
    end
    local tooHigh, need, have = ItemTooHighLevel(entry)
    if tooHigh then
        GameTooltip:AddLine("Too high level for " .. (selectedPartyName or "this bot") .. " (requires " .. need .. ", they are " .. have .. ").", 1, 0.25, 0.2, true)
    end
    if extra then GameTooltip:AddLine(extra, 0.4, 0.8, 1, true) end
    GameTooltip:Show()
end

local function ClassColorText(unit, name)
    local _, classFile = UnitClass(unit or "")
    local c = classFile and CLASS_COLORS[classFile]
    if c then
        return string.format("|cff%02x%02x%02x%s|r",
            math.floor(c.r * 255 + 0.5), math.floor(c.g * 255 + 0.5), math.floor(c.b * 255 + 0.5), name or "?")
    end
    return name or "?"
end

local function HidePopup()
    if popup then popup:Hide() end
end

local function ShowPopup(anchor, entries)
    if not popup then
        popup = CreateFrame("Frame", "AltBotsPopup", UIParent)
        popup:SetBackdrop({
            bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
            edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
            tile = true, tileSize = 16, edgeSize = 12,
            insets = { left = 3, right = 3, top = 3, bottom = 3 },
        })
        popup:SetBackdropColor(0.05, 0.05, 0.08, 0.97)
        popup:SetBackdropBorderColor(0.6, 0.55, 0.35, 1)
        popup:SetFrameStrata("DIALOG")
        popup.buttons = {}
    end
    for _, b in ipairs(popup.buttons) do b:Hide() end
    local y = -6
    for i, entry in ipairs(entries) do
        local b = popup.buttons[i]
        if not b then
            b = CreateFrame("Button", nil, popup)
            b:SetHeight(18)
            b.text = b:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
            b.text:SetPoint("LEFT", 6, 0)
            b.text:SetJustifyH("LEFT")
            b:SetHighlightTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight")
            popup.buttons[i] = b
        end
        b:SetWidth(180)
        b:ClearAllPoints()
        b:SetPoint("TOPLEFT", 4, y)
        b.text:SetWidth(170)
        b.text:SetText(entry.text)
        b:SetScript("OnClick", function()
            HidePopup()
            if entry.func then entry.func() end
        end)
        if entry.disabled then
            b:Disable(); b.text:SetTextColor(0.5, 0.5, 0.5)
        else
            b:Enable(); b.text:SetTextColor(1, 1, 1)
        end
        b:Show()
        y = y - 18
    end
    popup:SetWidth(188)
    popup:SetHeight((-y) + 8)
    popup:ClearAllPoints()
    popup:SetPoint("TOPLEFT", anchor, "BOTTOMLEFT", 0, -2)
    popup:Show()
end

local function MakeTextBtn(parent, w, h, text)
    local b = CreateFrame("Button", nil, parent, "UIPanelButtonTemplate")
    b:SetWidth(w); b:SetHeight(h or 22); b:SetText(text or "")
    b:SetScript("OnLeave", function() GameTooltip:Hide() end)
    return b
end

local function HookTip(btn, title, body)
    btn:SetScript("OnEnter", function(self) Tip(self, title, body) end)
    btn:SetScript("OnLeave", function() GameTooltip:Hide() end)
end

local function MakeIconBtn(parent, size, texture, title, body, onClick)
    local b = CreateFrame("Button", nil, parent)
    b:SetWidth(size); b:SetHeight(size)
    b:SetBackdrop({
        bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true, tileSize = 8, edgeSize = 10,
        insets = { left = 2, right = 2, top = 2, bottom = 2 },
    })
    b:SetBackdropColor(0.06, 0.06, 0.08, 0.95)
    b:SetBackdropBorderColor(0.45, 0.42, 0.30, 1)
    local icon = b:CreateTexture(nil, "ARTWORK")
    icon:SetPoint("TOPLEFT", 3, -3)
    icon:SetPoint("BOTTOMRIGHT", -3, 3)
    icon:SetTexture(texture)
    icon:SetTexCoord(0.08, 0.92, 0.08, 0.92)
    b.icon = icon
    local hi = b:CreateTexture(nil, "HIGHLIGHT")
    hi:SetPoint("TOPLEFT", 3, -3)
    hi:SetPoint("BOTTOMRIGHT", -3, 3)
    hi:SetTexture("Interface\\Buttons\\ButtonHilight-Square")
    hi:SetBlendMode("ADD")
    b:SetScript("OnEnter", function(self)
        self:SetBackdropBorderColor(1, 0.85, 0.3, 1)
        Tip(self, title, body)
    end)
    b:SetScript("OnLeave", function(self)
        self:SetBackdropBorderColor(0.45, 0.42, 0.30, 1)
        GameTooltip:Hide()
    end)
    b:RegisterForClicks("LeftButtonUp", "RightButtonUp")
    b:SetScript("OnClick", onClick)
    return b
end

local function MakeItemButton(parent, size)
    local b = CreateFrame("Button", nil, parent)
    b:SetWidth(size); b:SetHeight(size)
    b:SetBackdrop({
        bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true, tileSize = 8, edgeSize = 8,
        insets = { left = 1, right = 1, top = 1, bottom = 1 },
    })
    b:SetBackdropColor(0.07, 0.07, 0.09, 0.9)
    b:SetBackdropBorderColor(0.3, 0.3, 0.35, 1)
    b.icon = b:CreateTexture(nil, "ARTWORK")
    b.icon:SetPoint("TOPLEFT", 2, -2)
    b.icon:SetPoint("BOTTOMRIGHT", -2, 2)
    b.icon:SetTexCoord(0.08, 0.92, 0.08, 0.92)
    b.count = b:CreateFontString(nil, "OVERLAY", "NumberFontNormal")
    b.count:SetPoint("BOTTOMRIGHT", -2, 2)
    b.lock = b:CreateTexture(nil, "OVERLAY")
    b.lock:SetWidth(10); b.lock:SetHeight(10)
    b.lock:SetPoint("TOPRIGHT", 1, 1)
    b.lock:SetTexture("Interface\\LFGFrame\\UI-LFG-ICON-LOCK")
    b.lock:Hide()
    b:SetScript("OnLeave", function() GameTooltip:Hide() end)
    b:RegisterForClicks("LeftButtonUp", "RightButtonUp")
    return b
end

local function SetItemIcon(btn, entry, emptyTex)
    if not entry or entry == 0 then
        btn.icon:SetTexture(emptyTex or "Interface\\Buttons\\UI-Slot-Background")
        btn.icon:SetVertexColor(0.45, 0.45, 0.5)
        btn:SetBackdropBorderColor(0.3, 0.3, 0.35, 1)
        return
    end
    local texture = GetItemIcon and GetItemIcon(entry)
    if not texture then
        local _, _, _, _, _, _, _, _, _, icon = GetItemInfo(entry)
        texture = icon
    end
    btn.icon:SetTexture(texture or "Interface\\Icons\\INV_Misc_QuestionMark")
    local tooHigh = ItemTooHighLevel(entry)
    if tooHigh then
        btn.icon:SetVertexColor(1, 0.28, 0.22)
        btn:SetBackdropBorderColor(0.85, 0.2, 0.15, 1)
    else
        btn.icon:SetVertexColor(1, 1, 1)
        btn:SetBackdropBorderColor(0.3, 0.3, 0.35, 1)
    end
end

local function SetTabState(tab, selected)
    if selected then
        tab:Disable()
        tab:SetBackdropColor(0.18, 0.16, 0.10, 1)
        tab:SetBackdropBorderColor(0.85, 0.72, 0.25, 1)
        if tab.label then tab.label:SetTextColor(1, 0.82, 0) end
        if tab.underline then tab.underline:Show() end
    else
        tab:Enable()
        tab:SetBackdropColor(0.06, 0.06, 0.08, 0.95)
        tab:SetBackdropBorderColor(0.35, 0.35, 0.40, 1)
        if tab.label then tab.label:SetTextColor(0.85, 0.85, 0.85) end
        if tab.underline then tab.underline:Hide() end
    end
end

local function MakeTab(parent, label)
    local b = CreateFrame("Button", nil, parent)
    b:SetWidth(96); b:SetHeight(24)
    b:SetBackdrop({
        bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true, tileSize = 8, edgeSize = 10,
        insets = { left = 2, right = 2, top = 2, bottom = 2 },
    })
    b.label = b:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    b.label:SetPoint("CENTER", 0, 0)
    b.label:SetText(label)
    b.underline = b:CreateTexture(nil, "OVERLAY")
    b.underline:SetHeight(2)
    b.underline:SetPoint("BOTTOMLEFT", 8, 3)
    b.underline:SetPoint("BOTTOMRIGHT", -8, 3)
    b.underline:SetTexture(0.95, 0.78, 0.15, 1)
    b.underline:Hide()
    SetTabState(b, false)
    return b
end

local function SectionHeader(parent, text)
    local fs = parent:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    fs:SetText(text)
    local rule = parent:CreateTexture(nil, "ARTWORK")
    rule:SetHeight(1)
    rule:SetTexture(0.45, 0.40, 0.28, 0.8)
    return fs, rule
end

local function CreateMinimapButton()
    if minimapBtn then return end
    local b = CreateFrame("Button", "AltBotsMinimapButton", Minimap)
    b:SetWidth(32); b:SetHeight(32)
    b:SetFrameStrata("MEDIUM")
    b:SetFrameLevel(8)
    b:SetHighlightTexture("Interface\\Minimap\\UI-Minimap-ZoomButton-Highlight")
    local overlay = b:CreateTexture(nil, "OVERLAY")
    overlay:SetWidth(54); overlay:SetHeight(54)
    overlay:SetPoint("CENTER")
    overlay:SetTexture("Interface\\Minimap\\MiniMap-TrackingBorder")
    local icon = b:CreateTexture(nil, "BACKGROUND")
    icon:SetWidth(20); icon:SetHeight(20)
    icon:SetPoint("CENTER", 0, 1)
    icon:SetTexture("Interface\\Icons\\INV_Misc_GroupNeedMore")
    icon:SetTexCoord(0.08, 0.92, 0.08, 0.92)
    local function Place()
        AltBotsDB.minimapAngle = AltBotsDB.minimapAngle or 220
        local a = math.rad(AltBotsDB.minimapAngle)
        local r = (Minimap:GetWidth() / 2) + 8
        b:ClearAllPoints()
        b:SetPoint("CENTER", Minimap, "CENTER", math.cos(a) * r, math.sin(a) * r)
    end
    b:RegisterForDrag("LeftButton")
    b:SetScript("OnDragStart", function(self)
        self:SetScript("OnUpdate", function(me)
            local mx, my = Minimap:GetCenter()
            local x, y = GetCursorPosition()
            local scale = UIParent:GetEffectiveScale()
            x, y = x / scale, y / scale
            AltBotsDB.minimapAngle = math.deg(math.atan2(y - my, x - mx))
            Place()
        end)
    end)
    b:SetScript("OnDragStop", function(self) self:SetScript("OnUpdate", nil) end)
    b:SetScript("OnClick", function()
        EnsureWindow()
        if win:IsShown() then win:Hide() else win:Show(); ShowTab(ui.activeTab or "alts") end
    end)
    b:SetScript("OnEnter", function(self)
        Tip(self, "AltBots", "Click to open. Drag to move. Type /alt to toggle, or /alt minimap to hide this button.")
    end)
    b:SetScript("OnLeave", function() GameTooltip:Hide() end)
    b.Place = Place
    minimapBtn = b
    if AltBotsDB.minimapHidden then b:Hide() else Place(); b:Show() end
end

local function GetMinimapShown()
    return not AltBotsDB.minimapHidden
end

local function SetMinimapShown(shown)
    AltBotsDB.minimapHidden = not shown
    if shown then
        CreateMinimapButton()
        if minimapBtn then minimapBtn.Place(); minimapBtn:Show() end
    elseif minimapBtn then
        minimapBtn:Hide()
    end
end

-- Widget pools live under the window and are never reparented to nil.
local function InitPools()
    ui.pools = ui.pools or {
        altRows = {}, cmdRows = {}, partyRows = {},
        gearSlots = {}, bagSlots = {}, profBtns = {},
        roleBtns = {}, specBtns = {},
        prefArm = {}, prefWep = {},
        questListRows = {}, questRows = {}, questObjRows = {},
    }
end

local function HidePool(list)
    for _, w in ipairs(list or {}) do w:Hide() end
end

local function TakeRow(pool, parent, maker)
    for _, w in ipairs(pool) do
        if not w:IsShown() then
            w:SetParent(parent)
            w:Show()
            return w
        end
    end
    local w = maker(parent)
    table.insert(pool, w)
    w:Show()
    return w
end

local function StyleRowButton(b, w, h)
    b:SetWidth(w); b:SetHeight(h)
    b:SetBackdrop({
        bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true, tileSize = 8, edgeSize = 8,
        insets = { left = 2, right = 2, top = 2, bottom = 2 },
    })
    b:SetBackdropColor(0.07, 0.07, 0.09, 0.9)
    b:SetBackdropBorderColor(0.28, 0.28, 0.32, 1)
end

local function MakeAltRow(parent)
    local row = CreateFrame("Frame", nil, parent)
    StyleRowButton(row, 620, 28)
    row.dot = row:CreateTexture(nil, "OVERLAY")
    row.dot:SetWidth(8); row.dot:SetHeight(8)
    row.dot:SetPoint("LEFT", 8, 0)
    row.dot:SetTexture("Interface\\COMMON\\Indicator-Green")
    row.name = row:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
    row.name:SetPoint("LEFT", 20, 0)
    row.name:SetJustifyH("LEFT")
    row.name:SetWidth(170)
    row.status = row:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    row.status:SetPoint("LEFT", 192, 0)
    row.status:SetWidth(120)
    row.status:SetJustifyH("LEFT")
    row.remove = MakeTextBtn(row, 22, 20, "x")
    row.remove:SetPoint("RIGHT", -6, 0)
    row.invite = MakeTextBtn(row, 64, 20, "Invite")
    row.invite:SetPoint("RIGHT", -32, 0)
    row.quest = MakeTextBtn(row, 64, 20, "Quest")
    row.quest:SetPoint("RIGHT", -100, 0)
    row.spawn = MakeTextBtn(row, 64, 20, "Spawn")
    row.spawn:SetPoint("RIGHT", -168, 0)
    return row
end

local function MakeCmdRow(parent)
    local row = CreateFrame("Frame", nil, parent)
    StyleRowButton(row, 500, 26)
    row.dot = row:CreateTexture(nil, "OVERLAY")
    row.dot:SetWidth(8); row.dot:SetHeight(8)
    row.dot:SetPoint("LEFT", 8, 0)
    row.dot:SetTexture("Interface\\COMMON\\Indicator-Green")
    row.name = row:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
    row.name:SetPoint("LEFT", 22, 0)
    row.name:SetJustifyH("LEFT")
    row.name:SetWidth(180)
    row.pick = CreateFrame("Button", nil, row)
    row.pick:SetAllPoints(row)
    row.pick:SetHighlightTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight")
    return row
end

local function MakePartyRow(parent)
    local row = CreateFrame("Button", nil, parent)
    StyleRowButton(row, 168, 28)
    row.dot = row:CreateTexture(nil, "OVERLAY")
    row.dot:SetWidth(8); row.dot:SetHeight(8)
    row.dot:SetPoint("LEFT", 6, 0)
    row.dot:SetTexture("Interface\\COMMON\\Indicator-Green")
    row.name = row:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
    row.name:SetPoint("LEFT", 18, 0)
    row.name:SetJustifyH("LEFT")
    row.name:SetWidth(145)
    row:SetHighlightTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight")
    return row
end

EnsureWindow = function()
    if win then return end
    InitPools()
    win = CreateFrame("Frame", "AltBotsFrame", UIParent)
    win:SetWidth(680); win:SetHeight(560)
    win:SetPoint("CENTER")
    win:SetBackdrop({
        bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true, tileSize = 16, edgeSize = 16,
        insets = { left = 4, right = 4, top = 4, bottom = 4 },
    })
    win:SetBackdropColor(0.03, 0.03, 0.05, 0.94)
    win:SetBackdropBorderColor(0.65, 0.55, 0.30, 1)
    win:SetMovable(true)
    win:EnableMouse(true)
    win:SetToplevel(true)
    win:SetFrameStrata("HIGH")
    win:SetClampedToScreen(true)
    win:Hide()
    tinsert(UISpecialFrames, "AltBotsFrame")

    local titleBar = CreateFrame("Frame", nil, win)
    titleBar:SetPoint("TOPLEFT", 6, -6)
    titleBar:SetPoint("TOPRIGHT", -28, -6)
    titleBar:SetHeight(26)
    titleBar:EnableMouse(true)
    titleBar:RegisterForDrag("LeftButton")
    titleBar:SetScript("OnDragStart", function() win:StartMoving() end)
    titleBar:SetScript("OnDragStop", function() win:StopMovingOrSizing() end)

    local accent = titleBar:CreateTexture(nil, "BACKGROUND")
    accent:SetAllPoints()
    accent:SetTexture(0.10, 0.09, 0.06, 0.85)

    local title = titleBar:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
    title:SetPoint("LEFT", 10, 0)
    title:SetText("AltBots")
    title:SetTextColor(1, 0.86, 0.35)

    local close = CreateFrame("Button", nil, win, "UIPanelCloseButton")
    close:SetPoint("TOPRIGHT", 2, 2)
    close:SetScript("OnClick", function() HidePopup(); win:Hide() end)
    HookTip(close, "Close", "You can also press Esc or type /alt.")

    ui.tabs = {}
    local tabX = 12
    for i, info in ipairs({
        { id = "alts", label = "Alts" },
        { id = "commands", label = "Commands" },
        { id = "party", label = "Party" },
        { id = "quests", label = "Quests" },
    }) do
        local t = MakeTab(win, info.label)
        t:SetPoint("TOPLEFT", tabX, -36)
        t:SetScript("OnClick", function() ShowTab(info.id) end)
        HookTip(t, info.label, i == 1 and "Spawn, invite or despawn your saved alts."
            or (i == 2 and "Give follow, stay, farm and inventory orders."
            or (i == 3 and "Inspect, talents, gear and bags of grouped bots."
            or "See each grouped bot's quest log and objective progress.")))
        ui.tabs[info.id] = t
        tabX = tabX + 102
    end

    local body = CreateFrame("Frame", nil, win)
    body:SetPoint("TOPLEFT", 8, -64)
    body:SetPoint("BOTTOMRIGHT", -8, 28)
    ui.body = body

    ui.status = win:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    ui.status:SetPoint("BOTTOMLEFT", 12, 10)
    ui.status:SetPoint("BOTTOMRIGHT", -12, 10)
    ui.status:SetJustifyH("LEFT")
    ui.status:SetText("Ready.")

    -- Alts pane
    local alts = CreateFrame("Frame", nil, body)
    alts:SetAllPoints()
    ui.altsPane = alts
    local ah, ar = SectionHeader(alts, "Your characters")
    ah:SetPoint("TOPLEFT", 6, -2)
    ar:SetPoint("LEFT", ah, "RIGHT", 8, 0)
    ar:SetPoint("RIGHT", -8, 0)
    local hint = alts:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    hint:SetPoint("TOPLEFT", 6, -20)
    hint:SetPoint("TOPRIGHT", -6, -20)
    hint:SetJustifyH("LEFT")
    hint:SetText("")
    ui.altsHint = hint
    ui.altsEmpty = alts:CreateFontString(nil, "OVERLAY", "GameFontDisable")
    ui.altsEmpty:SetPoint("CENTER", 0, 10)
    ui.altsEmpty:SetText("No other characters saved yet.\nPress Sync, or log into an alt with this addon enabled.")
    ui.spawnAllBtn = MakeTextBtn(alts, 150, 20, "Spawn + invite all")
    ui.spawnAllBtn:SetPoint("TOPLEFT", 6, -40)
    HookTip(ui.spawnAllBtn, "Spawn + invite all", "Spawn every saved alt and invite them to your group.")
    ui.spawnAllBtn:SetScript("OnClick", function()
        local skipped = 0
        for _, n in ipairs(SortedAltNames()) do
            local info = Alts()[n]
            if info and info.stale then
                skipped = skipped + 1
            else
                SpawnAndInvite(n)
            end
        end
        SetStatus("Spawning and inviting all saved alts..." ..
            (skipped > 0 and (" Skipped " .. skipped .. " unverified.") or ""))
        RefreshAlts()
    end)
    ui.despawnAllBtn = MakeTextBtn(alts, 110, 20, "Despawn all")
    ui.despawnAllBtn:SetPoint("LEFT", ui.spawnAllBtn, "RIGHT", 6, 0)
    HookTip(ui.despawnAllBtn, "Despawn all", "Send every saved alt away.")
    ui.despawnAllBtn:SetScript("OnClick", function()
        local names = SortedAltNames()
        local grouped = GroupedNames()
        local out = {}
        for _, n in ipairs(names) do
            if spawned[n] or grouped[n] then table.insert(out, n) end
        end
        if #out == 0 then out = names end
        for _, n in ipairs(out) do Despawn(n) end
        SetStatus("Despawning " .. #out .. " alt" .. (#out == 1 and "" or "s") .. ".")
        RefreshAlts()
    end)
    ui.syncBtn = MakeTextBtn(alts, 70, 20, "Sync")
    ui.syncBtn:SetPoint("TOPRIGHT", -6, -40)
    HookTip(ui.syncBtn, "Sync with the server",
        "Ask the server which characters your account has. Characters that no longer exist are removed "
        .. "and new ones are added, so a character wipe cleans itself up.")
    ui.syncBtn:SetScript("OnClick", function()
        RequestAltSync(false)
        SetStatus("Asking the server for your character list...")
        RefreshAlts()
    end)
    ui.pruneBtn = MakeTextBtn(alts, 130, 20, "Remove unverified")
    ui.pruneBtn:SetPoint("RIGHT", ui.syncBtn, "LEFT", -6, 0)
    HookTip(ui.pruneBtn, "Remove unverified",
        "Remove every saved character that could not be checked since the server looked reset.")
    ui.pruneBtn:SetScript("OnClick", function()
        local n = 0
        for _, name in ipairs(SortedAltNames()) do
            local info = Alts()[name]
            if info and info.stale and RemoveAlt(name) then n = n + 1 end
        end
        SetStatus("Removed " .. n .. " unverified character" .. (n == 1 and "" or "s") .. ".")
        RefreshAlts()
    end)
    local asf = CreateFrame("ScrollFrame", "AltBotsAltsScroll", alts, "UIPanelScrollFrameTemplate")
    asf:SetPoint("TOPLEFT", 2, -66)
    asf:SetPoint("BOTTOMRIGHT", -26, 4)
    local ac = CreateFrame("Frame", nil, asf)
    ac:SetWidth(620); ac:SetHeight(1)
    asf:SetScrollChild(ac)
    ui.altsChild = ac

    -- Commands pane
    local cmd = CreateFrame("Frame", nil, body)
    cmd:SetAllPoints()
    ui.cmdPane = cmd
    local ch, cr = SectionHeader(cmd, "Give orders")
    ch:SetPoint("TOPLEFT", 6, -2)
    cr:SetPoint("LEFT", ch, "RIGHT", 8, 0)
    cr:SetPoint("RIGHT", -8, 0)

    local whoLbl = cmd:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    whoLbl:SetPoint("TOPLEFT", 8, -24)
    whoLbl:SetText("Who")
    ui.whoBtn = MakeTextBtn(cmd, 220, 22, "All grouped alts")
    ui.whoBtn:SetPoint("LEFT", whoLbl, "RIGHT", 8, 0)
    HookTip(ui.whoBtn, "Who receives the order", "Choose one saved alt, or every known alt currently in your group.")
    ui.whoBtn:SetScript("OnClick", function(self)
        local entries = { { text = "All grouped alts", func = function()
            ui.orderChoice = ALL_GROUPED
            self:SetText("All grouped alts")
        end } }
        for _, n in ipairs(SortedAltNames()) do
            local name = n
            table.insert(entries, { text = name, func = function()
                ui.orderChoice = name
                self:SetText(name)
            end })
        end
        ShowPopup(self, entries)
    end)
    ui.orderChoice = ALL_GROUPED

    local radiusLbl = cmd:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    radiusLbl:SetPoint("LEFT", ui.whoBtn, "RIGHT", 16, 0)
    radiusLbl:SetText("Farm radius")
    ui.radius = CreateFrame("EditBox", "AltBotsRadius", cmd, "InputBoxTemplate")
    ui.radius:SetWidth(44); ui.radius:SetHeight(20)
    ui.radius:SetPoint("LEFT", radiusLbl, "RIGHT", 8, 0)
    ui.radius:SetAutoFocus(false)
    ui.radius:SetNumeric(true)
    ui.radius:SetMaxLetters(3)
    ui.radius:SetText("40")
    ui.radius:SetScript("OnEnterPressed", function(self) self:ClearFocus() end)
    ui.radius:SetScript("OnEscapePressed", function(self) self:ClearFocus() end)
    HookTip(ui.radius, "Farm radius", "How far the bot may wander while farming your current target, in yards (10-100).")

    local function Choice() return ui.orderChoice or ALL_GROUPED end
    local function Radius() return ClampRadius(ui.radius:GetText()) end
    local icons = {
        { "Interface\\Icons\\Ability_Hunter_Pet_Follow", "Follow", "Tell the selected bot(s) to follow you.", function() Order("follow", Choice()) end },
        { "Interface\\Icons\\Spell_Nature_TimeStop", "Stay", "Hold position.", function() Order("stay", Choice()) end },
        { "Interface\\Icons\\Ability_Warrior_Charge", "Attack", "Attack the nearest enemy.", function() Order("attack", Choice()) end },
        { "Interface\\Icons\\Ability_Hunter_SniperShot", "Pull", "Pull your current target. Requires a living mob and the bot in your group.", function() Order("pull", Choice()) end },
        { "Interface\\Icons\\INV_Misc_Bone_HumanSkull_01", "Farm", "Farm your current target within the radius set above.", function() Order("farm", Choice(), Radius()) end },
        { "Interface\\Icons\\Spell_Shadow_SacrificialShield", "Stop farm", "Leave farm mode and return to follow.", function() Order("farmstop", Choice()) end },
        { "Interface\\Icons\\Ability_Warrior_Disarm", "Stop attack", "Stop attacking.", function() Order("stopattack", Choice()) end },
        { "Interface\\Icons\\INV_Misc_Bag_10", "Clean bags", "Sell greys and outgrown whites and greens. Works immediately, or when the bot is near a vendor.", function() Order("inventory", Choice()) end },
    }
    ui.cmdIcons = {}
    for i, info in ipairs(icons) do
        local b = MakeIconBtn(cmd, 36, info[1], info[2], info[3], info[4])
        b:SetPoint("TOPLEFT", 8 + (i - 1) * 42, -52)
        ui.cmdIcons[i] = b
    end

    local listH, listR = SectionHeader(cmd, "Grouped alts")
    listH:SetPoint("TOPLEFT", 6, -98)
    listR:SetPoint("LEFT", listH, "RIGHT", 8, 0)
    listR:SetPoint("RIGHT", -8, 0)
    ui.cmdEmpty = cmd:CreateFontString(nil, "OVERLAY", "GameFontDisable")
    ui.cmdEmpty:SetPoint("CENTER", 0, -10)
    ui.cmdEmpty:SetText("None of your saved alts are in the group yet.\nSpawn and invite them from the Alts tab.")
    local csf = CreateFrame("ScrollFrame", "AltBotsCmdScroll", cmd, "UIPanelScrollFrameTemplate")
    csf:SetPoint("TOPLEFT", 2, -116)
    csf:SetPoint("BOTTOMRIGHT", -26, 4)
    local cc = CreateFrame("Frame", nil, csf)
    cc:SetWidth(620); cc:SetHeight(1)
    csf:SetScrollChild(cc)
    ui.cmdChild = cc

    -- Party pane: member list on the left, details on the right.
    local party = CreateFrame("Frame", nil, body)
    party:SetAllPoints()
    ui.partyPane = party

    local left = CreateFrame("Frame", nil, party)
    left:SetPoint("TOPLEFT", 0, 0)
    left:SetPoint("BOTTOMLEFT", 0, 0)
    left:SetWidth(176)
    left:SetBackdrop({
        bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true, tileSize = 8, edgeSize = 10,
        insets = { left = 2, right = 2, top = 2, bottom = 2 },
    })
    left:SetBackdropColor(0.04, 0.04, 0.06, 0.9)
    left:SetBackdropBorderColor(0.35, 0.32, 0.22, 1)
    local lh, lr = SectionHeader(left, "Party")
    lh:SetPoint("TOPLEFT", 8, -6)
    lr:SetPoint("LEFT", lh, "RIGHT", 6, 0)
    lr:SetPoint("RIGHT", -8, 0)
    ui.partyEmpty = left:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    ui.partyEmpty:SetPoint("TOPLEFT", 8, -28)
    ui.partyEmpty:SetPoint("TOPRIGHT", -8, -28)
    ui.partyEmpty:SetJustifyH("LEFT")
    ui.partyEmpty:SetText("Invite a bot to your group to inspect it here.")
    local psf = CreateFrame("ScrollFrame", "AltBotsPartyListScroll", left, "UIPanelScrollFrameTemplate")
    psf:SetPoint("TOPLEFT", 4, -24)
    psf:SetPoint("BOTTOMRIGHT", -22, 4)
    local pc = CreateFrame("Frame", nil, psf)
    pc:SetWidth(148); pc:SetHeight(1)
    psf:SetScrollChild(pc)
    ui.partyListChild = pc

    local right = CreateFrame("Frame", nil, party)
    right:SetPoint("TOPLEFT", left, "TOPRIGHT", 6, 0)
    right:SetPoint("BOTTOMRIGHT", 0, 0)
    ui.partyRight = right

    ui.partyPickHint = right:CreateFontString(nil, "OVERLAY", "GameFontDisable")
    ui.partyPickHint:SetPoint("CENTER", 0, 20)
    ui.partyPickHint:SetText("Select a party member on the left.")

    ui.detail = CreateFrame("Frame", nil, right)
    ui.detail:SetAllPoints()
    ui.detail:Hide()

    ui.detailName = ui.detail:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
    ui.detailName:SetPoint("TOPLEFT", 4, -2)

    ui.inspectBtn = MakeTextBtn(ui.detail, 70, 22, "Inspect")
    ui.inspectBtn:SetPoint("TOPRIGHT", -4, -2)
    HookTip(ui.inspectBtn, "Inspect", "Open the game's inspect window. You must be within 28 yards.")
    ui.inspectBtn:SetScript("OnClick", function()
        if not selectedPartyUnit or not UnitExists(selectedPartyUnit) then
            SetStatus("Select a party member first.")
            return
        end
        if not CheckInteractDistance(selectedPartyUnit, 1) then
            Print((selectedPartyName or "That bot") .. " is too far away to inspect.")
            SetStatus((selectedPartyName or "That bot") .. " is too far away to inspect.")
            return
        end
        InspectUnit(selectedPartyUnit)
    end)

    local rh, rr = SectionHeader(ui.detail, "Role, spec and gear prefs")
    ui.prefHeader, ui.prefRule = rh, rr
    ui.roleLbl = ui.detail:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    ui.roleLbl:SetText("Role")
    ui.roleRow = CreateFrame("Frame", nil, ui.detail)
    ui.roleRow:SetWidth(400); ui.roleRow:SetHeight(22)

    ui.specLbl = ui.detail:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    ui.specLbl:SetText("Spec")
    ui.specRow = CreateFrame("Frame", nil, ui.detail)
    ui.specRow:SetWidth(400); ui.specRow:SetHeight(22)
    ui.specHint = ui.detail:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    ui.specHint:SetText("This class has no addon specs to switch.")

    ui.armorLbl = ui.detail:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    ui.armorLbl:SetText("Armor")
    ui.armorRow = CreateFrame("Frame", nil, ui.detail)
    ui.armorRow:SetWidth(400); ui.armorRow:SetHeight(22)

    ui.weaponLbl = ui.detail:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    ui.weaponLbl:SetText("Weapons")
    ui.weaponRow = CreateFrame("Frame", nil, ui.detail)
    ui.weaponRow:SetWidth(400); ui.weaponRow:SetHeight(22)

    local eh, er = SectionHeader(ui.detail, "Equipped")
    ui.equipHeader, ui.equipRule = eh, er
    ui.unlockBtn = MakeTextBtn(ui.detail, 100, 20, "Auto-upgrade")
    HookTip(ui.unlockBtn, "Auto-upgrade", "Clear locked slots so the bot can choose its own upgrades again.")
    ui.unlockBtn:SetScript("OnClick", function()
        if selectedPartyGuid then
            SendBot("UNLOCK", selectedPartyGuid)
            SetStatus("Unlocking gear on " .. (selectedPartyName or "bot") .. "...")
        end
    end)

    ui.doll = CreateFrame("Frame", nil, ui.detail)
    ui.doll:SetWidth(400); ui.doll:SetHeight(72)

    local ph, pr = SectionHeader(ui.detail, "Professions")
    ui.profHeader, ui.profRule = ph, pr
    ui.profRow = CreateFrame("Frame", nil, ui.detail)
    ui.profRow:SetWidth(400); ui.profRow:SetHeight(48)
    ui.profHint = ui.detail:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    ui.profHint:SetText("Profession data has not arrived yet.")

    local bh, br = SectionHeader(ui.detail, "Bags")
    ui.bagHeader, ui.bagRule = bh, br

    ui.refreshBtn = MakeTextBtn(ui.detail, 86, 20, "Refresh")
    HookTip(ui.refreshBtn, "Refresh bags", "Ask this bot for a fresh bag and equipment list.")
    ui.refreshBtn:SetScript("OnClick", function()
        if selectedPartyGuid then
            RequestGearAndBags(selectedPartyGuid, "Refreshing " .. (selectedPartyName or "bot") .. "...")
        else
            SetStatus("Select a party member first.")
        end
    end)

    ui.refreshAllBtn = MakeTextBtn(ui.detail, 86, 20, "Refresh all")
    HookTip(ui.refreshAllBtn, "Refresh all", "Ask every grouped bot for a fresh bag and equipment list.")
    ui.refreshAllBtn:SetScript("OnClick", RequestAllPartyBags)

    local bsf = CreateFrame("ScrollFrame", "AltBotsBagsScroll", ui.detail, "UIPanelScrollFrameTemplate")
    ui.bagsScroll = bsf
    local bc = CreateFrame("Frame", nil, bsf)
    bc:SetWidth(400); bc:SetHeight(1)
    bsf:SetScrollChild(bc)
    ui.bagsChild = bc
    ui.bagsEmpty = ui.detail:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    ui.bagsEmpty:SetPoint("CENTER", bsf, "CENTER", 0, 0)
    ui.bagsEmpty:SetText("Bags are empty, or still loading.")

    local function BagCapacity(guid)
        local meta = bagMeta[guid]
        if not meta then return 0 end
        local total = meta.backpack or 0
        for _, size in ipairs(meta.bags or {}) do
            total = total + (size or 0)
        end
        return total
    end

    local function OccupiedBagCount(guid)
        local n = 0
        for _, it in pairs(bagItems[guid] or {}) do
            if it.entry and it.entry > 0 then n = n + 1 end
        end
        return n
    end

    UpdateBagHeader = function()
        if not ui.bagHeader then return end
        local guid = selectedPartyGuid
        if not guid then
            ui.bagHeader:SetText("Bags")
            return
        end
        if bagsRefreshing[guid] and not bagMeta[guid] then
            ui.bagHeader:SetText("Bags  |  Refreshing...")
            return
        end
        local total = BagCapacity(guid)
        local used = OccupiedBagCount(guid)
        if bagsComplete[guid] then used = bagsComplete[guid] end
        local parts = { "Bags" }
        if bagsRefreshing[guid] then
            table.insert(parts, "Refreshing...")
        elseif used > 0 or total > 0 then
            table.insert(parts, used .. " item" .. (used == 1 and "" or "s"))
        end
        if total > 0 then
            table.insert(parts, (total - used) .. "/" .. total .. " free")
        end
        ui.bagHeader:SetText(table.concat(parts, "  |  "))
    end

    LayoutPartyDetail = function()
        if not ui.detail or not ui.prefHeader then return end
        local paneW = ui.detail:GetWidth() or 0
        if paneW < 100 then paneW = 470 end
        local innerW = paneW - 66
        local y = -28
        local function placeHeader(fs, rule, rightFrame)
            fs:ClearAllPoints()
            fs:SetPoint("TOPLEFT", 4, y)
            if rule then
                rule:ClearAllPoints()
                rule:SetPoint("LEFT", fs, "RIGHT", 8, 0)
                if rightFrame then
                    rule:SetPoint("RIGHT", rightFrame, "LEFT", -8, 0)
                else
                    rule:SetPoint("RIGHT", -4, 0)
                end
            end
        end
        local function placePref(lbl, row, extra)
            lbl:ClearAllPoints()
            lbl:SetPoint("TOPLEFT", 4, y - 2)
            row:ClearAllPoints()
            row:SetPoint("TOPLEFT", 58, y)
            row:SetWidth(innerW)
            if extra then
                extra:ClearAllPoints()
                extra:SetPoint("LEFT", row, "LEFT", 0, 0)
            end
            y = y - (row:GetHeight() or 22) - 6
        end

        placeHeader(ui.prefHeader, ui.prefRule)
        y = y - 20
        placePref(ui.roleLbl, ui.roleRow)
        placePref(ui.specLbl, ui.specRow, ui.specHint)
        placePref(ui.armorLbl, ui.armorRow)
        placePref(ui.weaponLbl, ui.weaponRow)

        y = y - 2
        ui.unlockBtn:ClearAllPoints()
        ui.unlockBtn:SetPoint("TOPRIGHT", -4, y + 1)
        placeHeader(ui.equipHeader, ui.equipRule, ui.unlockBtn)
        y = y - 22
        ui.doll:ClearAllPoints()
        ui.doll:SetPoint("TOPLEFT", 8, y)
        ui.doll:SetWidth(paneW - 16)
        ui.doll:SetHeight(72)
        y = y - 80

        placeHeader(ui.profHeader, ui.profRule)
        y = y - 20
        ui.profRow:ClearAllPoints()
        ui.profRow:SetPoint("TOPLEFT", 4, y)
        ui.profRow:SetWidth(paneW - 8)
        ui.profHint:ClearAllPoints()
        ui.profHint:SetPoint("TOPLEFT", ui.profRow, "TOPLEFT", 2, -2)
        y = y - (ui.profRow:GetHeight() or 48) - 8

        ui.refreshAllBtn:ClearAllPoints()
        ui.refreshAllBtn:SetPoint("TOPRIGHT", -4, y + 1)
        ui.refreshBtn:ClearAllPoints()
        ui.refreshBtn:SetPoint("RIGHT", ui.refreshAllBtn, "LEFT", -4, 0)
        placeHeader(ui.bagHeader, ui.bagRule, ui.refreshBtn)
        y = y - 22
        ui.bagsScroll:ClearAllPoints()
        ui.bagsScroll:SetPoint("TOPLEFT", 4, y)
        ui.bagsScroll:SetPoint("BOTTOMRIGHT", -28, 4)
        local scrollW = paneW - 36
        if scrollW < 200 then scrollW = 200 end
        ui.bagsChild:SetWidth(scrollW)

        -- Grow the window so the bag grid always shows at least MIN_BAG_ROWS rows
        -- (two rows of professions used to squeeze it down to one), capped to the screen.
        local MIN_BAG_ROWS, BASE_H = 4, 560
        local minBagH = MIN_BAG_ROWS * 36 + 8
        local curH = win:GetHeight() or BASE_H
        local detailH = ui.detail:GetHeight() or 0
        if detailH > 0 then
            local detailAtBase = detailH - (curH - BASE_H)
            local bagAtBase = detailAtBase + y - 4
            local wantH = BASE_H + math.max(0, minBagH - bagAtBase)
            local maxH = (UIParent:GetHeight() or 768) - 40
            if wantH > maxH then wantH = maxH end
            if wantH < BASE_H then wantH = BASE_H end
            if math.abs(wantH - curH) >= 1 then win:SetHeight(wantH) end
        end
    end

    BuildQuestPane(body)
end


local function AgoText(stamp)
    local secs = time() - (stamp or 0)
    if secs < 60 then return "just now" end
    if secs < 3600 then return math.floor(secs / 60) .. " min ago" end
    if secs < 86400 then return math.floor(secs / 3600) .. " h ago" end
    return math.floor(secs / 86400) .. " days ago"
end

local function AltsHintText()
    local data = RealmData()
    if altSync.state == "waiting" then
        return "Checking your characters with the server..."
    end
    local stale = StaleCount()
    if stale > 0 then
        return "|cffff9933The server looks reset. " .. stale .. " saved character" .. (stale == 1 and "" or "s") ..
            " could not be checked yet: press Sync, or remove them.|r"
    end
    if data.serverList and data.syncedAt then
        return "Synced with your account's character list " .. AgoText(data.syncedAt) .. "."
    end
    return "Characters are saved when you log into them. Press Sync to load your account's list from the server."
end

local function AltName(name, info)
    local c = info and info.classFile and CLASS_COLORS[info.classFile]
    local text = name
    if c then
        text = string.format("|cff%02x%02x%02x%s|r",
            math.floor(c.r * 255 + 0.5), math.floor(c.g * 255 + 0.5), math.floor(c.b * 255 + 0.5), name)
    end
    if info and info.level and info.level > 0 then
        text = text .. "  |cff999999" .. info.level .. "|r"
    end
    return text
end

RefreshAlts = function()
    if not win or not ui.altsChild then return end
    HidePool(ui.pools.altRows)
    local names = SortedAltNames()
    local grouped = GroupedNames()
    if ui.spawnAllBtn then
        if #names == 0 then ui.spawnAllBtn:Disable(); ui.despawnAllBtn:Disable()
        else ui.spawnAllBtn:Enable(); ui.despawnAllBtn:Enable() end
    end
    if ui.altsHint then ui.altsHint:SetText(AltsHintText()) end
    if ui.syncBtn then
        if altSync.state == "waiting" then ui.syncBtn:Disable() else ui.syncBtn:Enable() end
    end
    if ui.pruneBtn then
        if StaleCount() > 0 then ui.pruneBtn:Show() else ui.pruneBtn:Hide() end
    end
    if #names == 0 then
        ui.altsEmpty:Show()
        ui.altsChild:SetHeight(1)
        return
    end
    ui.altsEmpty:Hide()
    local y = -2
    for _, name in ipairs(names) do
        local row = TakeRow(ui.pools.altRows, ui.altsChild, MakeAltRow)
        row:ClearAllPoints()
        row:SetPoint("TOPLEFT", 2, y)
        row:SetWidth(620)
        local info = Alts()[name]
        row.name:SetText(AltName(name, info))
        local inGroup = grouped[name]
        local isOut = spawned[name] or inGroup
        row.status:SetTextColor(0.5, 0.5, 0.5)
        if inGroup then
            row.status:SetText("In your group")
            row.dot:SetTexture("Interface\\COMMON\\Indicator-Green")
        elseif info and info.stale then
            row.status:SetText("Not verified")
            row.status:SetTextColor(1, 0.6, 0.2)
            row.dot:SetTexture("Interface\\COMMON\\Indicator-Red")
        elseif spawned[name] then
            row.status:SetText("Questing solo")
            row.dot:SetTexture("Interface\\COMMON\\Indicator-Yellow")
        else
            row.status:SetText("Not spawned")
            row.dot:SetTexture("Interface\\COMMON\\Indicator-Gray")
        end
        row.dot:Show()

        if isOut then
            row.spawn:SetText("Despawn")
            row.spawn:Enable()
            row.spawn:SetScript("OnClick", function() Despawn(name); RefreshAlts() end)
            HookTip(row.spawn, "Despawn", "Send " .. name .. " away.")
        else
            row.spawn:SetText("Spawn")
            row.spawn:Enable()
            row.spawn:SetScript("OnClick", function()
                Spawn(name)
                Print("Spawned " .. name .. " without inviting.")
                SetStatus("Spawned " .. name .. ".")
                RefreshAlts()
            end)
            HookTip(row.spawn, "Spawn", "Spawn " .. name .. " without inviting it, so you can invite it yourself if you want.")
        end

        if isOut then
            row.quest:Disable()
            HookTip(row.quest, "Quest solo", name .. " is already spawned or in your group.")
        else
            row.quest:Enable()
            row.quest:SetScript("OnClick", function()
                Spawn(name)
                Print(name .. " is off questing on its own.")
                SetStatus(name .. " is off questing on its own.")
                RefreshAlts()
            end)
            HookTip(row.quest, "Quest solo", "Spawn " .. name .. " without inviting, so it quests on its own.")
        end

        row.remove:SetScript("OnClick", function()
            RemoveAlt(name)
            Print("Removed " .. name .. " from your saved characters.")
            SetStatus("Removed " .. name .. ".")
            RefreshAlts()
        end)
        HookTip(row.remove, "Remove " .. name, (info and info.stale)
            and "This character could not be checked since the server looked reset. Remove it if it no longer exists."
            or "Forget this character. Sync or a login on it adds it back.")

        if inGroup then
            row.invite:Disable()
            HookTip(row.invite, "Invite", name .. " is already in your group.")
        else
            row.invite:Enable()
            row.invite:SetScript("OnClick", function()
                if spawned[name] then
                    InviteKnownAlt(name)
                else
                    SpawnAndInvite(name)
                end
                RefreshAlts()
            end)
            HookTip(row.invite, "Invite", spawned[name] and ("Invite " .. name .. ". If it is already in another group, it will leave and join you.")
                or ("Spawn " .. name .. " and invite it. If it is already in another group, it will leave and join you."))
        end
        y = y - 32
    end
    ui.altsChild:SetHeight(math.max(1, -y + 4))
end

RefreshCommands = function()
    if not win or not ui.cmdChild then return end
    HidePool(ui.pools.cmdRows)
    local grouped = GroupedNames()
    local names = {}
    for _, n in ipairs(SortedAltNames()) do
        if grouped[n] then table.insert(names, n) end
    end
    if #names == 0 then
        ui.cmdEmpty:Show()
        ui.cmdChild:SetHeight(1)
        return
    end
    ui.cmdEmpty:Hide()
    local y = -2
    for _, name in ipairs(names) do
        local row = TakeRow(ui.pools.cmdRows, ui.cmdChild, MakeCmdRow)
        row:ClearAllPoints()
        row:SetPoint("TOPLEFT", 2, y)
        row.name:SetText(name)
        row.dot:SetTexture("Interface\\COMMON\\Indicator-Green")
        row.pick:SetScript("OnClick", function()
            ui.orderChoice = name
            ui.whoBtn:SetText(name)
            SetStatus("Orders will go to " .. name .. ".")
        end)
        y = y - 28
    end
    ui.cmdChild:SetHeight(math.max(1, -y + 4))
end

local function PartyMembers()
    local list = {}
    local raid = GetNumRaidMembers()
    if raid > 0 then
        for i = 1, raid do
            local u = "raid" .. i
            if UnitExists(u) and not UnitIsUnit(u, "player") then
                table.insert(list, { unit = u, name = UnitName(u), guid = LowGuid(UnitGUID(u)) })
            end
        end
    else
        for i = 1, GetNumPartyMembers() do
            local u = "party" .. i
            if UnitExists(u) then
                table.insert(list, { unit = u, name = UnitName(u), guid = LowGuid(UnitGUID(u)) })
            end
        end
    end
    return list
end

local function ConfirmRespec(name, specName, guid, specId)
    StaticPopupDialogs["ALTBOTS_RESPEC"] = {
        text = "Switch " .. (name or "this bot") .. " to " .. (specName or "this spec") .. "?\nThis fully respecs the bot.",
        button1 = YES, button2 = NO,
        OnAccept = function()
            SendBot("LEARNSPEC", guid, tostring(specId))
            SetStatus("Switching " .. (name or "bot") .. " to " .. (specName or "a new spec") .. "...")
        end,
        timeout = 0, whileDead = 1, hideOnEscape = 1, preferredIndex = 3,
    }
    StaticPopup_Show("ALTBOTS_RESPEC")
end

local function FillChoiceRow(row, pool, maker, options, current, onPick, isLegal)
    HidePool(pool)
    local x, y = 0, 0
    local rowW = row:GetWidth() or 400
    if rowW < 100 then rowW = 400 end
    for _, opt in ipairs(options) do
        local b = TakeRow(pool, row, maker)
        b:ClearAllPoints()
        local bw = b:GetWidth() or 56
        if x > 0 and (x + bw) > rowW then
            x = 0
            y = y + 22
        end
        b:SetPoint("TOPLEFT", x, -y)
        b:SetText(opt.label or opt.id)
        if current == opt.id then
            b:Disable()
        else
            b:Enable()
        end
        if isLegal and not isLegal(opt.id) then
            b:Disable()
        end
        local id = opt.id
        b:SetScript("OnClick", function() onPick(id) end)
        x = x + bw + 4
    end
    row:SetHeight(y + 22)
end

local function MakePrefBtn(parent)
    local b = MakeTextBtn(parent, 56, 20, "")
    return b
end

RefreshParty = function()
    if not win or not ui.partyListChild then return end
    HidePool(ui.pools.partyRows)
    local members = PartyMembers()
    if #members == 0 then
        ui.partyEmpty:Show()
        ui.partyPickHint:Show()
        ui.detail:Hide()
        selectedPartyGuid, selectedPartyName, selectedPartyUnit = nil, nil, nil
        ui.partyListChild:SetHeight(1)
        return
    end
    ui.partyEmpty:Hide()
    local stillThere = false
    local y = -2
    for _, m in ipairs(members) do
        local row = TakeRow(ui.pools.partyRows, ui.partyListChild, MakePartyRow)
        row:ClearAllPoints()
        row:SetPoint("TOPLEFT", 2, y)
        row.name:SetText(ClassColorText(m.unit, m.name or "?"))
        row.dot:SetTexture(UnitIsConnected(m.unit) and "Interface\\COMMON\\Indicator-Green" or "Interface\\COMMON\\Indicator-Gray")
        local guid, name, unit = m.guid, m.name, m.unit
        if selectedPartyGuid and guid == selectedPartyGuid then
            stillThere = true
            row:SetBackdropBorderColor(0.85, 0.72, 0.25, 1)
            row:SetBackdropColor(0.16, 0.14, 0.08, 1)
        else
            row:SetBackdropBorderColor(0.28, 0.28, 0.32, 1)
            row:SetBackdropColor(0.07, 0.07, 0.09, 0.9)
        end
        row:SetScript("OnClick", function()
            selectedPartyGuid, selectedPartyName, selectedPartyUnit = guid, name, unit
            RequestPartyData(guid)
            RefreshParty()
        end)
        y = y - 30
    end
    ui.partyListChild:SetHeight(math.max(1, -y + 4))
    if not stillThere then
        selectedPartyGuid, selectedPartyName, selectedPartyUnit = nil, nil, nil
        ui.detail:Hide()
        ui.partyPickHint:Show()
        return
    end
    ui.partyPickHint:Hide()
    ui.detail:Show()
    ui.detailName:SetText(ClassColorText(selectedPartyUnit, selectedPartyName or "?"))

    local guid = selectedPartyGuid
    local roles = rolesCache[guid] or {}
    local currentRole = currentRoleCache[guid] or "auto"
    local roleOpts = {}
    for _, id in ipairs(ROLE_ORDER) do
        if id == "auto" or roles[id] then
            table.insert(roleOpts, { id = id, label = ROLE_LABELS[id] or id })
        end
    end
    FillChoiceRow(ui.roleRow, ui.pools.roleBtns, MakePrefBtn, roleOpts, currentRole, function(id)
        SendBot("SETROLE", guid, id)
        currentRoleCache[guid] = id
        SetStatus((selectedPartyName or "Bot") .. " role set to " .. (ROLE_LABELS[id] or id) .. ".")
        RefreshParty()
    end)

    local specs = specsCache[guid] or {}
    HidePool(ui.pools.specBtns)
    if #specs == 0 then
        ui.specHint:Show()
    else
        ui.specHint:Hide()
        local specOpts = {}
        for _, s in ipairs(specs) do
            table.insert(specOpts, { id = tostring(s.specId), label = s.name or ("Spec " .. s.specId) })
        end
        FillChoiceRow(ui.specRow, ui.pools.specBtns, MakePrefBtn, specOpts, nil, function(id)
            local specName = id
            for _, s in ipairs(specs) do
                if tostring(s.specId) == id then specName = s.name; break end
            end
            ConfirmRespec(selectedPartyName, specName, guid, tonumber(id))
        end)
    end

    local prefs = gearPrefsCache[guid]
    local legalArmor, legalWeapon = {}, {}
    local armorPref, weaponPref = "auto", "auto"
    if prefs then
        for _, t in ipairs(prefs.legalArmor or {}) do legalArmor[t] = true end
        for _, t in ipairs(prefs.legalWeapon or {}) do legalWeapon[t] = true end
        armorPref, weaponPref = prefs.armorPref or "auto", prefs.weaponPref or "auto"
    end
    local armorOpts, wepOpts = {}, {}
    for _, id in ipairs(ARMOR_TYPES) do
        if id == "auto" or legalArmor[id] then table.insert(armorOpts, { id = id, label = id:gsub("^%l", string.upper) }) end
    end
    for _, id in ipairs(WEAPON_TYPES) do
        if id == "auto" or legalWeapon[id] then table.insert(wepOpts, { id = id, label = id:gsub("^%l", string.upper) }) end
    end
    FillChoiceRow(ui.armorRow, ui.pools.prefArm, MakePrefBtn, armorOpts, armorPref, function(id)
        SendBot("SETGEARPREF", guid, "armor:" .. id)
        if gearPrefsCache[guid] then gearPrefsCache[guid].armorPref = id end
        SetStatus("Armor preference: " .. id .. ".")
        RefreshParty()
    end)
    FillChoiceRow(ui.weaponRow, ui.pools.prefWep, MakePrefBtn, wepOpts, weaponPref, function(id)
        SendBot("SETGEARPREF", guid, "weapon:" .. id)
        if gearPrefsCache[guid] then gearPrefsCache[guid].weaponPref = id end
        SetStatus("Weapon preference: " .. id .. ".")
        RefreshParty()
    end)

    RefreshPartyGear()
    RefreshPartyProfs()
    RefreshPartyBags()
    UpdateBagHeader()
    if LayoutPartyDetail then LayoutPartyDetail() end
end

local function PlaceDollSlot(slot, col, row)
    local b
    for _, existing in ipairs(ui.pools.gearSlots) do
        if existing.slot == slot then b = existing; break end
    end
    if not b then
        b = MakeItemButton(ui.doll, 32)
        b.slot = slot
        table.insert(ui.pools.gearSlots, b)
    end
    b:ClearAllPoints()
    b:SetPoint("TOPLEFT", col * 36, -row * 36)
    b:Show()
    return b
end

RefreshPartyGear = function()
    if not win or not ui.doll or not selectedPartyGuid then
        HidePool(ui.pools.gearSlots)
        return
    end
    local guid = selectedPartyGuid
    local gear = gearCache[guid] or {}
    HidePool(ui.pools.gearSlots)
    for row, slots in ipairs(PAPERDOLL_ROWS) do
        for i, slot in ipairs(slots) do
            local b = PlaceDollSlot(slot, i - 1, row - 1)
            local info = gear[slot]
            local entry = info and info.entry or 0
            SetItemIcon(b, entry)
            if b.count then b.count:SetText("") end
            if SlotLocked(guid, slot) then b.lock:Show() else b.lock:Hide() end
            b:SetScript("OnEnter", function(self)
                if entry and entry > 0 then
                    ShowItemTip(self, entry, "Shift-click or right-click to unequip. Alt-click to unlock this slot.")
                else
                    Tip(self, GEAR_SLOT_NAMES[slot] or ("Slot " .. slot), "Empty. Alt-click to unlock this slot.")
                end
            end)
            b:SetScript("OnClick", function(self, button)
                if IsAltKeyDown() then
                    SendBot("UNLOCK", guid, tostring(slot))
                    return
                end
                if (button == "RightButton" or IsShiftKeyDown()) and entry and entry > 0 then
                    SendBot("UNEQUIP", guid, tostring(slot))
                    SetStatus("Unequipping " .. (GEAR_SLOT_NAMES[slot] or "item") .. "...")
                end
            end)
        end
    end
end

RefreshPartyBags = function()
    if not win or not ui.bagsChild then return end
    HidePool(ui.pools.bagSlots)
    if not selectedPartyGuid then
        ui.bagsEmpty:Show()
        ui.bagsEmpty:SetText("Select a party member to see their bags.")
        ui.bagsChild:SetHeight(1)
        return
    end
    local guid = selectedPartyGuid
    local meta = bagMeta[guid]
    if not meta then
        ui.bagsEmpty:Show()
        ui.bagsEmpty:SetText(bagsRefreshing[guid] and "Refreshing..." or "Bags are still loading.")
        ui.bagsChild:SetHeight(1)
        return
    end

    local slots = {}
    for i = 0, (meta.backpack or 16) - 1 do
        table.insert(slots, { bag = BACKPACK_BAG, slot = BACKPACK_FIRST_SLOT + i })
    end
    for i, size in ipairs(meta.bags or {}) do
        local bag = WORN_BAGS[i]
        for slot = 0, (size or 0) - 1 do
            table.insert(slots, { bag = bag, slot = slot })
        end
    end
    if #slots == 0 then
        ui.bagsEmpty:Show()
        ui.bagsEmpty:SetText("This bot has no bag slots.")
        ui.bagsChild:SetHeight(1)
        return
    end
    ui.bagsEmpty:Hide()

    local items = bagItems[guid] or {}
    local size = 32
    local gap = 4
    local childW = ui.bagsChild:GetWidth() or 400
    if childW < 100 then childW = 400 end
    local cols = math.floor((childW + gap) / (size + gap))
    if cols < 4 then cols = 4 end
    if cols > 12 then cols = 12 end

    for i, pos in ipairs(slots) do
        local b = TakeRow(ui.pools.bagSlots, ui.bagsChild, function(p) return MakeItemButton(p, size) end)
        local col = (i - 1) % cols
        local row = math.floor((i - 1) / cols)
        b:ClearAllPoints()
        b:SetPoint("TOPLEFT", col * (size + gap), -row * (size + gap))
        local it = items[(pos.bag * 1000) + pos.slot]
        local entry = it and it.entry or 0
        local count = it and it.count or 0
        SetItemIcon(b, entry, EMPTY_BAG_TEXTURE)
        if count > 1 then b.count:SetText(count) else b.count:SetText("") end
        b.lock:Hide()
        local bag, slot = pos.bag, pos.slot
        b:SetScript("OnEnter", function(self)
            if entry and entry > 0 then
                local extra = "Right-click to equip. That slot will stay locked so the bot will not swap it back out."
                local tooHigh, need = ItemTooHighLevel(entry)
                if tooHigh then
                    extra = "Requires level " .. need .. ". The bot cannot equip this yet.\n" .. extra
                end
                ShowItemTip(self, entry, extra)
            else
                Tip(self, "Empty slot", "Open bag space.")
            end
        end)
        b:SetScript("OnClick", function(self, button)
            if button == "RightButton" and entry and entry > 0 then
                local tooHigh = ItemTooHighLevel(entry)
                if tooHigh then
                    local msg = "Too high level for " .. (selectedPartyName or "this bot") .. "."
                    Print(msg)
                    SetStatus(msg)
                end
                SendBot("EQUIP", guid, tostring(bag) .. ":" .. tostring(slot))
                if not tooHigh then SetStatus("Equipping on " .. (selectedPartyName or "bot") .. "...") end
            end
        end)
    end
    local rows = math.ceil(#slots / cols)
    ui.bagsChild:SetHeight(math.max(1, rows * (size + gap)))
    UpdateBagHeader()
end

local function MakeProfBtn(parent)
    local b = CreateFrame("Button", nil, parent)
    b:SetWidth(42)
    b:SetHeight(52)
    b:SetBackdrop({
        bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true, tileSize = 8, edgeSize = 8,
        insets = { left = 1, right = 1, top = 1, bottom = 1 },
    })
    b:SetBackdropColor(0.07, 0.07, 0.09, 0.9)
    b:SetBackdropBorderColor(0.3, 0.3, 0.35, 1)
    b.icon = b:CreateTexture(nil, "ARTWORK")
    b.icon:SetPoint("TOP", 0, -3)
    b.icon:SetWidth(28)
    b.icon:SetHeight(28)
    b.icon:SetTexCoord(0.08, 0.92, 0.08, 0.92)
    b.rank = b:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    b.rank:SetPoint("BOTTOM", 0, 2)
    b:SetScript("OnLeave", function() GameTooltip:Hide() end)
    return b
end

local function ConfirmUnlearnProfession(name, profName, guid, skillId)
    StaticPopupDialogs["ALTBOTS_UNLEARN_PROF"] = {
        text = "Unlearn " .. (profName or "this profession") .. " on " .. (name or "this bot") .. "?\nThis wipes the skill.",
        button1 = YES, button2 = NO,
        OnAccept = function()
            SendBot("SETPROF", guid, "none:" .. tostring(skillId))
            SetStatus("Unlearning " .. (profName or "profession") .. " on " .. (name or "bot") .. "...")
        end,
        timeout = 0, whileDead = 1, hideOnEscape = 1, preferredIndex = 3,
    }
    StaticPopup_Show("ALTBOTS_UNLEARN_PROF")
end

RefreshPartyProfs = function()
    if not win or not ui.profRow then return end
    HidePool(ui.pools.profBtns)
    if not selectedPartyGuid then
        ui.profHint:Show()
        ui.profRow:SetHeight(20)
        return
    end
    local guid = selectedPartyGuid
    local learned = profCache[guid]
    if not learned and not profAllowed[guid] then
        ui.profHint:Show()
        ui.profHint:SetText("Profession data has not arrived yet.")
        ui.profRow:SetHeight(20)
        return
    end
    ui.profHint:Hide()
    learned = learned or {}
    local rowW = ui.profRow:GetWidth() or 400
    if rowW < 100 then rowW = 400 end
    local x, y = 0, 0
    local bw, bh, gap = 42, 52, 4
    for _, info in ipairs(PROFESSION_CATALOG) do
        local allowed = not profAllowed[guid] or profAllowed[guid][info.skillId]
        if allowed then
            if x > 0 and (x + bw) > rowW then
                x = 0
                y = y + bh + gap
            end
            local b = TakeRow(ui.pools.profBtns, ui.profRow, MakeProfBtn)
            b:ClearAllPoints()
            b:SetPoint("TOPLEFT", x, -y)
            b.icon:SetTexture(info.icon)
            local skill = learned[info.skillId]
            local skillId, profName = info.skillId, info.name
            if skill then
                b.icon:SetVertexColor(1, 1, 1)
                b:SetBackdropBorderColor(0.75, 0.65, 0.25, 1)
                b.rank:SetText(skill.value .. "/" .. skill.maxValue)
                b:SetScript("OnEnter", function(self)
                    Tip(self, profName, skill.value .. " / " .. skill.maxValue .. " skill.\nClick to unlearn. This wipes the skill.")
                end)
                b:SetScript("OnClick", function()
                    ConfirmUnlearnProfession(selectedPartyName, profName, guid, skillId)
                end)
            else
                b.icon:SetVertexColor(0.4, 0.4, 0.4)
                b:SetBackdropBorderColor(0.3, 0.3, 0.35, 1)
                b.rank:SetText(info.secondary and "off" or "learn")
                b:SetScript("OnEnter", function(self)
                    Tip(self, profName, "Not learned. Click to teach this bot " .. profName .. ".")
                end)
                b:SetScript("OnClick", function()
                    SendBot("SETPROF", guid, tostring(skillId))
                    SetStatus("Teaching " .. profName .. " to " .. (selectedPartyName or "bot") .. "...")
                end)
            end
            x = x + bw + gap
        end
    end
    ui.profRow:SetHeight(y + bh)
    if LayoutPartyDetail then LayoutPartyDetail() end
end

-- ---------------------------------------------------------------------------
-- Quests tab: a grouped bot's quest log (GETQUESTS) with objective progress.
-- ---------------------------------------------------------------------------

-- Same gray cutoff the server uses (Acore::XP::GetGrayLevel), from the bot's level.
local function GrayLevel(level)
    if level <= 5 then return 0 end
    if level <= 39 then return level - 5 - math.floor(level / 10) end
    if level <= 59 then return level - 1 - math.floor(level / 5) end
    return level - 9
end

-- Quest-log difficulty colors, judged against the bot's level rather than yours.
local function QuestLevelColor(questLevel, botLevel)
    if not botLevel or botLevel <= 0 then return 1, 0.82, 0 end
    local diff = questLevel - botLevel
    if diff >= 5 then return 1, 0.1, 0.1 end
    if diff >= 3 then return 1, 0.5, 0.25 end
    if diff >= -2 then return 1, 1, 0 end
    if questLevel > GrayLevel(botLevel) then return 0.25, 0.75, 0.25 end
    return 0.5, 0.5, 0.5
end

local QUEST_STATUS_TEXT = {
    [0] = { "In progress", 0.75, 0.75, 0.75 },
    [1] = { "Complete - ready to turn in", 0.3, 0.9, 0.3 },
    [2] = { "Failed", 1, 0.25, 0.2 },
}

local function ObjectiveName(obj)
    if obj.kind == "i" and obj.entry > 0 then
        local name = GetItemInfo(obj.entry)
        if name and name ~= "" then return name end
    end
    return obj.name ~= "" and obj.name or "Objective"
end

local function RequestQuests(guid, quiet)
    if not guid then return end
    questPending[guid] = GetTime()
    questBuild[guid] = nil
    ui.questTimedOut = nil
    ui.questQuiet = quiet and true or nil
    questAutoAt = GetTime() + QUEST_AUTO_REFRESH
    SendBot("GETQUESTS", guid)
end

local function RequestAllQuests()
    local n = 0
    for _, m in ipairs(PartyMembers()) do
        if m.guid then RequestQuests(m.guid); n = n + 1 end
    end
    SetStatus(n > 0 and ("Refreshing quest logs for " .. n .. " party member" .. (n == 1 and "" or "s") .. "...")
        or "No party members to refresh.")
end

local function MakeQuestListRow(parent)
    local row = MakePartyRow(parent)
    row.count = row:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    row.count:SetPoint("RIGHT", -6, 0)
    row.count:SetJustifyH("RIGHT")
    row.name:SetWidth(112)
    return row
end

local function MakeQuestRow(parent)
    local row = CreateFrame("Button", nil, parent)
    row:SetHeight(20)
    row:SetBackdrop({
        bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
        tile = true, tileSize = 8, edgeSize = 0,
        insets = { left = 0, right = 0, top = 0, bottom = 0 },
    })
    row:SetBackdropColor(0.10, 0.09, 0.06, 0.75)
    row.title = row:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    row.title:SetPoint("LEFT", 6, 0)
    row.title:SetJustifyH("LEFT")
    row.state = row:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    row.state:SetPoint("RIGHT", -6, 0)
    row.state:SetJustifyH("RIGHT")
    row:SetHighlightTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight")
    row:SetScript("OnLeave", function() GameTooltip:Hide() end)
    return row
end

local function MakeQuestObjRow(parent)
    local row = CreateFrame("Frame", nil, parent)
    row:SetHeight(15)
    row.text = row:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    row.text:SetPoint("LEFT", 22, 0)
    row.text:SetPoint("RIGHT", -70, 0)
    row.text:SetJustifyH("LEFT")
    row.count = row:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    row.count:SetPoint("RIGHT", -8, 0)
    row.count:SetJustifyH("RIGHT")
    return row
end

local function ShowQuestTip(owner, q, botLevel)
    GameTooltip:SetOwner(owner, "ANCHOR_RIGHT")
    local ok = pcall(GameTooltip.SetHyperlink, GameTooltip, "quest:" .. q.id .. ":" .. q.level)
    if not ok or GameTooltip:NumLines() == 0 then
        GameTooltip:SetText(q.title, 1, 0.82, 0)
    end
    local st = QUEST_STATUS_TEXT[q.status] or QUEST_STATUS_TEXT[0]
    GameTooltip:AddLine(" ")
    GameTooltip:AddLine((selectedQuestName or "Bot") .. ": " .. st[1], st[2], st[3], st[4])
    for _, obj in ipairs(q.objectives) do
        local done = obj.have >= obj.need
        GameTooltip:AddDoubleLine(ObjectiveName(obj), obj.have .. "/" .. obj.need,
            0.85, 0.85, 0.85, done and 0.3 or 1, done and 0.9 or 1, done and 0.3 or 1)
    end
    GameTooltip:AddLine("Quest level " .. q.level .. (botLevel and botLevel > 0 and (", bot level " .. botLevel) or "") ..
        "  |  ID " .. q.id, 0.5, 0.5, 0.5)
    GameTooltip:Show()
end

BuildQuestPane = function(body)
    local pane = CreateFrame("Frame", nil, body)
    pane:SetAllPoints()
    pane:Hide()
    ui.questPane = pane

    local left = CreateFrame("Frame", nil, pane)
    left:SetPoint("TOPLEFT", 0, 0)
    left:SetPoint("BOTTOMLEFT", 0, 0)
    left:SetWidth(176)
    left:SetBackdrop({
        bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true, tileSize = 8, edgeSize = 10,
        insets = { left = 2, right = 2, top = 2, bottom = 2 },
    })
    left:SetBackdropColor(0.04, 0.04, 0.06, 0.9)
    left:SetBackdropBorderColor(0.35, 0.32, 0.22, 1)
    local lh, lr = SectionHeader(left, "Party")
    lh:SetPoint("TOPLEFT", 8, -6)
    lr:SetPoint("LEFT", lh, "RIGHT", 6, 0)
    lr:SetPoint("RIGHT", -8, 0)
    ui.questListEmpty = left:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    ui.questListEmpty:SetPoint("TOPLEFT", 8, -28)
    ui.questListEmpty:SetPoint("TOPRIGHT", -8, -28)
    ui.questListEmpty:SetJustifyH("LEFT")
    ui.questListEmpty:SetText("Invite a bot to your group to see its quests here.")
    local lsf = CreateFrame("ScrollFrame", "AltBotsQuestListScroll", left, "UIPanelScrollFrameTemplate")
    lsf:SetPoint("TOPLEFT", 4, -24)
    lsf:SetPoint("BOTTOMRIGHT", -22, 4)
    local lc = CreateFrame("Frame", nil, lsf)
    lc:SetWidth(148); lc:SetHeight(1)
    lsf:SetScrollChild(lc)
    ui.questListChild = lc

    local right = CreateFrame("Frame", nil, pane)
    right:SetPoint("TOPLEFT", left, "TOPRIGHT", 6, 0)
    right:SetPoint("BOTTOMRIGHT", 0, 0)

    ui.questPickHint = right:CreateFontString(nil, "OVERLAY", "GameFontDisable")
    ui.questPickHint:SetPoint("CENTER", 0, 20)
    ui.questPickHint:SetText("Select a party member on the left.")

    local detail = CreateFrame("Frame", nil, right)
    detail:SetAllPoints()
    detail:Hide()
    ui.questDetail = detail

    ui.questName = detail:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
    ui.questName:SetPoint("TOPLEFT", 4, -2)

    ui.questRefreshAllBtn = MakeTextBtn(detail, 86, 20, "Refresh all")
    ui.questRefreshAllBtn:SetPoint("TOPRIGHT", -4, -2)
    HookTip(ui.questRefreshAllBtn, "Refresh all", "Ask every grouped bot for a fresh quest log.")
    ui.questRefreshAllBtn:SetScript("OnClick", function()
        RequestAllQuests()
        RefreshQuests()
    end)

    ui.questRefreshBtn = MakeTextBtn(detail, 86, 20, "Refresh")
    ui.questRefreshBtn:SetPoint("RIGHT", ui.questRefreshAllBtn, "LEFT", -4, 0)
    HookTip(ui.questRefreshBtn, "Refresh", "Ask this bot for its current quest log. The open log also refreshes itself every " .. QUEST_AUTO_REFRESH .. " seconds.")
    ui.questRefreshBtn:SetScript("OnClick", function()
        if selectedQuestGuid then
            RequestQuests(selectedQuestGuid)
            SetStatus("Refreshing quests for " .. (selectedQuestName or "bot") .. "...")
            RefreshQuests()
        else
            SetStatus("Select a party member first.")
        end
    end)

    local qh, qr = SectionHeader(detail, "Quest log")
    qh:SetPoint("TOPLEFT", 4, -28)
    qr:SetPoint("LEFT", qh, "RIGHT", 8, 0)
    qr:SetPoint("RIGHT", -8, 0)
    ui.questHeader = qh

    local qsf = CreateFrame("ScrollFrame", "AltBotsQuestScroll", detail, "UIPanelScrollFrameTemplate")
    qsf:SetPoint("TOPLEFT", 4, -46)
    qsf:SetPoint("BOTTOMRIGHT", -28, 4)
    ui.questScroll = qsf
    local qc = CreateFrame("Frame", nil, qsf)
    qc:SetWidth(440); qc:SetHeight(1)
    qsf:SetScrollChild(qc)
    ui.questChild = qc

    ui.questEmpty = detail:CreateFontString(nil, "OVERLAY", "GameFontDisable")
    ui.questEmpty:SetPoint("CENTER", qsf, "CENTER", 0, 0)
    ui.questEmpty:SetWidth(380)
    ui.questEmpty:SetJustifyH("CENTER")
    ui.questEmpty:SetText("")
end

-- Called by HandleIncoming (which sits above `ui`) once a full QUESTEND log is in.
OnQuestLogArrived = function(guid, log)
    if guid == selectedQuestGuid then
        if ui.activeTab == "quests" and not ui.questQuiet then
            local n = #log.list
            SetStatus((selectedQuestName or "Bot") .. ": " .. n .. " quest" .. (n == 1 and "" or "s") .. ".")
        end
        ui.questQuiet = nil
    end
    RefreshQuests()
end

local function QuestSummary(log)
    local total, ready, failed = #log.list, 0, 0
    for _, q in ipairs(log.list) do
        if q.status == 1 then ready = ready + 1 elseif q.status == 2 then failed = failed + 1 end
    end
    local parts = { total .. " quest" .. (total == 1 and "" or "s") }
    if ready > 0 then table.insert(parts, ready .. " ready to turn in") end
    if failed > 0 then table.insert(parts, failed .. " failed") end
    return table.concat(parts, "  |  "), total, ready
end

-- Draws the member list and the selected bot's log. `opening` = the tab was just shown:
-- pick someone if nobody is selected, and ask for their log if we have none yet.
RefreshQuests = function(opening)
    if not win or not ui.questListChild then return end
    HidePool(ui.pools.questListRows)
    local members = PartyMembers()
    if #members == 0 then
        ui.questListEmpty:Show()
        ui.questPickHint:Show()
        ui.questDetail:Hide()
        selectedQuestGuid, selectedQuestName, selectedQuestUnit = nil, nil, nil
        ui.questListChild:SetHeight(1)
        return
    end
    ui.questListEmpty:Hide()

    local found = false
    for _, m in ipairs(members) do
        if selectedQuestGuid and m.guid == selectedQuestGuid then
            found = true
            selectedQuestName, selectedQuestUnit = m.name, m.unit
        end
    end
    if not found then
        selectedQuestGuid, selectedQuestName, selectedQuestUnit = nil, nil, nil
        if opening then
            -- Follow the Party tab's pick, else the first member.
            local pick = members[1]
            for _, m in ipairs(members) do
                if selectedPartyGuid and m.guid == selectedPartyGuid then pick = m end
            end
            selectedQuestGuid, selectedQuestName, selectedQuestUnit = pick.guid, pick.name, pick.unit
        end
    end
    if opening and selectedQuestGuid and not questLog[selectedQuestGuid] and not questPending[selectedQuestGuid] then
        RequestQuests(selectedQuestGuid)
    end

    local y = -2
    for _, m in ipairs(members) do
        local row = TakeRow(ui.pools.questListRows, ui.questListChild, MakeQuestListRow)
        row:ClearAllPoints()
        row:SetPoint("TOPLEFT", 2, y)
        row.name:SetText(ClassColorText(m.unit, m.name or "?"))
        row.dot:SetTexture(UnitIsConnected(m.unit) and "Interface\\COMMON\\Indicator-Green" or "Interface\\COMMON\\Indicator-Gray")
        local log = m.guid and questLog[m.guid]
        if log then
            local _, total, ready = QuestSummary(log)
            row.count:SetText(ready > 0 and ("|cff4ce64c" .. ready .. "|r/" .. total) or tostring(total))
        else
            row.count:SetText("")
        end
        if selectedQuestGuid and m.guid == selectedQuestGuid then
            row:SetBackdropBorderColor(0.85, 0.72, 0.25, 1)
            row:SetBackdropColor(0.16, 0.14, 0.08, 1)
        else
            row:SetBackdropBorderColor(0.28, 0.28, 0.32, 1)
            row:SetBackdropColor(0.07, 0.07, 0.09, 0.9)
        end
        local guid, name, unit = m.guid, m.name, m.unit
        row:SetScript("OnClick", function()
            selectedQuestGuid, selectedQuestName, selectedQuestUnit = guid, name, unit
            RequestQuests(guid)
            SetStatus("Loading quests for " .. (name or "bot") .. "...")
            RefreshQuests()
        end)
        y = y - 30
    end
    ui.questListChild:SetHeight(math.max(1, -y + 4))

    HidePool(ui.pools.questRows)
    HidePool(ui.pools.questObjRows)
    if not selectedQuestGuid then
        ui.questDetail:Hide()
        ui.questPickHint:Show()
        return
    end
    ui.questPickHint:Hide()
    ui.questDetail:Show()
    ui.questName:SetText(ClassColorText(selectedQuestUnit, selectedQuestName or "?"))

    local guid = selectedQuestGuid
    local log = questLog[guid]
    local pending = questPending[guid]
    local timedOut = pending and (GetTime() - pending) > QUEST_TIMEOUT

    local childW = (ui.questScroll:GetWidth() or 0)
    if childW < 200 then childW = 440 end
    ui.questChild:SetWidth(childW)

    if not log then
        ui.questHeader:SetText("Quest log")
        ui.questChild:SetHeight(1)
        ui.questEmpty:Show()
        if timedOut then
            ui.questEmpty:SetText("No answer from " .. (selectedQuestName or "this member") .. ".\nOnly bots in your group answer. If this is a bot, rebuild the server with the quest log patch, then press Refresh.")
        else
            ui.questEmpty:SetText("Loading quests...")
        end
        return
    end

    local summary = QuestSummary(log)
    if pending and not timedOut then summary = summary .. "  |  Refreshing..." end
    ui.questHeader:SetText("Quest log  |  " .. summary)

    if #log.list == 0 then
        ui.questEmpty:Show()
        ui.questEmpty:SetText((selectedQuestName or "This bot") .. " has no quests.")
        ui.questChild:SetHeight(1)
        return
    end
    ui.questEmpty:Hide()

    local botLevel = log.botLevel
    if (not botLevel or botLevel <= 0) and selectedQuestUnit then botLevel = UnitLevel(selectedQuestUnit) end
    local qy = 0
    for _, q in ipairs(log.list) do
        local row = TakeRow(ui.pools.questRows, ui.questChild, MakeQuestRow)
        row:ClearAllPoints()
        row:SetPoint("TOPLEFT", 0, qy)
        row:SetWidth(childW)
        row.title:SetWidth(childW - 150)
        local r, g, b = QuestLevelColor(q.level, botLevel)
        row.title:SetText("[" .. q.level .. "] " .. q.title)
        row.title:SetTextColor(r, g, b)
        local st = QUEST_STATUS_TEXT[q.status] or QUEST_STATUS_TEXT[0]
        local stateText = st[1]
        if q.status == 0 and #q.objectives > 0 then
            local done = 0
            for _, obj in ipairs(q.objectives) do
                if obj.have >= obj.need then done = done + 1 end
            end
            stateText = done .. "/" .. #q.objectives .. " done"
        elseif q.status == 1 then
            stateText = "Ready to turn in"
        end
        row.state:SetText(stateText)
        row.state:SetTextColor(st[2], st[3], st[4])
        local quest = q
        row:SetScript("OnEnter", function(self) ShowQuestTip(self, quest, botLevel) end)
        qy = qy - 21

        if #q.objectives == 0 then
            local orow = TakeRow(ui.pools.questObjRows, ui.questChild, MakeQuestObjRow)
            orow:ClearAllPoints()
            orow:SetPoint("TOPLEFT", 0, qy)
            orow:SetWidth(childW)
            orow.text:SetText(q.status == 1 and "Complete - ready to turn in" or (q.status == 2 and "Quest failed" or "No tracked objectives (talk to an NPC or deliver)"))
            orow.text:SetTextColor(0.55, 0.55, 0.55)
            orow.count:SetText("")
            qy = qy - 15
        end
        for _, obj in ipairs(q.objectives) do
            local orow = TakeRow(ui.pools.questObjRows, ui.questChild, MakeQuestObjRow)
            orow:ClearAllPoints()
            orow:SetPoint("TOPLEFT", 0, qy)
            orow:SetWidth(childW)
            local done = obj.have >= obj.need
            local label = ObjectiveName(obj)
            if obj.kind == "e" then
                orow.text:SetText("- " .. label)
                orow.count:SetText(done and "Done" or "Not yet")
            else
                orow.text:SetText("- " .. label)
                orow.count:SetText(obj.have .. "/" .. obj.need)
            end
            if done then
                orow.text:SetTextColor(0.45, 0.85, 0.45)
                orow.count:SetTextColor(0.45, 0.85, 0.45)
            else
                orow.text:SetTextColor(0.85, 0.85, 0.85)
                orow.count:SetTextColor(1, 1, 1)
            end
            qy = qy - 15
        end
        qy = qy - 6
    end
    ui.questChild:SetHeight(math.max(1, -qy + 4))
end

ShowTab = function(id)
    EnsureWindow()
    HidePopup()
    ui.activeTab = id
    AltBotsDB.tab = id
    ui.altsPane:Hide(); ui.cmdPane:Hide(); ui.partyPane:Hide(); ui.questPane:Hide()
    SetTabState(ui.tabs.alts, id == "alts")
    SetTabState(ui.tabs.commands, id == "commands")
    SetTabState(ui.tabs.party, id == "party")
    SetTabState(ui.tabs.quests, id == "quests")
    if id == "alts" then
        local data = RealmData()
        if data.serverList and altSync.state ~= "waiting" and time() - (data.syncedAt or 0) > SYNC_REFRESH then
            RequestAltSync(true)
        end
        ui.altsPane:Show(); RefreshAlts()
    elseif id == "commands" then
        ui.cmdPane:Show(); RefreshCommands()
    elseif id == "quests" then
        ui.questPane:Show(); RefreshQuests(true)
    else
        ui.partyPane:Show(); RefreshParty()
    end
end

frame:SetScript("OnUpdate", function()
    local now = GetTime()
    for name, p in pairs(pending) do
        if now >= p.at then
            pending[name] = nil
            InviteKnownAlt(name)
        end
    end
    if #queue > 0 and now >= nextSendAt then
        local item = table.remove(queue, 1)
        if item.gm then
            GM(item.gm)
        elseif item.addon then
            SendAddonMessage(PREFIX, item.addon, "WHISPER", UnitName("player"))
        end
        if item.after then item.after() end
        nextSendAt = now + SEND_GAP
    end
    if syncAt and now >= syncAt then
        syncAt = nil
        RequestAltSync(true)
    end
    if altSync.state == "waiting" and now - (altSync.sentAt or now) > SYNC_TIMEOUT then
        OnAltSyncTimeout()
        if OnAltsChanged then OnAltsChanged() end
    end
    if itemInfoTries and itemInfoTries > 0 and now >= (itemInfoRetryAt or 0) then
        itemInfoTries = itemInfoTries - 1
        itemInfoRetryAt = now + 0.5
        if RefreshPartyGear then RefreshPartyGear() end
        if RefreshPartyBags then RefreshPartyBags() end
        if RefreshQuests and ui.activeTab == "quests" then RefreshQuests() end
    end
    if win and win:IsShown() and ui.activeTab == "quests" then
        -- Redraw once a request times out, and quietly re-ask the selected bot now and then.
        local guid = selectedQuestGuid
        if guid and questPending[guid] and now - questPending[guid] > QUEST_TIMEOUT and not ui.questTimedOut then
            ui.questTimedOut = true
            RefreshQuests()
        end
        if guid and questLog[guid] and not questPending[guid] and now >= questAutoAt then
            RequestQuests(guid, true)
        end
    end
    if delayedRefreshAt and now >= delayedRefreshAt then
        delayedRefreshAt = nil
        local guids = delayedRefreshGuids or {}
        delayedRefreshGuids = nil
        for _, g in ipairs(guids) do
            RequestGearAndBags(g)
        end
        if #guids > 0 then
            SetStatus("Refreshing bags...")
        end
    end
end)

OnAltsChanged = function()
    if win and win:IsShown() then
        if ui.activeTab == "alts" then RefreshAlts()
        elseif ui.activeTab == "commands" then RefreshCommands()
        end
    end
end

local MISSING_GUID_PATTERN = "^BotMgr: no character with guid (%d+) found"

local function OnSystemMessage(text)
    local g = tonumber(text and text:match(MISSING_GUID_PATTERN))
    if not g then return end
    for name, info in pairs(Alts()) do
        if info.guid == g and name ~= UnitName("player") then
            RemoveAlt(name)
            Print("Removed " .. name .. ": the server has no character with guid " .. g .. " any more.")
            OnAltsChanged()
        end
    end
end

frame:RegisterEvent("ADDON_LOADED")
frame:RegisterEvent("PLAYER_ENTERING_WORLD")
frame:RegisterEvent("PLAYER_LEVEL_UP")
frame:RegisterEvent("CHAT_MSG_SYSTEM")
frame:RegisterEvent("CHAT_MSG_ADDON")
frame:RegisterEvent("CHAT_MSG_WHISPER")
frame:RegisterEvent("PARTY_MEMBERS_CHANGED")
frame:RegisterEvent("RAID_ROSTER_UPDATE")
frame:SetScript("OnEvent", function(self, event, arg1, arg2, arg3, arg4)
    if event == "ADDON_LOADED" and arg1 == "AltBots" then
        MigrateDB()
    elseif event == "PLAYER_ENTERING_WORLD" then
        RegisterSelf()
        CreateMinimapButton()
        if not syncScheduled then
            syncScheduled = true
            syncAt = GetTime() + SYNC_DELAY
        end
    elseif event == "PLAYER_LEVEL_UP" then
        local me = Alts()[UnitName("player") or ""]
        if me and tonumber(arg1) then me.level = tonumber(arg1) end
    elseif event == "CHAT_MSG_SYSTEM" then
        OnSystemMessage(arg1)
    elseif event == "CHAT_MSG_ADDON" and arg1 == PREFIX then
        HandleIncoming(arg2)
    elseif event == "CHAT_MSG_WHISPER" and arg1 and string.sub(arg1, 1, 7) == "COABOT\t" then
        HandleIncoming(string.sub(arg1, 8))
    elseif event == "PARTY_MEMBERS_CHANGED" or event == "RAID_ROSTER_UPDATE" then
        if win and win:IsShown() then
            if ui.activeTab == "party" then RefreshParty()
            elseif ui.activeTab == "quests" then RefreshQuests()
            elseif ui.activeTab == "alts" then RefreshAlts()
            elseif ui.activeTab == "commands" then RefreshCommands()
            end
        end
    end
end)

local function ToggleWindow()
    EnsureWindow()
    if ui.radius then
        ui.radius:SetText(tostring(AltBotsDB.farmRadius or 40))
        ui.radius:SetScript("OnEditFocusLost", function(self)
            AltBotsDB.farmRadius = ClampRadius(self:GetText())
            self:SetText(tostring(AltBotsDB.farmRadius))
        end)
    end
    if win:IsShown() then
        HidePopup()
        win:Hide()
    else
        win:Show()
        ShowTab(ui.activeTab or AltBotsDB.tab or "alts")
        SetStatus("Ready.")
    end
end

local USAGE = "/alt  open window | /alt invite|solo|despawn <name> | /alt list | /alt sync | /alt prune"
    .. " | /alt add <name> <guid> | /alt remove <name> | /alt clear"
    .. " | /alt follow|stay|attack|stopattack|pull <name|all> | /alt farm <name|all> [radius] | /alt farmstop <name|all>"
    .. " | /alt inventory <name|all> | /alt minimap"

local function OrderArg(name)
    if name and name:lower() == "all" then return ALL_GROUPED end
    return name
end

SLASH_ALTBOTS1 = "/altbots"
SLASH_ALTBOTS2 = "/alt"
SlashCmdList["ALTBOTS"] = function(msg)
    msg = msg or ""
    local cmd, arg = msg:match("^(%S*)%s*(.-)$")
    cmd = (cmd or ""):lower()
    if cmd == "" then
        ToggleWindow()
    elseif cmd == "list" then
        local names = {}
        for n in pairs(Alts()) do table.insert(names, n) end
        table.sort(names)
        if #names == 0 then Print("No saved characters on " .. Realm() .. ".") end
        for _, n in ipairs(names) do
            local info = Alts()[n]
            Print(AltName(n, info) .. " = " .. tostring(info.guid) .. " (" .. (info.source or "login") .. ")"
                .. (info.stale and " |cffff9933not verified|r" or ""))
        end
    elseif cmd == "sync" then
        RequestAltSync(false)
        Print("Asking the server for your character list...")
    elseif cmd == "prune" then
        local n = 0
        for name, info in pairs(Alts()) do
            if info.stale and name ~= UnitName("player") and RemoveAlt(name) then n = n + 1 end
        end
        Print("Removed " .. n .. " unverified character" .. (n == 1 and "" or "s") .. ".")
        OnAltsChanged()
    elseif cmd == "clear" then
        RealmData().chars = {}
        RealmData().syncedAt = nil
        for k in pairs(spawned) do spawned[k] = nil end
        RegisterSelf()
        RequestAltSync(false)
        Print("Cleared the saved characters for " .. Realm() .. " and asked the server for a fresh list.")
        OnAltsChanged()
    elseif cmd == "add" then
        local n, g = arg:match("^(%S+)%s+(%d+)$")
        if n then
            Alts()[n] = { guid = tonumber(g), source = "manual" }
            Print("Saved " .. n .. " = " .. g)
            OnAltsChanged()
        else
            Print("Usage: /alt add <name> <guid>")
        end
    elseif cmd == "remove" then
        local n = ResolveAlt(arg)
        if n then
            RemoveAlt(n); Print("Removed " .. n); OnAltsChanged()
        elseif arg and arg ~= "" then
            Print("I don't know '" .. arg .. "'.")
        else
            Print("Usage: /alt remove <name>")
        end
    elseif cmd == "invite" then
        SpawnAndInvite(ResolveAlt(arg) or arg)
    elseif cmd == "spawn" or cmd == "solo" then
        local n = ResolveAlt(arg)
        if n then Spawn(n) else Print("I don't know '" .. tostring(arg) .. "' yet.") end
    elseif cmd == "despawn" then
        local n = ResolveAlt(arg)
        if n then Despawn(n) else Print("I don't know '" .. tostring(arg) .. "' yet.") end
    elseif cmd == "farm" then
        local n, r = arg:match("^(%S+)%s*(%d*)$")
        if not n then Print("Usage: /alt farm <name|all> [radius]") return end
        if r == "" then r = AltBotsDB.farmRadius or 40 end
        Order("farm", OrderArg(n), r)
    elseif cmd == "farmstop" or cmd == "follow" or cmd == "stay" or cmd == "attack" or cmd == "stopattack" or cmd == "pull" or cmd == "inventory" then
        local n = arg:match("^(%S+)$")
        if not n then Print("Usage: /alt " .. cmd .. " <name|all>") return end
        Order(cmd, OrderArg(n))
    elseif cmd == "minimap" then
        SetMinimapShown(not GetMinimapShown())
        Print(GetMinimapShown() and "Minimap button shown." or "Minimap button hidden.")
    elseif cmd == "help" then
        Print(USAGE)
    else
        Print(USAGE)
    end
end
