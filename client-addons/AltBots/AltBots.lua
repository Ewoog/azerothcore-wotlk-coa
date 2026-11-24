-- AltBots: remembers each of your characters' GUIDs when you log into them, and
-- syncs that list with the server's list for your account (GETALTS), so characters
-- removed by a wipe disappear and new ones show up without a login on each. Then
-- your main can spawn them as CoA Companions bots and invite them.
-- Alts tab: spawn, quest solo, invite or despawn, with a name/class filter.
-- Commands tab: follow, stay, attack, stop attack, pull, farm, collect (open the nearest
-- worldforged pickup or chest) and inventory (clean bags) orders for one alt or every alt in
-- your group, and live health, power and range cards. Materials: farm a material (a gather
-- order delivered by mail or to the guild bank), or mail/bank it now.
-- Party tab bags: left-click an item to trade it to you, mail it, bank it or farm more of it.
-- Rules tab: standing rules ("keep mailing/banking this item") for every alt on your account;
-- sent to the server at login and on every change (RULECLEAR / RULE).
-- Quick bar and key bindings (Bindings.xml): the same orders for every grouped alt.
-- Quests tab: each bot's quest log, an All bots view that shows who is missing which
-- quest, a filter, and Share quests (SHAREQUESTS) for your own alts.
-- Party tab: inspect, role, spec, gear preferences, professions, equipped gear
-- and bags of grouped bots (COABOT GETGEAR/GETBAGS/EQUIP/UNEQUIP/GETPROFS/SETPROF).
-- It only sends .botcmd chat commands, COABOT addon messages to yourself
-- and a normal group invite. No combat automation runs in the client.

AltBotsDB = AltBotsDB or {}

local PREFIX = "COABOT"
local W = {}
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

local ORDER_TEXT = {
    collect = {
        started = "is going for %s.", noobject = "found nothing it can open near you.",
        combat = "is in combat.", dead = "is dead.", toofar = "is not near you.",
        ok = "collected it.", failed = "could not collect it.",
    },
    trade = {
        coming = "is coming to trade.", none = "does not carry that item.", bound = "cannot trade that item (soulbound).",
        busy = "is already trading.", combat = "is in combat.", dead = "is dead.", toofar = "is too far away to trade.",
        open = "opened a trade. Accept it to take the item.", ok = "handed the item over.",
        cancelled = "the trade was cancelled.", timeout = "closed the trade (no accept).", failed = "could not trade.",
    },
    mail = { ok = "mailed %s item(s) to you.", none = "does not carry that item.", bound = "cannot mail that item (soulbound).",
        tool = "keeps its profession tools." },
    bank = { ok = "put %s item(s) in the guild bank.", none = "does not carry that item.", bound = "cannot bank that item (soulbound).",
        tool = "keeps its profession tools.", noguild = "is not in a guild.", bankfull = "found no room in the guild bank." },
    gather = { placed = "left the group to farm it. Mail or bank delivery as you picked; see chat for progress.",
        noguild = "is not in a guild, so it cannot bank. Pick Mail instead.", failed = "could not take the order (see chat)." },
    gatherstop = { ok = "stopped its farm order.", denied = "is not farming for you.", none = "has no farm order." },
}

local ORDER_ASYNC = { ok = true, failed = true, open = true, cancelled = true, timeout = true }
local gatherCatalog, gatherCatalogSeen, gatherCatalogOrder = {}, {}, {}

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
    winPos = true, quickBarPos = true, quickBar = true, scale = true, matDeliver = true, matCount = true, sendRules = true,
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

local function AltNameByGuid(low)
    for name, info in pairs(Alts()) do
        if info.guid == low then return name end
    end
    return nil
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
local BuildRulesPane, RefreshRules, SyncRules
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

    elseif verb == "ORDER" and guid then
        local kind, token, extra = parts[3], parts[4], parts[5]
        local texts = ORDER_TEXT[kind] or {}
        local who = AltNameByGuid(guid) or ("guid " .. guid)
        local text = texts[token] or ((kind or "order") .. ": " .. tostring(token))
        text = string.format(text, extra or "it")
        local msg = who .. " " .. text
        SetStatus(msg)
        local serverSays = (kind == "collect" or kind == "trade") and ORDER_ASYNC[token]
        if not serverSays and token ~= "started" and token ~= "coming" and token ~= "ok" and token ~= "placed" then Print(msg) end
        if (kind == "mail" or kind == "bank" or kind == "trade") and token == "ok" and RequestGearAndBags then
            RequestGearAndBags(guid, "Refreshing bags...")
        end
        return

    elseif verb == "GCAT" then
        local category, list = body:match("^GCAT:([^:]*):(.*)$")
        if category then
            gatherCatalog[category] = gatherCatalog[category] or {}
            if not gatherCatalogSeen[category] then
                gatherCatalogSeen[category] = true
                table.insert(gatherCatalogOrder, category)
            end
            for chunk in string.gmatch(list or "", "[^|]+") do
                local id, name = chunk:match("^(%d+),(.*)$")
                if id then table.insert(gatherCatalog[category], { id = tonumber(id), name = name }) end
            end
        end
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

local COLLECT_RADIUS = 50

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
        elseif verb == "collect" then
            if grouped[n] then
                Queue({ addon = "COLLECT:" .. low .. ":" .. COLLECT_RADIUS }); table.insert(sent, n)
            else
                Print(n .. " is not in your group, so it can't collect for you.")
            end
        else
            Queue({ gm = ".botcmd " .. verb .. " " .. low }); table.insert(sent, n)
        end
    end
    if #sent > 0 then
        local labels = { follow = "follow", stay = "stay", attack = "attack nearest", stopattack = "stop attacking",
            pull = "pull your target", farmstop = "stop farming", inventory = "clean bags",
            collect = "collect the nearest pickup" }
        local what = labels[verb] or verb
        if verb == "farm" then what = "farm your target (" .. ClampRadius(radius) .. " yd)" end
        Print(table.concat(sent, ", ") .. ": " .. what .. ".")
        SetStatus(table.concat(sent, ", ") .. ": " .. what .. ".")
        if verb == "inventory" then ScheduleBagRefresh(sent) end
    end
end

local function ItemIdFrom(text)
    if not text then return nil end
    return tonumber(text:match("item:(%d+)") or text:match("^%s*(%d+)%s*$"))
end

local function ItemLabel(id)
    local name = id and GetItemInfo(id)
    return name or (id and ("item " .. id)) or "?"
end

local function MaterialDeliver()
    return AltBotsDB.matDeliver == "bank" and "bank" or "mail"
end

-- Farm-material order for one alt: a gather order the bot travels for, delivered by mail or to
-- its guild bank. The bot leaves your group while it works.
local function FarmMaterial(choice, itemId, count)
    if not itemId then Print("Pick a material first (catalog, item ID or shift-click an item link).") return end
    if choice == ALL_GROUPED then
        Print("Pick one alt in 'Send to' for a farm order. It leaves your group while it farms.")
        SetStatus("Pick one alt for a farm order.")
        return
    end
    local n = ResolveAlt(choice)
    local low = n and AltGuid(n)
    if not low then return end
    count = math.max(1, math.min(1000, tonumber(count) or 20))
    SendBot("GATHER", low, itemId .. ":" .. count .. ":" .. MaterialDeliver())
    SetStatus(n .. ": farm " .. count .. "x " .. ItemLabel(itemId) .. " (" .. (MaterialDeliver() == "bank" and "guild bank" or "mail") .. ")...")
end

local function StopFarmMaterial(choice)
    for _, n in ipairs(OrderTargets(choice)) do
        SendBot("GATHERSTOP", AltGuid(n))
    end
end

local function SendMaterial(choice, itemId, mode, count)
    if not itemId then Print("Pick a material first (catalog, item ID or shift-click an item link).") return end
    for _, n in ipairs(OrderTargets(choice)) do
        SendBot("SENDITEM", AltGuid(n), itemId .. ":" .. mode .. ((count and count > 0) and (":" .. count) or ""))
    end
    SetStatus((mode == "bank" and "Banking " or "Mailing ") .. ItemLabel(itemId) .. "...")
end

local function TradeItemToMe(low, itemId, count)
    SendBot("GIVEITEM", low, itemId .. ((count and count > 0) and (":" .. count) or ""))
end

local function Spawn(name)
    local low = AltGuid(name); if not low then return end
    Queue({ gm = ".botcmd spawnbot " .. low }); spawned[name] = true
end
local function Despawn(name)
    local low = AltGuid(name); if not low then return end
    Queue({ gm = ".botcmd despawn " .. low }); spawned[name] = nil
end

local win, popup, minimapBtn, quickBar
local ui = {}

local WIN_W, WIN_H = 720, 580
local ROW_W = 664
local FLAT = "Interface\\Buttons\\WHITE8X8"
local CLASS_ICON_FILE = "Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-Classes"

-- Flat slate panels with one blue accent (the same blue as the chat prefix).
local C = {
    window = { 0.055, 0.059, 0.071, 0.97 },
    header = { 0.078, 0.083, 0.102, 1 },
    panel = { 0.082, 0.088, 0.107, 0.96 },
    row = { 0.110, 0.117, 0.141, 0.94 },
    rowSel = { 0.120, 0.200, 0.300, 1 },
    border = { 0.170, 0.180, 0.220, 1 },
    borderHi = { 0.320, 0.340, 0.400, 1 },
    btn = { 0.150, 0.160, 0.195, 1 },
    btnOn = { 0.160, 0.420, 0.680, 1 },
    accent = { 0.270, 0.680, 1.000, 1 },
    text = { 0.920, 0.930, 0.960 },
    muted = { 0.560, 0.590, 0.660 },
    faint = { 0.400, 0.420, 0.480 },
    good = { 0.330, 0.850, 0.470 },
    warn = { 1.000, 0.700, 0.250 },
    bad = { 1.000, 0.330, 0.300 },
    track = { 0, 0, 0, 0.45 },
}

local BODY_FONT = (NumberFontNormal and NumberFontNormal:GetFont()) or "Fonts\\ARIALN.TTF"
local TITLE_FONT = (GameFontNormal and GameFontNormal:GetFont()) or "Fonts\\FRIZQT__.TTF"

local function MakeFont(name, path, size, color)
    local f = CreateFont(name)
    f:SetFont(path, size, "")
    f:SetTextColor(color[1], color[2], color[3])
    f:SetShadowColor(0, 0, 0, 0.85)
    f:SetShadowOffset(1, -1)
    return f
end

local F = {
    title = MakeFont("AltBotsFontTitle", TITLE_FONT, 15, C.text),
    big = MakeFont("AltBotsFontBig", TITLE_FONT, 12, C.text),
    body = MakeFont("AltBotsFontBody", BODY_FONT, 14, C.text),
    small = MakeFont("AltBotsFontSmall", BODY_FONT, 12, C.muted),
    caps = MakeFont("AltBotsFontCaps", BODY_FONT, 12, C.accent),
    button = MakeFont("AltBotsFontButton", BODY_FONT, 13, C.text),
    buttonOff = MakeFont("AltBotsFontButtonOff", BODY_FONT, 13, C.faint),
}

local FLAT_BACKDROP = {
    bgFile = FLAT, edgeFile = FLAT, tile = false, edgeSize = 1,
    insets = { left = 1, right = 1, top = 1, bottom = 1 },
}

function W.Skin(f, bg, border)
    f:SetBackdrop(FLAT_BACKDROP)
    bg = bg or C.panel
    border = border or C.border
    f:SetBackdropColor(bg[1], bg[2], bg[3], bg[4] or 1)
    f:SetBackdropBorderColor(border[1], border[2], border[3], border[4] or 1)
end

function W.Paint(tex, c, alpha)
    tex:SetTexture(c[1], c[2], c[3], alpha or c[4] or 1)
end

function W.TextColor(fs, c)
    fs:SetTextColor(c[1], c[2], c[3])
end

function W.Hex(c)
    return string.format("|cff%02x%02x%02x",
        math.floor(c[1] * 255 + 0.5), math.floor(c[2] * 255 + 0.5), math.floor(c[3] * 255 + 0.5))
end

function W.AddHover(b, alpha)
    local hl = b:CreateTexture(nil, "HIGHLIGHT")
    hl:SetPoint("TOPLEFT", 1, -1)
    hl:SetPoint("BOTTOMRIGHT", -1, 1)
    hl:SetTexture(1, 1, 1, alpha or 0.08)
    return hl
end

local DOT_COLORS = { green = C.good, yellow = C.warn, red = C.bad, gray = C.faint }

function W.SetDot(tex, state)
    W.Paint(tex, DOT_COLORS[state] or C.faint)
end

function W.SetClassIcon(tex, classFile)
    local coords = classFile and CLASS_ICON_TCOORDS and CLASS_ICON_TCOORDS[classFile]
    if coords then
        tex:SetTexture(CLASS_ICON_FILE)
        tex:SetTexCoord(coords[1], coords[2], coords[3], coords[4])
    else
        tex:SetTexture("Interface\\Icons\\INV_Misc_QuestionMark")
        tex:SetTexCoord(0.08, 0.92, 0.08, 0.92)
    end
end

function W.SetRowSelected(row, selected)
    if selected then
        W.Skin(row, C.rowSel, C.accent)
    else
        W.Skin(row, C.row, C.border)
    end
    if row.stripe then
        if selected then row.stripe:Show() else row.stripe:Hide() end
    end
end

function W.ClassFileOf(unit)
    if not unit then return nil end
    local _, classFile = UnitClass(unit)
    return classFile
end

SetStatus = function(text)
    if ui.status then ui.status:SetText(text or "") end
end

function W.Tip(owner, title, body)
    GameTooltip:SetOwner(owner, "ANCHOR_RIGHT")
    GameTooltip:SetText(title or "", 1, 1, 1)
    if body and body ~= "" then
        GameTooltip:AddLine(body, C.muted[1] + 0.2, C.muted[2] + 0.2, C.muted[3] + 0.2, true)
    end
    GameTooltip:Show()
end

function W.ShowItemTip(owner, entry, extra)
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
    if extra then GameTooltip:AddLine(extra, C.accent[1], C.accent[2], C.accent[3], true) end
    GameTooltip:Show()
end

function W.ClassColorText(unit, name)
    local _, classFile = UnitClass(unit or "")
    local c = classFile and CLASS_COLORS[classFile]
    if c then
        return string.format("|cff%02x%02x%02x%s|r",
            math.floor(c.r * 255 + 0.5), math.floor(c.g * 255 + 0.5), math.floor(c.b * 255 + 0.5), name or "?")
    end
    return name or "?"
end

function W.HidePopup()
    if popup then popup:Hide() end
end

function W.ShowPopup(anchor, entries)
    if not popup then
        popup = CreateFrame("Frame", "AltBotsPopup", UIParent)
        W.Skin(popup, C.header, C.borderHi)
        popup:SetFrameStrata("DIALOG")
        popup:SetClampedToScreen(true)
        popup.buttons = {}
    end
    for _, b in ipairs(popup.buttons) do b:Hide() end
    local width = math.max(180, math.floor(anchor:GetWidth() or 180))
    local y = -4
    for i, entry in ipairs(entries) do
        local b = popup.buttons[i]
        if not b then
            b = CreateFrame("Button", nil, popup)
            b:SetHeight(20)
            b.text = b:CreateFontString(nil, "OVERLAY", "AltBotsFontBody")
            b.text:SetPoint("LEFT", 8, 0)
            b.text:SetJustifyH("LEFT")
            local hl = b:CreateTexture(nil, "HIGHLIGHT")
            hl:SetAllPoints()
            W.Paint(hl, C.accent, 0.22)
            popup.buttons[i] = b
        end
        b:SetWidth(width - 8)
        b:ClearAllPoints()
        b:SetPoint("TOPLEFT", 4, y)
        b.text:SetWidth(width - 20)
        b.text:SetText(entry.text)
        b:SetScript("OnClick", function()
            W.HidePopup()
            if entry.func then entry.func() end
        end)
        if entry.disabled then
            b:Disable(); W.TextColor(b.text, C.faint)
        else
            b:Enable(); W.TextColor(b.text, C.text)
        end
        b:Show()
        y = y - 20
    end
    popup:SetWidth(width)
    popup:SetHeight((-y) + 4)
    popup:ClearAllPoints()
    popup:SetPoint("TOPLEFT", anchor, "BOTTOMLEFT", 0, -2)
    popup:Show()
end

function W.SetBtnActive(b, active)
    b.active = active and true or nil
    if active then
        W.Skin(b, C.btnOn, C.accent)
    else
        W.Skin(b, C.btn, C.border)
    end
end

function W.MakeTextBtn(parent, w, h, text)
    local b = CreateFrame("Button", nil, parent)
    b:SetWidth(w); b:SetHeight(h or 22)
    W.Skin(b, C.btn, C.border)
    local fs = b:CreateFontString(nil, "OVERLAY", "AltBotsFontButton")
    fs:SetPoint("LEFT", 4, 0)
    fs:SetPoint("RIGHT", -4, 0)
    b:SetFontString(fs)
    b:SetNormalFontObject(F.button)
    b:SetHighlightFontObject(F.button)
    b:SetDisabledFontObject(F.buttonOff)
    W.AddHover(b, 0.10)
    b:SetText(text or "")
    b.SetActive = W.SetBtnActive
    b:SetScript("OnLeave", function() GameTooltip:Hide() end)
    return b
end

function W.HookTip(btn, title, body)
    btn:SetScript("OnEnter", function(self) W.Tip(self, title, body) end)
    btn:SetScript("OnLeave", function() GameTooltip:Hide() end)
end

function W.MakeIconBtn(parent, size, texture, title, body, onClick)
    local b = CreateFrame("Button", nil, parent)
    b:SetWidth(size); b:SetHeight(size)
    W.Skin(b, C.btn, C.border)
    local icon = b:CreateTexture(nil, "ARTWORK")
    icon:SetPoint("TOPLEFT", 2, -2)
    icon:SetPoint("BOTTOMRIGHT", -2, 2)
    icon:SetTexture(texture)
    icon:SetTexCoord(0.08, 0.92, 0.08, 0.92)
    b.icon = icon
    W.AddHover(b, 0.15)
    b:SetScript("OnEnter", function(self)
        self:SetBackdropBorderColor(C.accent[1], C.accent[2], C.accent[3], 1)
        W.Tip(self, title, body)
    end)
    b:SetScript("OnLeave", function(self)
        self:SetBackdropBorderColor(C.border[1], C.border[2], C.border[3], 1)
        GameTooltip:Hide()
    end)
    b:RegisterForClicks("LeftButtonUp", "RightButtonUp")
    b:SetScript("OnClick", onClick)
    return b
end

function W.MakeActionTile(parent, action, onClick)
    local b = CreateFrame("Button", nil, parent)
    b:SetWidth(70); b:SetHeight(58)
    W.Skin(b, C.btn, C.border)
    W.AddHover(b, 0.07)
    b.icon = b:CreateTexture(nil, "ARTWORK")
    b.icon:SetWidth(28); b.icon:SetHeight(28)
    b.icon:SetPoint("TOP", 0, -7)
    b.icon:SetTexture(action.icon)
    b.icon:SetTexCoord(0.08, 0.92, 0.08, 0.92)
    b.label = b:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    b.label:SetPoint("BOTTOM", 0, 6)
    b.label:SetText(action.label)
    W.TextColor(b.label, C.text)
    b:SetScript("OnEnter", function(self)
        self:SetBackdropBorderColor(C.accent[1], C.accent[2], C.accent[3], 1)
        W.Tip(self, action.label, action.tip)
    end)
    b:SetScript("OnLeave", function(self)
        self:SetBackdropBorderColor(C.border[1], C.border[2], C.border[3], 1)
        GameTooltip:Hide()
    end)
    b:SetScript("OnMouseDown", function(self) self.icon:SetPoint("TOP", 1, -8) end)
    b:SetScript("OnMouseUp", function(self) self.icon:SetPoint("TOP", 0, -7) end)
    b:SetScript("OnClick", onClick)
    return b
end

function W.MakeItemButton(parent, size)
    local b = CreateFrame("Button", nil, parent)
    b:SetWidth(size); b:SetHeight(size)
    W.Skin(b, C.row, C.border)
    b.icon = b:CreateTexture(nil, "ARTWORK")
    b.icon:SetPoint("TOPLEFT", 2, -2)
    b.icon:SetPoint("BOTTOMRIGHT", -2, 2)
    b.icon:SetTexCoord(0.08, 0.92, 0.08, 0.92)
    W.AddHover(b, 0.15)
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

function W.SetItemIcon(btn, entry, emptyTex)
    if not entry or entry == 0 then
        btn.icon:SetTexture(emptyTex or "Interface\\Buttons\\UI-Slot-Background")
        btn.icon:SetVertexColor(0.40, 0.42, 0.48)
        btn:SetBackdropBorderColor(C.border[1], C.border[2], C.border[3], 1)
        return
    end
    local _, _, quality, _, _, _, _, _, _, infoIcon = GetItemInfo(entry)
    local texture = (GetItemIcon and GetItemIcon(entry)) or infoIcon
    btn.icon:SetTexture(texture or "Interface\\Icons\\INV_Misc_QuestionMark")
    if ItemTooHighLevel(entry) then
        btn.icon:SetVertexColor(1, 0.28, 0.22)
        btn:SetBackdropBorderColor(C.bad[1], C.bad[2], C.bad[3], 1)
        return
    end
    btn.icon:SetVertexColor(1, 1, 1)
    if quality and quality >= 2 and GetItemQualityColor then
        local r, g, b = GetItemQualityColor(quality)
        btn:SetBackdropBorderColor(r, g, b, 1)
    else
        btn:SetBackdropBorderColor(C.border[1], C.border[2], C.border[3], 1)
    end
end

function W.MakeEditBox(parent, w, h)
    local e = CreateFrame("EditBox", nil, parent)
    e:SetWidth(w); e:SetHeight(h or 22)
    W.Skin(e, C.row, C.border)
    e:SetFontObject(F.body)
    e:SetTextInsets(7, 7, 0, 0)
    e:SetAutoFocus(false)
    e:SetScript("OnEscapePressed", function(self) self:ClearFocus() end)
    e:SetScript("OnEnterPressed", function(self) self:ClearFocus() end)
    e:SetScript("OnEditFocusGained", function(self)
        self:SetBackdropBorderColor(C.accent[1], C.accent[2], C.accent[3], 1)
        if self.placeholder then self.placeholder:Hide() end
    end)
    e:SetScript("OnEditFocusLost", function(self)
        self:SetBackdropBorderColor(C.border[1], C.border[2], C.border[3], 1)
        if self.placeholder and self:GetText() == "" then self.placeholder:Show() end
        if self.onCommit then self.onCommit(self) end
    end)
    return e
end

function W.SkinScroll(sf)
    local name = sf:GetName()
    local bar = name and _G[name .. "ScrollBar"]
    if not bar then return end
    local up, down = _G[name .. "ScrollBarScrollUpButton"], _G[name .. "ScrollBarScrollDownButton"]
    if up then up:Hide(); up:SetAlpha(0) end
    if down then down:Hide(); down:SetAlpha(0) end
    bar:ClearAllPoints()
    bar:SetPoint("TOPLEFT", sf, "TOPRIGHT", 8, 0)
    bar:SetPoint("BOTTOMLEFT", sf, "BOTTOMRIGHT", 8, 0)
    bar:SetWidth(6)
    local track = bar:CreateTexture(nil, "BACKGROUND")
    track:SetAllPoints()
    W.Paint(track, C.track)
    local thumb = bar:GetThumbTexture()
    if thumb then
        thumb:SetTexture(C.borderHi[1], C.borderHi[2], C.borderHi[3], 1)
        thumb:SetWidth(6)
        thumb:SetHeight(32)
    end
end

function W.MakeScroll(name, parent, childW)
    local sf = CreateFrame("ScrollFrame", name, parent, "UIPanelScrollFrameTemplate")
    W.SkinScroll(sf)
    local child = CreateFrame("Frame", nil, sf)
    child:SetWidth(childW); child:SetHeight(1)
    sf:SetScrollChild(child)
    return sf, child
end

function W.MakeBar(parent, height)
    local bar = CreateFrame("StatusBar", nil, parent)
    bar:SetHeight(height)
    bar:SetStatusBarTexture(FLAT)
    bar:SetMinMaxValues(0, 1)
    bar:SetValue(0)
    bar.bg = bar:CreateTexture(nil, "BACKGROUND")
    bar.bg:SetAllPoints()
    W.Paint(bar.bg, C.track)
    return bar
end

function W.SetTabState(tab, selected)
    if selected then
        tab:Disable()
        W.TextColor(tab.label, C.text)
        tab.underline:Show()
    else
        tab:Enable()
        W.TextColor(tab.label, C.muted)
        tab.underline:Hide()
    end
end

function W.MakeTab(parent, label)
    local b = CreateFrame("Button", nil, parent)
    b:SetHeight(30)
    b.label = b:CreateFontString(nil, "OVERLAY", "AltBotsFontBig")
    b.label:SetPoint("CENTER", 0, 1)
    b.label:SetText(label)
    b.badge = b:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    b.badge:SetPoint("LEFT", b.label, "RIGHT", 5, 0)
    b.badge:SetText("")
    b:SetWidth(math.max(70, math.floor((b.label:GetStringWidth() or 40) + 44)))
    b.underline = b:CreateTexture(nil, "OVERLAY")
    b.underline:SetHeight(2)
    b.underline:SetPoint("BOTTOMLEFT", 8, 0)
    b.underline:SetPoint("BOTTOMRIGHT", -8, 0)
    W.Paint(b.underline, C.accent)
    local hl = b:CreateTexture(nil, "HIGHLIGHT")
    hl:SetHeight(2)
    hl:SetPoint("BOTTOMLEFT", 8, 0)
    hl:SetPoint("BOTTOMRIGHT", -8, 0)
    W.Paint(hl, C.borderHi)
    W.SetTabState(b, false)
    return b
end

function W.SectionHeader(parent, text)
    local fs = parent:CreateFontString(nil, "OVERLAY", "AltBotsFontCaps")
    fs:SetText(text)
    local rule = parent:CreateTexture(nil, "ARTWORK")
    rule:SetHeight(1)
    W.Paint(rule, C.border)
    return fs, rule
end

function W.MakePanel(parent)
    local p = CreateFrame("Frame", nil, parent)
    W.Skin(p, C.panel, C.border)
    return p
end

function W.EmptyText(parent, template)
    local fs = parent:CreateFontString(nil, "OVERLAY", template or "AltBotsFontSmall")
    fs:SetJustifyH("CENTER")
    fs:SetSpacing(3)
    return fs
end

-- Every order button (Commands tab tiles, quick bar, key bindings) uses this list.
local ORDER_ACTIONS = {
    { verb = "follow", icon = "Interface\\Icons\\Ability_Hunter_Pet_Follow", label = "Follow",
        tip = "Follow you." },
    { verb = "stay", icon = "Interface\\Icons\\Spell_Nature_TimeStop", label = "Stay",
        tip = "Hold position." },
    { verb = "attack", icon = "Interface\\Icons\\Ability_Warrior_Charge", label = "Attack",
        tip = "Attack the nearest enemy." },
    { verb = "stopattack", icon = "Interface\\Icons\\Ability_Warrior_Disarm", label = "Stop attack",
        tip = "Stop attacking." },
    { verb = "pull", icon = "Interface\\Icons\\Ability_Hunter_SniperShot", label = "Pull",
        tip = "Pull your current target. Needs a living mob and the bot in your group." },
    { verb = "farm", icon = "Interface\\Icons\\INV_Misc_Bone_HumanSkull_01", label = "Farm",
        tip = "Farm your current target within the farm radius." },
    { verb = "farmstop", icon = "Interface\\Icons\\Spell_Shadow_SacrificialShield", label = "Stop farm",
        tip = "Leave farm mode and return to follow." },
    { verb = "collect", icon = "Interface\\Icons\\INV_Misc_Bag_CenarionHerbBag", label = "Collect",
        tip = "Open the nearest worldforged pickup (or other chest) within 50 yards of you, take what is inside "
            .. "and come back. Each alt picks one it has not looted yet." },
    { verb = "inventory", icon = "Interface\\Icons\\INV_Misc_Bag_10", label = "Clean bags",
        tip = "Sell greys and outgrown whites and greens. Works immediately, or when the bot is near a vendor." },
}

function W.RunOrder(verb, choice, radius)
    if verb == "farm" then
        Order("farm", choice or ALL_GROUPED, radius or AltBotsDB.farmRadius or 40)
    else
        Order(verb, choice or ALL_GROUPED)
    end
end

function W.SavePosition(f, key)
    local point, _, relPoint, x, y = f:GetPoint(1)
    if point then AltBotsDB[key] = { point, relPoint, x, y } end
end

function W.RestorePosition(f, key, fallback)
    local pos = AltBotsDB[key]
    f:ClearAllPoints()
    if type(pos) == "table" and pos[1] then
        f:SetPoint(pos[1], UIParent, pos[2] or pos[1], tonumber(pos[3]) or 0, tonumber(pos[4]) or 0)
    else
        f:SetPoint(fallback[1], UIParent, fallback[2], fallback[3], fallback[4])
    end
end

function W.ApplyScale()
    local s = tonumber(AltBotsDB.scale) or 1
    if win then win:SetScale(s) end
    if quickBar then quickBar:SetScale(s) end
end

function W.GetQuickBarShown()
    return AltBotsDB.quickBar and true or false
end

local SetQuickBarShown

function W.CreateQuickBar()
    if quickBar then return end
    local size, gap = 30, 4
    local b = CreateFrame("Frame", "AltBotsQuickBar", UIParent)
    b:SetHeight(size + 10)
    b:SetWidth(18 + #ORDER_ACTIONS * (size + gap) + 4)
    W.Skin(b, C.window, C.border)
    b:SetFrameStrata("MEDIUM")
    b:SetMovable(true)
    b:SetClampedToScreen(true)
    W.RestorePosition(b, "quickBarPos", { "BOTTOM", "BOTTOM", 0, 160 })

    local grip = CreateFrame("Button", nil, b)
    grip:SetWidth(12)
    grip:SetPoint("TOPLEFT", 3, -3)
    grip:SetPoint("BOTTOMLEFT", 3, 3)
    local gripTex = grip:CreateTexture(nil, "ARTWORK")
    gripTex:SetWidth(2)
    gripTex:SetPoint("TOP", 0, -4)
    gripTex:SetPoint("BOTTOM", 0, 4)
    W.Paint(gripTex, C.accent)
    W.AddHover(grip, 0.10)
    grip:RegisterForDrag("LeftButton")
    grip:RegisterForClicks("RightButtonUp")
    grip:SetScript("OnDragStart", function() b:StartMoving() end)
    grip:SetScript("OnDragStop", function()
        b:StopMovingOrSizing()
        W.SavePosition(b, "quickBarPos")
    end)
    grip:SetScript("OnClick", function() SetQuickBarShown(false) end)
    W.HookTip(grip, "AltBots quick bar", "Orders for every alt in your group. Drag to move, right-click to hide. "
        .. "Type /alt bar to show it again.")

    b.buttons = {}
    for i, action in ipairs(ORDER_ACTIONS) do
        local verb = action.verb
        local btn = W.MakeIconBtn(b, size, action.icon, action.label .. " (all grouped alts)", action.tip, function()
            W.RunOrder(verb, ALL_GROUPED)
        end)
        btn:SetPoint("LEFT", 18 + (i - 1) * (size + gap), 0)
        b.buttons[i] = btn
    end
    quickBar = b
    W.ApplyScale()
    b:Hide()
end

SetQuickBarShown = function(shown)
    AltBotsDB.quickBar = shown and true or nil
    if shown then
        W.CreateQuickBar()
        quickBar:Show()
    elseif quickBar then
        quickBar:Hide()
    end
    if ui.quickBarBtn then ui.quickBarBtn:SetActive(shown) end
end

function W.CreateMinimapButton()
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
    b:RegisterForClicks("LeftButtonUp", "RightButtonUp")
    b:SetScript("OnDragStart", function(self)
        self:SetScript("OnUpdate", function()
            local mx, my = Minimap:GetCenter()
            local x, y = GetCursorPosition()
            local scale = Minimap:GetEffectiveScale()
            x, y = x / scale, y / scale
            AltBotsDB.minimapAngle = math.deg(math.atan2(y - my, x - mx))
            Place()
        end)
    end)
    b:SetScript("OnDragStop", function(self) self:SetScript("OnUpdate", nil) end)
    b:SetScript("OnClick", function(_, button)
        if button == "RightButton" then
            SetQuickBarShown(not W.GetQuickBarShown())
            return
        end
        EnsureWindow()
        if win:IsShown() then win:Hide() else win:Show(); ShowTab(ui.activeTab or "alts") end
    end)
    b:SetScript("OnEnter", function(self)
        W.Tip(self, "AltBots", "Left-click: open the window.\nRight-click: show or hide the quick bar.\nDrag: move this button."
            .. "\n/alt toggles the window, /alt minimap hides this button.")
    end)
    b:SetScript("OnLeave", function() GameTooltip:Hide() end)
    b.Place = Place
    minimapBtn = b
    if AltBotsDB.minimapHidden then b:Hide() else Place(); b:Show() end
end

function W.GetMinimapShown()
    return not AltBotsDB.minimapHidden
end

function W.SetMinimapShown(shown)
    AltBotsDB.minimapHidden = not shown
    if shown then
        W.CreateMinimapButton()
        if minimapBtn then minimapBtn.Place(); minimapBtn:Show() end
    elseif minimapBtn then
        minimapBtn:Hide()
    end
end

-- Widget pools live under the window and are never reparented to nil.
function W.InitPools()
    ui.pools = ui.pools or {
        altRows = {}, cmdRows = {}, partyRows = {},
        gearSlots = {}, bagSlots = {}, profBtns = {},
        roleBtns = {}, specBtns = {},
        prefArm = {}, prefWep = {},
        questListRows = {}, questRows = {}, questObjRows = {},
        ruleRows = {},
    }
end

function W.HidePool(list)
    for _, w in ipairs(list or {}) do w:Hide() end
end

function W.TakeRow(pool, parent, maker)
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

function W.StyleRowButton(b, w, h)
    b:SetWidth(w); b:SetHeight(h)
    W.Skin(b, C.row, C.border)
end

function W.AddStripe(row)
    row.stripe = row:CreateTexture(nil, "OVERLAY")
    row.stripe:SetWidth(2)
    row.stripe:SetPoint("TOPLEFT", 1, -1)
    row.stripe:SetPoint("BOTTOMLEFT", 1, 1)
    W.Paint(row.stripe, C.accent)
    row.stripe:Hide()
end

function W.MakeAltRow(parent)
    local row = CreateFrame("Frame", nil, parent)
    W.StyleRowButton(row, ROW_W, 34)
    W.AddStripe(row)
    row.classIcon = row:CreateTexture(nil, "ARTWORK")
    row.classIcon:SetWidth(22); row.classIcon:SetHeight(22)
    row.classIcon:SetPoint("LEFT", 8, 0)
    row.name = row:CreateFontString(nil, "OVERLAY", "AltBotsFontBody")
    row.name:SetPoint("LEFT", row.classIcon, "RIGHT", 10, 0)
    row.name:SetJustifyH("LEFT")
    row.name:SetWidth(170)
    row.dot = row:CreateTexture(nil, "OVERLAY")
    row.dot:SetWidth(6); row.dot:SetHeight(6)
    row.dot:SetPoint("LEFT", 216, 0)
    row.status = row:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    row.status:SetPoint("LEFT", row.dot, "RIGHT", 7, 0)
    row.status:SetWidth(120)
    row.status:SetJustifyH("LEFT")
    row.remove = W.MakeTextBtn(row, 22, 22, "x")
    row.remove:SetPoint("RIGHT", -6, 0)
    row.invite = W.MakeTextBtn(row, 70, 22, "Invite")
    row.invite:SetPoint("RIGHT", row.remove, "LEFT", -4, 0)
    row.quest = W.MakeTextBtn(row, 70, 22, "Quest solo")
    row.quest:SetPoint("RIGHT", row.invite, "LEFT", -4, 0)
    row.spawn = W.MakeTextBtn(row, 70, 22, "Spawn")
    row.spawn:SetPoint("RIGHT", row.quest, "LEFT", -4, 0)
    return row
end

function W.MakeCmdRow(parent)
    local row = CreateFrame("Button", nil, parent)
    W.StyleRowButton(row, ROW_W, 40)
    W.AddStripe(row)
    W.AddHover(row, 0.04)
    row.classIcon = row:CreateTexture(nil, "ARTWORK")
    row.classIcon:SetWidth(26); row.classIcon:SetHeight(26)
    row.classIcon:SetPoint("LEFT", 8, 0)
    row.name = row:CreateFontString(nil, "OVERLAY", "AltBotsFontBody")
    row.name:SetPoint("TOPLEFT", row.classIcon, "TOPRIGHT", 10, 1)
    row.name:SetJustifyH("LEFT")
    row.name:SetWidth(170)
    row.sub = row:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    row.sub:SetPoint("BOTTOMLEFT", row.classIcon, "BOTTOMRIGHT", 10, -1)
    row.sub:SetJustifyH("LEFT")
    row.sub:SetWidth(170)
    row.hp = W.MakeBar(row, 11)
    row.hp:SetPoint("TOPLEFT", 232, -9)
    row.hp:SetPoint("TOPRIGHT", -120, -9)
    row.hp:SetStatusBarColor(C.good[1], C.good[2], C.good[3])
    row.hpText = row.hp:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    row.hpText:SetPoint("RIGHT", -4, 0)
    W.TextColor(row.hpText, C.text)
    row.mp = W.MakeBar(row, 5)
    row.mp:SetPoint("TOPLEFT", row.hp, "BOTTOMLEFT", 0, -4)
    row.mp:SetPoint("TOPRIGHT", row.hp, "BOTTOMRIGHT", 0, -4)
    row.dot = row:CreateTexture(nil, "OVERLAY")
    row.dot:SetWidth(6); row.dot:SetHeight(6)
    row.dot:SetPoint("RIGHT", -100, 0)
    row.state = row:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    row.state:SetPoint("LEFT", row.dot, "RIGHT", 7, 0)
    row.state:SetWidth(86)
    row.state:SetJustifyH("LEFT")
    row.pick = row
    return row
end

function W.MakePartyRow(parent)
    local row = CreateFrame("Button", nil, parent)
    W.StyleRowButton(row, 168, 30)
    W.AddStripe(row)
    W.AddHover(row, 0.05)
    row.classIcon = row:CreateTexture(nil, "ARTWORK")
    row.classIcon:SetWidth(18); row.classIcon:SetHeight(18)
    row.classIcon:SetPoint("LEFT", 7, 0)
    row.dot = row:CreateTexture(nil, "OVERLAY")
    row.dot:SetWidth(5); row.dot:SetHeight(5)
    row.dot:SetPoint("BOTTOMRIGHT", row.classIcon, "BOTTOMRIGHT", 2, -2)
    row.name = row:CreateFontString(nil, "OVERLAY", "AltBotsFontBody")
    row.name:SetPoint("LEFT", row.classIcon, "RIGHT", 7, 0)
    row.name:SetJustifyH("LEFT")
    row.name:SetWidth(96)
    row.info = row:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    row.info:SetPoint("RIGHT", -7, 0)
    row.info:SetJustifyH("RIGHT")
    return row
end

function W.MakeSidebar(parent, title, emptyText)
    local left = W.MakePanel(parent)
    left:SetPoint("TOPLEFT", 0, 0)
    left:SetPoint("BOTTOMLEFT", 0, 0)
    left:SetWidth(190)
    local h, r = W.SectionHeader(left, title)
    h:SetPoint("TOPLEFT", 10, -9)
    r:SetPoint("LEFT", h, "RIGHT", 8, 0)
    r:SetPoint("RIGHT", -10, 0)
    local empty = left:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    empty:SetPoint("TOPLEFT", 10, -32)
    empty:SetPoint("TOPRIGHT", -10, -32)
    empty:SetJustifyH("LEFT")
    empty:SetSpacing(2)
    empty:SetText(emptyText)
    return left, empty
end

function W.AltName(name, info)
    local c = info and info.classFile and CLASS_COLORS[info.classFile]
    local text = name
    if c then
        text = string.format("|cff%02x%02x%02x%s|r",
            math.floor(c.r * 255 + 0.5), math.floor(c.g * 255 + 0.5), math.floor(c.b * 255 + 0.5), name)
    end
    if info and info.level and info.level > 0 then
        text = text .. "  " .. W.Hex(C.faint) .. info.level .. "|r"
    end
    return text
end

function W.UpdateHeaderSummary()
    if not ui.summary then return end
    local grouped = GroupedNames()
    local inGroup, solo, total = 0, 0, 0
    for _, n in ipairs(SortedAltNames()) do
        total = total + 1
        if grouped[n] then inGroup = inGroup + 1 elseif spawned[n] then solo = solo + 1 end
    end
    ui.summary:SetText(W.Hex(C.good) .. inGroup .. "|r in group   " .. W.Hex(C.warn) .. solo .. "|r solo   "
        .. W.Hex(C.muted) .. total .. " saved|r")
    if ui.tabs and ui.tabs.commands then
        ui.tabs.commands.badge:SetText(inGroup > 0 and tostring(inGroup) or "")
    end
end

EnsureWindow = function()
    if win then return end
    W.InitPools()
    win = CreateFrame("Frame", "AltBotsFrame", UIParent)
    win:SetWidth(WIN_W); win:SetHeight(WIN_H)
    W.Skin(win, C.window, C.border)
    win:SetMovable(true)
    win:EnableMouse(true)
    win:SetToplevel(true)
    win:SetFrameStrata("HIGH")
    win:SetClampedToScreen(true)
    W.RestorePosition(win, "winPos", { "CENTER", "CENTER", 0, 0 })
    win:Hide()
    tinsert(UISpecialFrames, "AltBotsFrame")
    W.ApplyScale()

    local header = CreateFrame("Frame", nil, win)
    header:SetPoint("TOPLEFT", 1, -1)
    header:SetPoint("TOPRIGHT", -1, -1)
    header:SetHeight(44)
    header:EnableMouse(true)
    header:RegisterForDrag("LeftButton")
    header:SetScript("OnDragStart", function() win:StartMoving() end)
    header:SetScript("OnDragStop", function()
        win:StopMovingOrSizing()
        W.SavePosition(win, "winPos")
    end)
    local headerBg = header:CreateTexture(nil, "BACKGROUND")
    headerBg:SetAllPoints()
    W.Paint(headerBg, C.header)
    local accentLine = header:CreateTexture(nil, "ARTWORK")
    accentLine:SetHeight(2)
    accentLine:SetPoint("TOPLEFT")
    accentLine:SetPoint("TOPRIGHT")
    W.Paint(accentLine, C.accent)

    local logo = header:CreateTexture(nil, "ARTWORK")
    logo:SetWidth(26); logo:SetHeight(26)
    logo:SetPoint("LEFT", 12, -1)
    logo:SetTexture("Interface\\Icons\\INV_Misc_GroupNeedMore")
    logo:SetTexCoord(0.08, 0.92, 0.08, 0.92)

    local title = header:CreateFontString(nil, "OVERLAY", "AltBotsFontTitle")
    title:SetPoint("TOPLEFT", logo, "TOPRIGHT", 10, 1)
    title:SetText("AltBots")
    local subtitle = header:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    subtitle:SetPoint("BOTTOMLEFT", logo, "BOTTOMRIGHT", 10, -1)
    subtitle:SetText("Companion alts")

    local close = CreateFrame("Button", nil, header)
    close:SetWidth(26); close:SetHeight(26)
    close:SetPoint("RIGHT", -10, -1)
    close.text = close:CreateFontString(nil, "OVERLAY", "AltBotsFontTitle")
    close.text:SetPoint("CENTER", 0, 0)
    close.text:SetText("x")
    W.TextColor(close.text, C.muted)
    local closeHl = close:CreateTexture(nil, "HIGHLIGHT")
    closeHl:SetAllPoints()
    W.Paint(closeHl, C.bad, 0.25)
    close:SetScript("OnClick", function() W.HidePopup(); win:Hide() end)
    close:SetScript("OnEnter", function(self) W.TextColor(self.text, C.text); W.Tip(self, "Close", "Esc or /alt also closes it.") end)
    close:SetScript("OnLeave", function(self) W.TextColor(self.text, C.muted); GameTooltip:Hide() end)

    ui.quickBarBtn = W.MakeTextBtn(header, 84, 24, "Quick bar")
    ui.quickBarBtn:SetPoint("RIGHT", close, "LEFT", -8, 0)
    ui.quickBarBtn:SetActive(W.GetQuickBarShown())
    W.HookTip(ui.quickBarBtn, "Quick bar", "A small movable bar with every order for all your grouped alts, so you can "
        .. "command them with this window closed. Also /alt bar, or right-click the minimap button.")
    ui.quickBarBtn:SetScript("OnClick", function() SetQuickBarShown(not W.GetQuickBarShown()) end)

    ui.summary = header:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    ui.summary:SetPoint("RIGHT", ui.quickBarBtn, "LEFT", -14, 0)
    ui.summary:SetJustifyH("RIGHT")

    local nav = CreateFrame("Frame", nil, win)
    nav:SetPoint("TOPLEFT", header, "BOTTOMLEFT", 0, 0)
    nav:SetPoint("TOPRIGHT", header, "BOTTOMRIGHT", 0, 0)
    nav:SetHeight(30)
    local navRule = nav:CreateTexture(nil, "ARTWORK")
    navRule:SetHeight(1)
    navRule:SetPoint("BOTTOMLEFT")
    navRule:SetPoint("BOTTOMRIGHT")
    W.Paint(navRule, C.border)

    ui.tabs = {}
    local prevTab
    for _, info in ipairs({
        { id = "alts", label = "Alts", tip = "Spawn, invite or despawn your saved alts." },
        { id = "commands", label = "Commands", tip = "Orders, plus live health, mana and range of grouped alts." },
        { id = "party", label = "Party", tip = "Role, spec, gear, professions and bags of grouped bots." },
        { id = "quests", label = "Quests", tip = "Each grouped bot's quest log, or all of them side by side." },
        { id = "rules", label = "Rules", tip = "Standing rules: your alts keep mailing or banking the items you list while a rule is on." },
    }) do
        local t = W.MakeTab(nav, info.label)
        if prevTab then t:SetPoint("LEFT", prevTab, "RIGHT", 2, 0) else t:SetPoint("BOTTOMLEFT", 8, 0) end
        t:SetScript("OnClick", function() ShowTab(info.id) end)
        W.HookTip(t, info.label, info.tip)
        ui.tabs[info.id] = t
        prevTab = t
    end

    local body = CreateFrame("Frame", nil, win)
    body:SetPoint("TOPLEFT", nav, "BOTTOMLEFT", 12, -10)
    body:SetPoint("BOTTOMRIGHT", win, "BOTTOMRIGHT", -12, 32)
    ui.body = body

    local footer = CreateFrame("Frame", nil, win)
    footer:SetPoint("BOTTOMLEFT", 1, 1)
    footer:SetPoint("BOTTOMRIGHT", -1, 1)
    footer:SetHeight(24)
    local footerBg = footer:CreateTexture(nil, "BACKGROUND")
    footerBg:SetAllPoints()
    W.Paint(footerBg, C.header)
    local footerRule = footer:CreateTexture(nil, "ARTWORK")
    footerRule:SetHeight(1)
    footerRule:SetPoint("TOPLEFT")
    footerRule:SetPoint("TOPRIGHT")
    W.Paint(footerRule, C.border)
    ui.status = footer:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    ui.status:SetPoint("LEFT", 12, 0)
    ui.status:SetPoint("RIGHT", -120, 0)
    ui.status:SetJustifyH("LEFT")
    ui.status:SetText("Ready.")
    local version = footer:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    version:SetPoint("RIGHT", -12, 0)
    W.TextColor(version, C.faint)
    version:SetText("v" .. (GetAddOnMetadata("AltBots", "Version") or "?") .. "  |  /alt help")

    -- Alts pane
    local alts = CreateFrame("Frame", nil, body)
    alts:SetAllPoints()
    ui.altsPane = alts
    local ah, ar = W.SectionHeader(alts, "Your characters")
    ah:SetPoint("TOPLEFT", 0, -2)
    ar:SetPoint("LEFT", ah, "RIGHT", 8, 0)
    ar:SetPoint("RIGHT", 0, 0)
    local hint = alts:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    hint:SetPoint("TOPLEFT", 0, -20)
    hint:SetPoint("TOPRIGHT", 0, -20)
    hint:SetJustifyH("LEFT")
    hint:SetText("")
    ui.altsHint = hint
    ui.altsEmpty = W.EmptyText(alts, "AltBotsFontBody")
    ui.altsEmpty:SetPoint("CENTER", 0, 10)
    W.TextColor(ui.altsEmpty, C.muted)
    ui.altsEmpty:SetText("No other characters saved yet.\nPress Sync, or log into an alt with this addon enabled.")
    ui.spawnAllBtn = W.MakeTextBtn(alts, 140, 24, "Spawn + invite all")
    ui.spawnAllBtn:SetPoint("TOPLEFT", 0, -42)
    ui.spawnAllBtn:SetActive(true)
    W.HookTip(ui.spawnAllBtn, "Spawn + invite all", "Spawn every saved alt and invite them to your group.")
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
    ui.despawnAllBtn = W.MakeTextBtn(alts, 100, 24, "Despawn all")
    ui.despawnAllBtn:SetPoint("LEFT", ui.spawnAllBtn, "RIGHT", 6, 0)
    W.HookTip(ui.despawnAllBtn, "Despawn all", "Send every saved alt away.")
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
    ui.syncBtn = W.MakeTextBtn(alts, 70, 24, "Sync")
    ui.syncBtn:SetPoint("TOPRIGHT", 0, -42)
    W.HookTip(ui.syncBtn, "Sync with the server",
        "Ask the server which characters your account has. Characters that no longer exist are removed "
        .. "and new ones are added, so a character wipe cleans itself up.")
    ui.syncBtn:SetScript("OnClick", function()
        RequestAltSync(false)
        SetStatus("Asking the server for your character list...")
        RefreshAlts()
    end)
    ui.pruneBtn = W.MakeTextBtn(alts, 130, 24, "Remove unverified")
    ui.pruneBtn:SetPoint("RIGHT", ui.syncBtn, "LEFT", -6, 0)
    W.HookTip(ui.pruneBtn, "Remove unverified",
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
    ui.altSearch = W.MakeEditBox(alts, 150, 24)
    ui.altSearch:SetPoint("LEFT", ui.despawnAllBtn, "RIGHT", 12, 0)
    ui.altSearch.placeholder = ui.altSearch:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    ui.altSearch.placeholder:SetPoint("LEFT", 8, 0)
    ui.altSearch.placeholder:SetText("Filter by name or class")
    ui.altSearch:SetScript("OnTextChanged", function() RefreshAlts() end)
    local asf, ac = W.MakeScroll("AltBotsAltsScroll", alts, ROW_W)
    asf:SetPoint("TOPLEFT", 0, -76)
    asf:SetPoint("BOTTOMRIGHT", -18, 0)
    ui.altsChild = ac

    -- Commands pane
    local cmd = CreateFrame("Frame", nil, body)
    cmd:SetAllPoints()
    ui.cmdPane = cmd
    local ch, cr = W.SectionHeader(cmd, "Orders")
    ch:SetPoint("TOPLEFT", 0, -2)
    cr:SetPoint("LEFT", ch, "RIGHT", 8, 0)
    cr:SetPoint("RIGHT", 0, 0)

    local whoLbl = cmd:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    whoLbl:SetPoint("TOPLEFT", 0, -28)
    whoLbl:SetText("Send to")
    ui.whoBtn = W.MakeTextBtn(cmd, 200, 24, "All grouped alts")
    ui.whoBtn:SetPoint("LEFT", whoLbl, "RIGHT", 8, 0)
    W.HookTip(ui.whoBtn, "Who receives the order", "Choose one saved alt, or every known alt currently in your group. "
        .. "Clicking a card below picks that alt too.")
    ui.whoBtn:SetScript("OnClick", function(self)
        local entries = { { text = "All grouped alts", func = function()
            ui.orderChoice = ALL_GROUPED
            self:SetText("All grouped alts")
            RefreshCommands()
        end } }
        for _, n in ipairs(SortedAltNames()) do
            local name = n
            table.insert(entries, { text = W.AltName(name, Alts()[name]), func = function()
                ui.orderChoice = name
                self:SetText(name)
                RefreshCommands()
            end })
        end
        W.ShowPopup(self, entries)
    end)
    ui.orderChoice = ALL_GROUPED

    ui.radius = W.MakeEditBox(cmd, 48, 24)
    ui.radius:SetPoint("TOPRIGHT", 0, -22)
    ui.radius:SetNumeric(true)
    ui.radius:SetMaxLetters(3)
    ui.radius:SetText(tostring(AltBotsDB.farmRadius or 40))
    ui.radius.onCommit = function(self)
        AltBotsDB.farmRadius = ClampRadius(self:GetText())
        self:SetText(tostring(AltBotsDB.farmRadius))
    end
    W.HookTip(ui.radius, "Farm radius", "How far the bot may wander while farming your current target, in yards (10-100).")
    local radiusLbl = cmd:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    radiusLbl:SetPoint("RIGHT", ui.radius, "LEFT", -8, 0)
    radiusLbl:SetText("Farm radius (yd)")

    local function Choice() return ui.orderChoice or ALL_GROUPED end
    local function Radius() return ClampRadius(ui.radius:GetText()) end
    ui.cmdIcons = {}
    for i, action in ipairs(ORDER_ACTIONS) do
        local verb = action.verb
        local b = W.MakeActionTile(cmd, action, function() W.RunOrder(verb, Choice(), Radius()) end)
        b:SetPoint("TOPLEFT", (i - 1) * 74, -56)
        ui.cmdIcons[i] = b
    end

    local matH, matR = W.SectionHeader(cmd, "Materials")
    matH:SetPoint("TOPLEFT", 0, -128)
    matR:SetPoint("LEFT", matH, "RIGHT", 8, 0)
    matR:SetPoint("RIGHT", 0, 0)

    ui.matBox = W.MakeEditBox(cmd, 170, 24)
    ui.matBox:SetPoint("TOPLEFT", 0, -150)
    ui.matBox.placeholder = ui.matBox:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    ui.matBox.placeholder:SetPoint("LEFT", 8, 0)
    ui.matBox.placeholder:SetText("Item ID or shift-click a link")
    W.TextColor(ui.matBox.placeholder, C.faint)
    ui.matBox.onCommit = function(self)
        local id = ItemIdFrom(self:GetText())
        ui.matId = id
        if id then self:SetText(ItemLabel(id)) end
    end
    ui.matBox:SetScript("OnEnter", function(self)
        if ui.matId then W.ShowItemTip(self, ui.matId, "The material for Farm, Mail now and Bank now.")
        else W.Tip(self, "Material", "Type an item ID, shift-click an item link while this box has focus, or use Catalog.") end
    end)
    ui.matBox:SetScript("OnLeave", function() GameTooltip:Hide() end)
    ui.linkBoxes = ui.linkBoxes or {}
    table.insert(ui.linkBoxes, ui.matBox)
    if ChatEdit_InsertLink then
        hooksecurefunc("ChatEdit_InsertLink", function(link)
            if not link then return end
            for _, box in ipairs(ui.linkBoxes) do
                if box:HasFocus() then
                    box:SetText(link)
                    box:ClearFocus()
                    return
                end
            end
        end)
    end

    local function SetMaterial(id)
        ui.matId = id
        ui.matBox:SetText(ItemLabel(id))
        if ui.matBox.placeholder then ui.matBox.placeholder:Hide() end
    end
    ui.SetMaterial = SetMaterial

    local catBtn = W.MakeTextBtn(cmd, 64, 24, "Catalog")
    catBtn:SetPoint("LEFT", ui.matBox, "RIGHT", 6, 0)
    W.HookTip(catBtn, "Gather catalog", "Herbs, ore, cloth, leather and other materials bots can gather or hunt for.")
    local function ShowCategory(anchor, category, page)
        local list = gatherCatalog[category] or {}
        local per = 18
        local first = (page - 1) * per + 1
        local entries = {}
        for i = first, math.min(#list, first + per - 1) do
            local item = list[i]
            table.insert(entries, { text = item.name, func = function() SetMaterial(item.id) end })
        end
        if #list > first + per - 1 then
            table.insert(entries, { text = "|cff33ccffMore...|r", func = function() ShowCategory(anchor, category, page + 1) end })
        end
        if page > 1 then
            table.insert(entries, { text = "|cff33ccffBack|r", func = function() ShowCategory(anchor, category, page - 1) end })
        end
        W.ShowPopup(anchor, entries)
    end
    catBtn:SetScript("OnClick", function(self)
        if #gatherCatalogOrder == 0 then
            Queue({ addon = "GETGATHERCATALOG:0" })
            SetStatus("Loading the gather catalog. Click Catalog again in a moment.")
            return
        end
        local entries = {}
        for _, category in ipairs(gatherCatalogOrder) do
            local c = category
            table.insert(entries, { text = c .. " (" .. #(gatherCatalog[c] or {}) .. ")", func = function() ShowCategory(self, c, 1) end })
        end
        W.ShowPopup(self, entries)
    end)

    ui.matCount = W.MakeEditBox(cmd, 44, 24)
    ui.matCount:SetPoint("LEFT", catBtn, "RIGHT", 6, 0)
    ui.matCount:SetNumeric(true)
    ui.matCount:SetMaxLetters(4)
    ui.matCount:SetText(tostring(AltBotsDB.matCount or 20))
    ui.matCount.onCommit = function(self)
        local n = math.max(1, math.min(1000, tonumber(self:GetText()) or 20))
        AltBotsDB.matCount = n
        self:SetText(tostring(n))
    end
    W.HookTip(ui.matCount, "Count", "How many to farm (1-1000). Mail now and Bank now send every stack the alt carries.")

    local deliverBtn = W.MakeTextBtn(cmd, 86, 24, "")
    deliverBtn:SetPoint("LEFT", ui.matCount, "RIGHT", 6, 0)
    local function ShowDeliver()
        deliverBtn:SetText(MaterialDeliver() == "bank" and "Guild bank" or "Mail to me")
    end
    ShowDeliver()
    deliverBtn:SetScript("OnClick", function()
        AltBotsDB.matDeliver = MaterialDeliver() == "bank" and "mail" or "bank"
        ShowDeliver()
    end)
    W.HookTip(deliverBtn, "Delivery", "Where a farm order sends what it gathers: your mailbox, or the alt's guild bank. Click to switch.")

    local farmMatBtn = W.MakeTextBtn(cmd, 70, 24, "Farm it")
    farmMatBtn:SetPoint("LEFT", deliverBtn, "RIGHT", 6, 0)
    W.HookTip(farmMatBtn, "Farm material", "Send the alt picked in 'Send to' to gather or hunt this material anywhere in the world. "
        .. "It leaves your group, delivers by the chosen way and reports progress in chat.")
    farmMatBtn:SetScript("OnClick", function()
        ui.matBox:ClearFocus(); ui.matCount:ClearFocus()
        FarmMaterial(Choice(), ui.matId, ui.matCount:GetText())
    end)

    local stopMatBtn = W.MakeTextBtn(cmd, 46, 24, "Stop")
    stopMatBtn:SetPoint("LEFT", farmMatBtn, "RIGHT", 6, 0)
    W.HookTip(stopMatBtn, "Stop farm order", "Cancel the material farm order. Invite the alt again from the Alts tab afterwards.")
    stopMatBtn:SetScript("OnClick", function() StopFarmMaterial(Choice()) end)

    local mailNowBtn = W.MakeTextBtn(cmd, 64, 24, "Mail now")
    mailNowBtn:SetPoint("LEFT", stopMatBtn, "RIGHT", 6, 0)
    W.HookTip(mailNowBtn, "Mail now", "Mail every stack of this material the alt carries to you.")
    mailNowBtn:SetScript("OnClick", function()
        ui.matBox:ClearFocus()
        SendMaterial(Choice(), ui.matId, "mail")
    end)

    local bankNowBtn = W.MakeTextBtn(cmd, 64, 24, "Bank now")
    bankNowBtn:SetPoint("LEFT", mailNowBtn, "RIGHT", 6, 0)
    W.HookTip(bankNowBtn, "Bank now", "Put every stack of this material the alt carries in its guild bank.")
    bankNowBtn:SetScript("OnClick", function()
        ui.matBox:ClearFocus()
        SendMaterial(Choice(), ui.matId, "bank")
    end)

    local listH, listR = W.SectionHeader(cmd, "Grouped alts")
    listH:SetPoint("TOPLEFT", 0, -186)
    listR:SetPoint("LEFT", listH, "RIGHT", 8, 0)
    listR:SetPoint("RIGHT", 0, 0)
    ui.cmdEmpty = W.EmptyText(cmd, "AltBotsFontBody")
    ui.cmdEmpty:SetPoint("CENTER", 0, -50)
    W.TextColor(ui.cmdEmpty, C.muted)
    ui.cmdEmpty:SetText("None of your saved alts are in the group yet.\nSpawn and invite them from the Alts tab.")
    local csf, cc = W.MakeScroll("AltBotsCmdScroll", cmd, ROW_W)
    csf:SetPoint("TOPLEFT", 0, -204)
    csf:SetPoint("BOTTOMRIGHT", -18, 0)
    ui.cmdChild = cc

    -- Party pane: member list on the left, details on the right.
    local party = CreateFrame("Frame", nil, body)
    party:SetAllPoints()
    ui.partyPane = party

    local left
    left, ui.partyEmpty = W.MakeSidebar(party, "Party", "Invite a bot to your group to inspect it here.")
    local psf, pc = W.MakeScroll("AltBotsPartyListScroll", left, 168)
    psf:SetPoint("TOPLEFT", 6, -28)
    psf:SetPoint("BOTTOMRIGHT", -16, 6)
    ui.partyListChild = pc

    local right = CreateFrame("Frame", nil, party)
    right:SetPoint("TOPLEFT", left, "TOPRIGHT", 12, 0)
    right:SetPoint("BOTTOMRIGHT", 0, 0)
    ui.partyRight = right

    ui.partyPickHint = W.EmptyText(right, "AltBotsFontBody")
    ui.partyPickHint:SetPoint("CENTER", 0, 20)
    W.TextColor(ui.partyPickHint, C.muted)
    ui.partyPickHint:SetText("Select a party member on the left.")

    ui.detail = CreateFrame("Frame", nil, right)
    ui.detail:SetAllPoints()
    ui.detail:Hide()

    ui.detailName = ui.detail:CreateFontString(nil, "OVERLAY", "AltBotsFontTitle")
    ui.detailName:SetPoint("TOPLEFT", 0, -2)
    ui.detailSub = ui.detail:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    ui.detailSub:SetPoint("TOPLEFT", 0, -22)
    ui.detailSub:SetJustifyH("LEFT")

    ui.inspectBtn = W.MakeTextBtn(ui.detail, 70, 24, "Inspect")
    ui.inspectBtn:SetPoint("TOPRIGHT", -4, -2)
    W.HookTip(ui.inspectBtn, "Inspect", "Open the game's inspect window. You must be within 28 yards.")
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

    local rh, rr = W.SectionHeader(ui.detail, "Role, spec and gear")
    ui.prefHeader, ui.prefRule = rh, rr
    ui.roleLbl = ui.detail:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    ui.roleLbl:SetText("Role")
    ui.roleRow = CreateFrame("Frame", nil, ui.detail)
    ui.roleRow:SetWidth(400); ui.roleRow:SetHeight(22)

    ui.specLbl = ui.detail:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    ui.specLbl:SetText("Spec")
    ui.specRow = CreateFrame("Frame", nil, ui.detail)
    ui.specRow:SetWidth(400); ui.specRow:SetHeight(22)
    ui.specHint = ui.detail:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    W.TextColor(ui.specHint, C.faint)
    ui.specHint:SetText("This class has no addon specs to switch.")

    ui.armorLbl = ui.detail:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    ui.armorLbl:SetText("Armor")
    ui.armorRow = CreateFrame("Frame", nil, ui.detail)
    ui.armorRow:SetWidth(400); ui.armorRow:SetHeight(22)

    ui.weaponLbl = ui.detail:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    ui.weaponLbl:SetText("Weapons")
    ui.weaponRow = CreateFrame("Frame", nil, ui.detail)
    ui.weaponRow:SetWidth(400); ui.weaponRow:SetHeight(22)

    local eh, er = W.SectionHeader(ui.detail, "Equipped")
    ui.equipHeader, ui.equipRule = eh, er
    ui.unlockBtn = W.MakeTextBtn(ui.detail, 100, 20, "Auto-upgrade")
    W.HookTip(ui.unlockBtn, "Auto-upgrade", "Clear locked slots so the bot can choose its own upgrades again.")
    ui.unlockBtn:SetScript("OnClick", function()
        if selectedPartyGuid then
            SendBot("UNLOCK", selectedPartyGuid)
            SetStatus("Unlocking gear on " .. (selectedPartyName or "bot") .. "...")
        end
    end)

    ui.doll = CreateFrame("Frame", nil, ui.detail)
    ui.doll:SetWidth(400); ui.doll:SetHeight(72)

    local ph, pr = W.SectionHeader(ui.detail, "Professions")
    ui.profHeader, ui.profRule = ph, pr
    ui.profRow = CreateFrame("Frame", nil, ui.detail)
    ui.profRow:SetWidth(400); ui.profRow:SetHeight(48)
    ui.profHint = ui.detail:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    W.TextColor(ui.profHint, C.faint)
    ui.profHint:SetText("Profession data has not arrived yet.")

    local bh, br = W.SectionHeader(ui.detail, "Bags")
    ui.bagHeader, ui.bagRule = bh, br

    ui.refreshBtn = W.MakeTextBtn(ui.detail, 80, 20, "Refresh")
    W.HookTip(ui.refreshBtn, "Refresh bags", "Ask this bot for a fresh bag and equipment list.")
    ui.refreshBtn:SetScript("OnClick", function()
        if selectedPartyGuid then
            RequestGearAndBags(selectedPartyGuid, "Refreshing " .. (selectedPartyName or "bot") .. "...")
        else
            SetStatus("Select a party member first.")
        end
    end)

    ui.refreshAllBtn = W.MakeTextBtn(ui.detail, 80, 20, "Refresh all")
    W.HookTip(ui.refreshAllBtn, "Refresh all", "Ask every grouped bot for a fresh bag and equipment list.")
    ui.refreshAllBtn:SetScript("OnClick", RequestAllPartyBags)

    local bsf, bc = W.MakeScroll("AltBotsBagsScroll", ui.detail, 400)
    ui.bagsScroll = bsf
    ui.bagsChild = bc
    ui.bagsEmpty = W.EmptyText(ui.detail)
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

    ui.BagFree = function(guid)
        local total = BagCapacity(guid)
        if total <= 0 then return nil end
        local used = bagsComplete[guid] or OccupiedBagCount(guid)
        return total - used, total
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
        if ui.UpdateDetailSub then ui.UpdateDetailSub() end
    end

    LayoutPartyDetail = function()
        if not ui.detail or not ui.prefHeader then return end
        local paneW = ui.detail:GetWidth() or 0
        if paneW < 100 then paneW = 470 end
        local innerW = paneW - 66
        local y = -46
        local function placeHeader(fs, rule, rightFrame)
            fs:ClearAllPoints()
            fs:SetPoint("TOPLEFT", 0, y)
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
            lbl:SetPoint("TOPLEFT", 0, y - 4)
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

        y = y - 4
        ui.unlockBtn:ClearAllPoints()
        ui.unlockBtn:SetPoint("TOPRIGHT", -4, y + 4)
        placeHeader(ui.equipHeader, ui.equipRule, ui.unlockBtn)
        y = y - 22
        ui.doll:ClearAllPoints()
        ui.doll:SetPoint("TOPLEFT", 0, y)
        ui.doll:SetWidth(paneW - 8)
        ui.doll:SetHeight(72)
        y = y - 80

        placeHeader(ui.profHeader, ui.profRule)
        y = y - 20
        ui.profRow:ClearAllPoints()
        ui.profRow:SetPoint("TOPLEFT", 0, y)
        ui.profRow:SetWidth(paneW - 8)
        ui.profHint:ClearAllPoints()
        ui.profHint:SetPoint("TOPLEFT", ui.profRow, "TOPLEFT", 2, -2)
        y = y - (ui.profRow:GetHeight() or 48) - 10

        ui.refreshAllBtn:ClearAllPoints()
        ui.refreshAllBtn:SetPoint("TOPRIGHT", -4, y + 4)
        ui.refreshBtn:ClearAllPoints()
        ui.refreshBtn:SetPoint("RIGHT", ui.refreshAllBtn, "LEFT", -4, 0)
        placeHeader(ui.bagHeader, ui.bagRule, ui.refreshBtn)
        y = y - 22
        ui.bagsScroll:ClearAllPoints()
        ui.bagsScroll:SetPoint("TOPLEFT", 0, y)
        ui.bagsScroll:SetPoint("BOTTOMRIGHT", -18, 2)
        local scrollW = paneW - 26
        if scrollW < 200 then scrollW = 200 end
        ui.bagsChild:SetWidth(scrollW)

        -- Grow the window so the bag grid always shows at least MIN_BAG_ROWS rows,
        -- capped to the screen.
        local MIN_BAG_ROWS = 4
        local minBagH = MIN_BAG_ROWS * 36 + 8
        local curH = win:GetHeight() or WIN_H
        local detailH = ui.detail:GetHeight() or 0
        if detailH > 0 then
            local detailAtBase = detailH - (curH - WIN_H)
            local bagAtBase = detailAtBase + y - 4
            local wantH = WIN_H + math.max(0, minBagH - bagAtBase)
            local maxH = ((UIParent:GetHeight() or 768) / (win:GetScale() or 1)) - 40
            if wantH > maxH then wantH = maxH end
            if wantH < WIN_H then wantH = WIN_H end
            if math.abs(wantH - curH) >= 1 then win:SetHeight(wantH) end
        end
    end

    BuildQuestPane(body)
    BuildRulesPane(body)
end


function W.AgoText(stamp)
    local secs = time() - (stamp or 0)
    if secs < 60 then return "just now" end
    if secs < 3600 then return math.floor(secs / 60) .. " min ago" end
    if secs < 86400 then return math.floor(secs / 3600) .. " h ago" end
    return math.floor(secs / 86400) .. " days ago"
end

function W.AltsHintText()
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
        return "Synced with your account's character list " .. W.AgoText(data.syncedAt) .. "."
    end
    return "Characters are saved when you log into them. Press Sync to load your account's list from the server."
end

function W.AltMatchesFilter(name, info, filter)
    if not filter or filter == "" then return true end
    if string.find(name:lower(), filter, 1, true) then return true end
    local classFile = info and info.classFile
    return classFile and string.find(classFile:lower(), filter, 1, true) and true or false
end

RefreshAlts = function()
    if not win or not ui.altsChild then return end
    W.HidePool(ui.pools.altRows)
    W.UpdateHeaderSummary()
    local allNames = SortedAltNames()
    local grouped = GroupedNames()
    local filter = ui.altSearch and ui.altSearch:GetText():lower() or ""
    local names = {}
    for _, n in ipairs(allNames) do
        if W.AltMatchesFilter(n, Alts()[n], filter) then table.insert(names, n) end
    end
    if ui.spawnAllBtn then
        if #names == 0 then ui.spawnAllBtn:Disable(); ui.despawnAllBtn:Disable()
        else ui.spawnAllBtn:Enable(); ui.despawnAllBtn:Enable() end
    end
    if ui.altsHint then ui.altsHint:SetText(W.AltsHintText()) end
    if ui.syncBtn then
        if altSync.state == "waiting" then ui.syncBtn:Disable() else ui.syncBtn:Enable() end
    end
    if ui.pruneBtn then
        if StaleCount() > 0 then ui.pruneBtn:Show() else ui.pruneBtn:Hide() end
    end
    if #names == 0 then
        ui.altsEmpty:Show()
        if #allNames > 0 then
            ui.altsEmpty:SetText("No saved character matches that filter.")
        else
            ui.altsEmpty:SetText("No other characters saved yet.\nPress Sync, or log into an alt with this addon enabled.")
        end
        ui.altsChild:SetHeight(1)
        return
    end
    ui.altsEmpty:Hide()
    local y = 0
    for _, name in ipairs(names) do
        local row = W.TakeRow(ui.pools.altRows, ui.altsChild, W.MakeAltRow)
        row:ClearAllPoints()
        row:SetPoint("TOPLEFT", 0, y)
        row:SetWidth(ROW_W)
        local info = Alts()[name]
        row.name:SetText(W.AltName(name, info))
        W.SetClassIcon(row.classIcon, info and info.classFile)
        local inGroup = grouped[name]
        local isOut = spawned[name] or inGroup
        W.TextColor(row.status, C.muted)
        W.SetRowSelected(row, inGroup)
        if inGroup then
            row.status:SetText("In your group")
            W.TextColor(row.status, C.good)
            W.SetDot(row.dot, "green")
        elseif info and info.stale then
            row.status:SetText("Not verified")
            W.TextColor(row.status, C.warn)
            W.SetDot(row.dot, "red")
        elseif spawned[name] then
            row.status:SetText("Questing solo")
            W.TextColor(row.status, C.warn)
            W.SetDot(row.dot, "yellow")
        else
            row.status:SetText("Not spawned")
            W.SetDot(row.dot, "gray")
        end
        row.dot:Show()

        if isOut then
            row.spawn:SetText("Despawn")
            row.spawn:Enable()
            row.spawn:SetScript("OnClick", function() Despawn(name); RefreshAlts() end)
            W.HookTip(row.spawn, "Despawn", "Send " .. name .. " away.")
        else
            row.spawn:SetText("Spawn")
            row.spawn:Enable()
            row.spawn:SetScript("OnClick", function()
                Spawn(name)
                Print("Spawned " .. name .. " without inviting.")
                SetStatus("Spawned " .. name .. ".")
                RefreshAlts()
            end)
            W.HookTip(row.spawn, "Spawn", "Spawn " .. name .. " without inviting it, so you can invite it yourself if you want.")
        end

        if isOut then
            row.quest:Disable()
            W.HookTip(row.quest, "Quest solo", name .. " is already spawned or in your group.")
        else
            row.quest:Enable()
            row.quest:SetScript("OnClick", function()
                Spawn(name)
                Print(name .. " is off questing on its own.")
                SetStatus(name .. " is off questing on its own.")
                RefreshAlts()
            end)
            W.HookTip(row.quest, "Quest solo", "Spawn " .. name .. " without inviting, so it quests on its own.")
        end

        row.remove:SetScript("OnClick", function()
            RemoveAlt(name)
            Print("Removed " .. name .. " from your saved characters.")
            SetStatus("Removed " .. name .. ".")
            RefreshAlts()
        end)
        W.HookTip(row.remove, "Remove " .. name, (info and info.stale)
            and "This character could not be checked since the server looked reset. Remove it if it no longer exists."
            or "Forget this character. Sync or a login on it adds it back.")

        if inGroup then
            row.invite:Disable()
            W.HookTip(row.invite, "Invite", name .. " is already in your group.")
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
            W.HookTip(row.invite, "Invite", spawned[name] and ("Invite " .. name .. ". If it is already in another group, it will leave and join you.")
                or ("Spawn " .. name .. " and invite it. If it is already in another group, it will leave and join you."))
        end
        y = y - 38
    end
    ui.altsChild:SetHeight(math.max(1, -y + 4))
end

function W.GroupUnitsByName()
    local map = {}
    local raid = GetNumRaidMembers()
    if raid > 0 then
        for i = 1, raid do
            local u = "raid" .. i
            local n = UnitName(u)
            if n then map[n] = u end
        end
    else
        for i = 1, GetNumPartyMembers() do
            local u = "party" .. i
            local n = UnitName(u)
            if n then map[n] = u end
        end
    end
    return map
end

function W.ShortNumber(n)
    n = tonumber(n) or 0
    if n >= 1000000 then return string.format("%.1fm", n / 1000000) end
    if n >= 10000 then return string.format("%.0fk", n / 1000) end
    if n >= 1000 then return string.format("%.1fk", n / 1000) end
    return tostring(n)
end

function W.UnitRangeState(unit)
    if not UnitIsConnected(unit) then return "Offline", "gray" end
    if UnitIsDeadOrGhost(unit) then return "Dead", "red" end
    if UnitAffectingCombat(unit) then return "In combat", "red" end
    if CheckInteractDistance(unit, 4) then return "Close", "green" end
    if UnitInRange(unit) then return "In range", "green" end
    if UnitIsVisible(unit) then return "Far", "yellow" end
    return "Out of sight", "yellow"
end

function W.UpdateCmdCard(row)
    local unit = row.unit
    if not unit or not UnitExists(unit) then
        row.hp:SetValue(0); row.mp:SetValue(0)
        row.hpText:SetText("")
        row.state:SetText("Not here")
        W.SetDot(row.dot, "gray")
        return
    end
    local hp, hpMax = UnitHealth(unit) or 0, UnitHealthMax(unit) or 0
    if hpMax <= 0 then hpMax = 1 end
    row.hp:SetMinMaxValues(0, hpMax)
    row.hp:SetValue(hp)
    local pct = hp / hpMax
    if pct < 0.35 then
        row.hp:SetStatusBarColor(C.bad[1], C.bad[2], C.bad[3])
    elseif pct < 0.7 then
        row.hp:SetStatusBarColor(C.warn[1], C.warn[2], C.warn[3])
    else
        row.hp:SetStatusBarColor(C.good[1], C.good[2], C.good[3])
    end
    row.hpText:SetText(W.ShortNumber(hp) .. " / " .. W.ShortNumber(hpMax))
    local mp, mpMax = UnitPower(unit) or 0, UnitPowerMax(unit) or 0
    if mpMax <= 0 then mpMax = 1; mp = 0 end
    row.mp:SetMinMaxValues(0, mpMax)
    row.mp:SetValue(mp)
    local pc = PowerBarColor and PowerBarColor[UnitPowerType(unit)]
    if pc then row.mp:SetStatusBarColor(pc.r, pc.g, pc.b) else row.mp:SetStatusBarColor(0.3, 0.5, 1) end
    local text, dot = W.UnitRangeState(unit)
    row.state:SetText(text)
    W.SetDot(row.dot, dot)
    local level = UnitLevel(unit) or 0
    local className = UnitClass(unit)
    row.sub:SetText((level > 0 and ("Level " .. level) or "Level ??") .. (className and ("  " .. className) or ""))
end

function W.UpdateCmdCards()
    for _, row in ipairs(ui.pools and ui.pools.cmdRows or {}) do
        if row:IsShown() then W.UpdateCmdCard(row) end
    end
end

RefreshCommands = function()
    if not win or not ui.cmdChild then return end
    W.HidePool(ui.pools.cmdRows)
    W.UpdateHeaderSummary()
    local grouped = GroupedNames()
    local units = W.GroupUnitsByName()
    local names = {}
    for _, n in ipairs(SortedAltNames()) do
        if grouped[n] then table.insert(names, n) end
    end
    if ui.orderChoice ~= ALL_GROUPED and not Alts()[ui.orderChoice or ""] then
        ui.orderChoice = ALL_GROUPED
        ui.whoBtn:SetText("All grouped alts")
    end
    if #names == 0 then
        ui.cmdEmpty:Show()
        ui.cmdChild:SetHeight(1)
        return
    end
    ui.cmdEmpty:Hide()
    local y = 0
    for _, name in ipairs(names) do
        local row = W.TakeRow(ui.pools.cmdRows, ui.cmdChild, W.MakeCmdRow)
        row:ClearAllPoints()
        row:SetPoint("TOPLEFT", 0, y)
        row.unit = units[name]
        row.name:SetText(W.ClassColorText(row.unit, name))
        W.SetClassIcon(row.classIcon, W.ClassFileOf(row.unit) or (Alts()[name] and Alts()[name].classFile))
        local picked = ui.orderChoice == name
        W.SetRowSelected(row, picked)
        local who = name
        row:RegisterForClicks("LeftButtonUp", "RightButtonUp")
        row:SetScript("OnClick", function(_, button)
            if button == "RightButton" or ui.orderChoice == who then
                ui.orderChoice = ALL_GROUPED
                ui.whoBtn:SetText("All grouped alts")
                SetStatus("Orders will go to every grouped alt.")
            else
                ui.orderChoice = who
                ui.whoBtn:SetText(who)
                SetStatus("Orders will go to " .. who .. ".")
            end
            RefreshCommands()
        end)
        row:SetScript("OnEnter", function(self)
            W.Tip(self, who, picked and "Orders go to this alt only. Click again or right-click to send to every grouped alt."
                or "Click to send orders to this alt only.")
        end)
        row:SetScript("OnLeave", function() GameTooltip:Hide() end)
        W.UpdateCmdCard(row)
        y = y - 44
    end
    ui.cmdChild:SetHeight(math.max(1, -y + 4))
end

function W.PartyMembers()
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

function W.ConfirmRespec(name, specName, guid, specId)
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

function W.SizePrefBtn(b, text)
    b:SetText(text)
    local fs = b:GetFontString()
    local w = fs and fs:GetStringWidth() or 0
    b:SetWidth(math.max(60, math.floor(w + 18)))
end

function W.FillChoiceRow(row, pool, maker, options, current, onPick, isLegal)
    W.HidePool(pool)
    local x, y = 0, 0
    local rowW = row:GetWidth() or 400
    if rowW < 100 then rowW = 400 end
    for _, opt in ipairs(options) do
        local b = W.TakeRow(pool, row, maker)
        b:ClearAllPoints()
        W.SizePrefBtn(b, opt.label or opt.id)
        local bw = b:GetWidth() or 60
        if x > 0 and (x + bw) > rowW then
            x = 0
            y = y + 22
        end
        b:SetPoint("TOPLEFT", x, -y)
        local id = opt.id
        local isCurrent = current == id
        b:Enable()
        b:SetActive(isCurrent)
        if isLegal and not isLegal(id) then
            b:Disable()
        end
        if isCurrent then
            b:SetScript("OnClick", nil)
        else
            b:SetScript("OnClick", function() onPick(id) end)
        end
        x = x + bw + 4
    end
    row:SetHeight(y + 22)
end

function W.MakePrefBtn(parent)
    return W.MakeTextBtn(parent, 60, 20, "")
end

function W.AvgItemLevel(guid)
    local sum, n = 0, 0
    for slot, info in pairs(gearCache[guid] or {}) do
        if GEAR_SLOT_NAMES[slot] and info.entry and info.entry > 0 and (info.itemLevel or 0) > 0 then
            sum = sum + info.itemLevel
            n = n + 1
        end
    end
    if n == 0 then return nil end
    return math.floor(sum / n + 0.5)
end

ui.UpdateDetailSub = function()
    if not ui.detailSub then return end
    local guid, unit = selectedPartyGuid, selectedPartyUnit
    if not guid then ui.detailSub:SetText(""); return end
    local parts = {}
    local level = unit and UnitExists(unit) and UnitLevel(unit) or 0
    if level > 0 then
        local className = UnitClass(unit)
        table.insert(parts, "Level " .. level .. (className and (" " .. className) or ""))
    end
    local role = currentRoleCache[guid]
    if role and role ~= "auto" then table.insert(parts, ROLE_LABELS[role] or role) end
    local ilvl = W.AvgItemLevel(guid)
    if ilvl then table.insert(parts, "Item level " .. ilvl) end
    local free, total
    if ui.BagFree then free, total = ui.BagFree(guid) end
    if free then table.insert(parts, free .. "/" .. total .. " bag slots free") end
    ui.detailSub:SetText(table.concat(parts, "   " .. W.Hex(C.faint) .. "/|r   "))
end

RefreshParty = function()
    if not win or not ui.partyListChild then return end
    W.HidePool(ui.pools.partyRows)
    local members = W.PartyMembers()
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
    local y = 0
    for _, m in ipairs(members) do
        local row = W.TakeRow(ui.pools.partyRows, ui.partyListChild, W.MakePartyRow)
        row:ClearAllPoints()
        row:SetPoint("TOPLEFT", 0, y)
        row.name:SetText(W.ClassColorText(m.unit, m.name or "?"))
        W.SetClassIcon(row.classIcon, W.ClassFileOf(m.unit))
        W.SetDot(row.dot, UnitIsConnected(m.unit) and "green" or "gray")
        local level = UnitLevel(m.unit) or 0
        local ilvl = m.guid and W.AvgItemLevel(m.guid)
        row.info:SetText(ilvl and (W.Hex(C.accent) .. ilvl .. "|r") or (level > 0 and tostring(level) or ""))
        local guid, name, unit = m.guid, m.name, m.unit
        local isSelected = selectedPartyGuid ~= nil and guid == selectedPartyGuid
        if isSelected then stillThere = true end
        W.SetRowSelected(row, isSelected)
        row:SetScript("OnClick", function()
            selectedPartyGuid, selectedPartyName, selectedPartyUnit = guid, name, unit
            RequestPartyData(guid)
            RefreshParty()
        end)
        row:SetScript("OnEnter", function(self)
            W.Tip(self, name or "?", "Level " .. level .. (ilvl and (", average item level " .. ilvl) or "")
                .. ". The blue number is the average item level once its gear has loaded.")
        end)
        row:SetScript("OnLeave", function() GameTooltip:Hide() end)
        y = y - 34
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
    ui.detailName:SetText(W.ClassColorText(selectedPartyUnit, selectedPartyName or "?"))
    ui.UpdateDetailSub()

    local guid = selectedPartyGuid
    local roles = rolesCache[guid] or {}
    local currentRole = currentRoleCache[guid] or "auto"
    local roleOpts = {}
    for _, id in ipairs(ROLE_ORDER) do
        if id == "auto" or roles[id] then
            table.insert(roleOpts, { id = id, label = ROLE_LABELS[id] or id })
        end
    end
    W.FillChoiceRow(ui.roleRow, ui.pools.roleBtns, W.MakePrefBtn, roleOpts, currentRole, function(id)
        SendBot("SETROLE", guid, id)
        currentRoleCache[guid] = id
        SetStatus((selectedPartyName or "Bot") .. " role set to " .. (ROLE_LABELS[id] or id) .. ".")
        RefreshParty()
    end)

    local specs = specsCache[guid] or {}
    W.HidePool(ui.pools.specBtns)
    if #specs == 0 then
        ui.specHint:Show()
    else
        ui.specHint:Hide()
        local specOpts = {}
        for _, s in ipairs(specs) do
            table.insert(specOpts, { id = tostring(s.specId), label = s.name or ("Spec " .. s.specId) })
        end
        W.FillChoiceRow(ui.specRow, ui.pools.specBtns, W.MakePrefBtn, specOpts, nil, function(id)
            local specName = id
            for _, s in ipairs(specs) do
                if tostring(s.specId) == id then specName = s.name; break end
            end
            W.ConfirmRespec(selectedPartyName, specName, guid, tonumber(id))
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
    W.FillChoiceRow(ui.armorRow, ui.pools.prefArm, W.MakePrefBtn, armorOpts, armorPref, function(id)
        SendBot("SETGEARPREF", guid, "armor:" .. id)
        if gearPrefsCache[guid] then gearPrefsCache[guid].armorPref = id end
        SetStatus("Armor preference: " .. id .. ".")
        RefreshParty()
    end)
    W.FillChoiceRow(ui.weaponRow, ui.pools.prefWep, W.MakePrefBtn, wepOpts, weaponPref, function(id)
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

function W.PlaceDollSlot(slot, col, row)
    local b
    for _, existing in ipairs(ui.pools.gearSlots) do
        if existing.slot == slot then b = existing; break end
    end
    if not b then
        b = W.MakeItemButton(ui.doll, 32)
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
        W.HidePool(ui.pools.gearSlots)
        return
    end
    local guid = selectedPartyGuid
    local gear = gearCache[guid] or {}
    W.HidePool(ui.pools.gearSlots)
    for row, slots in ipairs(PAPERDOLL_ROWS) do
        for i, slot in ipairs(slots) do
            local b = W.PlaceDollSlot(slot, i - 1, row - 1)
            local info = gear[slot]
            local entry = info and info.entry or 0
            W.SetItemIcon(b, entry)
            if b.count then b.count:SetText("") end
            if SlotLocked(guid, slot) then b.lock:Show() else b.lock:Hide() end
            b:SetScript("OnEnter", function(self)
                if entry and entry > 0 then
                    W.ShowItemTip(self, entry, "Shift-click or right-click to unequip. Alt-click to unlock this slot.")
                else
                    W.Tip(self, GEAR_SLOT_NAMES[slot] or ("Slot " .. slot), "Empty. Alt-click to unlock this slot.")
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
    W.HidePool(ui.pools.bagSlots)
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
        local b = W.TakeRow(ui.pools.bagSlots, ui.bagsChild, function(p) return W.MakeItemButton(p, size) end)
        local col = (i - 1) % cols
        local row = math.floor((i - 1) / cols)
        b:ClearAllPoints()
        b:SetPoint("TOPLEFT", col * (size + gap), -row * (size + gap))
        local it = items[(pos.bag * 1000) + pos.slot]
        local entry = it and it.entry or 0
        local count = it and it.count or 0
        W.SetItemIcon(b, entry, EMPTY_BAG_TEXTURE)
        if count > 1 then b.count:SetText(count) else b.count:SetText("") end
        b.lock:Hide()
        local bag, slot = pos.bag, pos.slot
        b:SetScript("OnEnter", function(self)
            if entry and entry > 0 then
                local extra = "Left-click to trade, mail, bank or farm more of it.\n"
                    .. "Right-click to equip. That slot will stay locked so the bot will not swap it back out."
                local tooHigh, need = ItemTooHighLevel(entry)
                if tooHigh then
                    extra = "Requires level " .. need .. ". The bot cannot equip this yet.\n" .. extra
                end
                W.ShowItemTip(self, entry, extra)
            else
                W.Tip(self, "Empty slot", "Open bag space.")
            end
        end)
        b:SetScript("OnClick", function(self, button)
            if button == "LeftButton" and entry and entry > 0 then
                local _, link = GetItemInfo(entry)
                if IsModifiedClick() and link then
                    HandleModifiedItemClick(link)
                    return
                end
                local itemId, botGuid, botName = entry, guid, selectedPartyName or "bot"
                local label = ItemLabel(itemId)
                W.ShowPopup(self, {
                    { text = "Trade to me", func = function()
                        TradeItemToMe(botGuid, itemId, 0)
                        SetStatus(botName .. ": trading " .. label .. " to you...")
                    end },
                    { text = "Mail to me", func = function()
                        SendBot("SENDITEM", botGuid, itemId .. ":mail")
                        SetStatus(botName .. ": mailing " .. label .. "...")
                    end },
                    { text = "Put in guild bank", func = function()
                        SendBot("SENDITEM", botGuid, itemId .. ":bank")
                        SetStatus(botName .. ": banking " .. label .. "...")
                    end },
                    { text = "Farm more of this", func = function()
                        local alt = AltNameByGuid(botGuid)
                        if not alt then Print("Farm orders work for your own alts.") return end
                        ShowTab("commands")
                        ui.orderChoice = alt
                        ui.whoBtn:SetText(alt)
                        if ui.SetMaterial then ui.SetMaterial(itemId) end
                        RefreshCommands()
                        SetStatus("Set the count and delivery, then press Farm it.")
                    end },
                    { text = "Equip", func = function()
                        SendBot("EQUIP", botGuid, tostring(bag) .. ":" .. tostring(slot))
                    end },
                })
                return
            end
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

function W.MakeProfBtn(parent)
    local b = CreateFrame("Button", nil, parent)
    b:SetWidth(46)
    b:SetHeight(54)
    W.Skin(b, C.row, C.border)
    W.AddHover(b, 0.10)
    b.icon = b:CreateTexture(nil, "ARTWORK")
    b.icon:SetPoint("TOP", 0, -4)
    b.icon:SetWidth(28)
    b.icon:SetHeight(28)
    b.icon:SetTexCoord(0.08, 0.92, 0.08, 0.92)
    b.rank = b:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    b.rank:SetPoint("BOTTOM", 0, 7)
    b.bar = W.MakeBar(b, 2)
    b.bar:SetPoint("BOTTOMLEFT", 3, 3)
    b.bar:SetPoint("BOTTOMRIGHT", -3, 3)
    b.bar:SetStatusBarColor(C.accent[1], C.accent[2], C.accent[3])
    b:SetScript("OnLeave", function() GameTooltip:Hide() end)
    return b
end

function W.ConfirmUnlearnProfession(name, profName, guid, skillId)
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
    W.HidePool(ui.pools.profBtns)
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
    local bw, bh, gap = 46, 54, 4
    for _, info in ipairs(PROFESSION_CATALOG) do
        local allowed = not profAllowed[guid] or profAllowed[guid][info.skillId]
        if allowed then
            if x > 0 and (x + bw) > rowW then
                x = 0
                y = y + bh + gap
            end
            local b = W.TakeRow(ui.pools.profBtns, ui.profRow, W.MakeProfBtn)
            b:ClearAllPoints()
            b:SetPoint("TOPLEFT", x, -y)
            b.icon:SetTexture(info.icon)
            local skill = learned[info.skillId]
            local skillId, profName = info.skillId, info.name
            if skill then
                b.icon:SetVertexColor(1, 1, 1)
                b.icon:SetDesaturated(false)
                b:SetBackdropBorderColor(C.accent[1], C.accent[2], C.accent[3], 1)
                b.rank:SetText(skill.value)
                W.TextColor(b.rank, C.text)
                b.bar:Show()
                b.bar:SetMinMaxValues(0, math.max(1, skill.maxValue or 1))
                b.bar:SetValue(skill.value or 0)
                b:SetScript("OnEnter", function(self)
                    W.Tip(self, profName, skill.value .. " / " .. skill.maxValue .. " skill.\nClick to unlearn. This wipes the skill.")
                end)
                b:SetScript("OnClick", function()
                    W.ConfirmUnlearnProfession(selectedPartyName, profName, guid, skillId)
                end)
            else
                b.icon:SetVertexColor(0.55, 0.55, 0.55)
                b.icon:SetDesaturated(true)
                b:SetBackdropBorderColor(C.border[1], C.border[2], C.border[3], 1)
                b.rank:SetText(info.secondary and "off" or "learn")
                W.TextColor(b.rank, C.faint)
                b.bar:Hide()
                b:SetScript("OnEnter", function(self)
                    W.Tip(self, profName, "Not learned. Click to teach this bot " .. profName .. ".")
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
function W.GrayLevel(level)
    if level <= 5 then return 0 end
    if level <= 39 then return level - 5 - math.floor(level / 10) end
    if level <= 59 then return level - 1 - math.floor(level / 5) end
    return level - 9
end

-- Quest-log difficulty colors, judged against the bot's level rather than yours.
function W.QuestLevelColor(questLevel, botLevel)
    if not botLevel or botLevel <= 0 then return 1, 0.82, 0 end
    local diff = questLevel - botLevel
    if diff >= 5 then return 1, 0.1, 0.1 end
    if diff >= 3 then return 1, 0.5, 0.25 end
    if diff >= -2 then return 1, 1, 0 end
    if questLevel > W.GrayLevel(botLevel) then return 0.25, 0.75, 0.25 end
    return 0.5, 0.5, 0.5
end

local QUEST_STATUS_TEXT = {
    [0] = { "In progress", 0.75, 0.75, 0.75 },
    [1] = { "Complete - ready to turn in", 0.3, 0.9, 0.3 },
    [2] = { "Failed", 1, 0.25, 0.2 },
}

function W.ObjectiveName(obj)
    if obj.kind == "i" and obj.entry > 0 then
        local name = GetItemInfo(obj.entry)
        if name and name ~= "" then return name end
    end
    return obj.name ~= "" and obj.name or "Objective"
end

function W.RequestQuests(guid, quiet)
    if not guid then return end
    questPending[guid] = GetTime()
    questBuild[guid] = nil
    ui.questTimedOut = nil
    ui.questQuiet = quiet and true or nil
    questAutoAt = GetTime() + QUEST_AUTO_REFRESH
    SendBot("GETQUESTS", guid)
end

function W.RequestAllQuests()
    local n = 0
    for _, m in ipairs(W.PartyMembers()) do
        if m.guid then W.RequestQuests(m.guid); n = n + 1 end
    end
    SetStatus(n > 0 and ("Refreshing quest logs for " .. n .. " party member" .. (n == 1 and "" or "s") .. "...")
        or "No party members to refresh.")
end

-- SHAREQUESTS: the server gives every quest in your log to your own grouped alts, even when
-- they are not eligible, and says in chat what each alt took. The quest logs refresh after it.
function W.ShareQuestsWithAlts()
    SendBot("SHAREQUESTS", 0)
    W.RequestAllQuests()
    SetStatus("Sharing your quests with your grouped alts...")
end

function W.MakeQuestListRow(parent)
    local row = W.MakePartyRow(parent)
    row.info:Hide()
    row.count = row:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    row.count:SetPoint("RIGHT", -7, 0)
    row.count:SetJustifyH("RIGHT")
    row.name:SetWidth(100)
    return row
end

function W.MakeQuestRow(parent)
    local row = CreateFrame("Button", nil, parent)
    row:SetHeight(24)
    W.Skin(row, C.row, C.row)
    W.AddHover(row, 0.05)
    row.title = row:CreateFontString(nil, "OVERLAY", "AltBotsFontBody")
    row.title:SetPoint("LEFT", 8, 1)
    row.title:SetJustifyH("LEFT")
    row.state = row:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    row.state:SetPoint("RIGHT", -8, 1)
    row.state:SetJustifyH("RIGHT")
    row.prog = W.MakeBar(row, 2)
    row.prog:SetPoint("BOTTOMLEFT", 1, 1)
    row.prog:SetPoint("BOTTOMRIGHT", -1, 1)
    row:SetScript("OnLeave", function() GameTooltip:Hide() end)
    return row
end

function W.MakeQuestObjRow(parent)
    local row = CreateFrame("Frame", nil, parent)
    row:SetHeight(16)
    row.text = row:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    row.text:SetPoint("LEFT", 20, 0)
    row.text:SetPoint("RIGHT", -90, 0)
    row.text:SetJustifyH("LEFT")
    row.count = row:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    row.count:SetPoint("RIGHT", -8, 0)
    row.count:SetJustifyH("RIGHT")
    return row
end

function W.ShowQuestTip(owner, q, botLevel, who)
    GameTooltip:SetOwner(owner, "ANCHOR_RIGHT")
    local ok = pcall(GameTooltip.SetHyperlink, GameTooltip, "quest:" .. q.id .. ":" .. q.level)
    if not ok or GameTooltip:NumLines() == 0 then
        GameTooltip:SetText(q.title, 1, 0.82, 0)
    end
    local st = QUEST_STATUS_TEXT[q.status] or QUEST_STATUS_TEXT[0]
    GameTooltip:AddLine(" ")
    GameTooltip:AddLine((who or selectedQuestName or "Bot") .. ": " .. st[1], st[2], st[3], st[4])
    for _, obj in ipairs(q.objectives) do
        local done = obj.have >= obj.need
        GameTooltip:AddDoubleLine(W.ObjectiveName(obj), obj.have .. "/" .. obj.need,
            0.85, 0.85, 0.85, done and 0.3 or 1, done and 0.9 or 1, done and 0.3 or 1)
    end
    GameTooltip:AddLine("Quest level " .. q.level .. (botLevel and botLevel > 0 and (", bot level " .. botLevel) or "") ..
        "  |  ID " .. q.id, 0.5, 0.5, 0.5)
    GameTooltip:Show()
end

function W.QuestFilter()
    return ui.questSearch and ui.questSearch:GetText():lower() or ""
end

function W.QuestMatches(q, filter)
    if filter == "" then return true end
    return string.find(q.title:lower(), filter, 1, true) ~= nil
end

function W.ObjectivesDone(q)
    local done = 0
    for _, obj in ipairs(q.objectives) do
        if obj.have >= obj.need then done = done + 1 end
    end
    return done, #q.objectives
end

function W.QuestProgress(q)
    if q.status == 1 then return 1 end
    local done, total = W.ObjectivesDone(q)
    if total == 0 then return 0 end
    return done / total
end

function W.QuestStateText(q)
    local st = QUEST_STATUS_TEXT[q.status] or QUEST_STATUS_TEXT[0]
    if q.status == 1 then return "Ready to turn in", st end
    if q.status == 0 and #q.objectives > 0 then
        local done, total = W.ObjectivesDone(q)
        return done .. "/" .. total .. " done", st
    end
    return st[1], st
end

function W.MyRules()
    AltBotsDB.sendRules = AltBotsDB.sendRules or {}
    local key = Realm() .. "-" .. (UnitName("player") or "?")
    AltBotsDB.sendRules[key] = AltBotsDB.sendRules[key] or {}
    return AltBotsDB.sendRules[key]
end

SyncRules = function()
    Queue({ addon = "RULECLEAR:0" })
    for _, rule in ipairs(W.MyRules()) do
        if rule.on then Queue({ addon = "RULE:0:" .. rule.id .. ":" .. (rule.mode == "bank" and "bank" or "mail") }) end
    end
end

function W.MakeRuleRow(parent)
    local row = CreateFrame("Frame", nil, parent)
    row:SetWidth(ROW_W); row:SetHeight(36)
    W.Skin(row, C.row, C.border)
    row.item = W.MakeItemButton(row, 28)
    row.item:SetPoint("LEFT", 4, 0)
    row.name = row:CreateFontString(nil, "OVERLAY", "AltBotsFontBody")
    row.name:SetPoint("LEFT", row.item, "RIGHT", 8, 0)
    row.name:SetWidth(300)
    row.name:SetJustifyH("LEFT")
    row.remove = W.MakeTextBtn(row, 64, 24, "Remove")
    row.remove:SetPoint("RIGHT", -6, 0)
    row.toggle = W.MakeTextBtn(row, 64, 24, "")
    row.toggle:SetPoint("RIGHT", row.remove, "LEFT", -6, 0)
    row.mode = W.MakeTextBtn(row, 96, 24, "")
    row.mode:SetPoint("RIGHT", row.toggle, "LEFT", -6, 0)
    return row
end

RefreshRules = function()
    if not win or not ui.rulesChild then return end
    W.HidePool(ui.pools.ruleRows)
    local rules = W.MyRules()
    if #rules == 0 then
        ui.rulesEmpty:Show()
        ui.rulesChild:SetHeight(1)
        return
    end
    ui.rulesEmpty:Hide()
    local y = 0
    for i, rule in ipairs(rules) do
        local row = W.TakeRow(ui.pools.ruleRows, ui.rulesChild, W.MakeRuleRow)
        row:ClearAllPoints()
        row:SetPoint("TOPLEFT", 0, y)
        local r, index = rule, i
        W.SetItemIcon(row.item, r.id)
        row.item.count:SetText("")
        row.item:SetScript("OnEnter", function(self) W.ShowItemTip(self, r.id) end)
        row.name:SetText(ItemLabel(r.id))
        W.TextColor(row.name, r.on and C.text or C.faint)
        row.mode:SetText(r.mode == "bank" and "Guild bank" or "Mail to me")
        W.HookTip(row.mode, "Where it goes", "Mail: every 5 minutes your alts mail what they carry of it to you. "
            .. "Guild bank: every 30 seconds they deposit it in their guild bank (alts without a guild skip it). Click to switch.")
        row.mode:SetScript("OnClick", function()
            r.mode = r.mode == "bank" and "mail" or "bank"
            SyncRules(); RefreshRules()
        end)
        row.toggle:SetText(r.on and "On" or "Off")
        row.toggle:SetActive(r.on)
        W.HookTip(row.toggle, "On or off", "Turn the rule on or off without removing it.")
        row.toggle:SetScript("OnClick", function()
            r.on = not r.on
            SyncRules(); RefreshRules()
            SetStatus(ItemLabel(r.id) .. (r.on and ": rule on." or ": rule off."))
        end)
        row.remove:SetScript("OnClick", function()
            table.remove(W.MyRules(), index)
            SyncRules(); RefreshRules()
        end)
        y = y - 40
    end
    ui.rulesChild:SetHeight(math.max(1, -y + 4))
end

BuildRulesPane = function(body)
    local pane = CreateFrame("Frame", nil, body)
    pane:SetAllPoints()
    pane:Hide()
    ui.rulesPane = pane

    local h, r = W.SectionHeader(pane, "Standing rules")
    h:SetPoint("TOPLEFT", 0, -2)
    r:SetPoint("LEFT", h, "RIGHT", 8, 0)
    r:SetPoint("RIGHT", 0, 0)

    local help = pane:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    help:SetPoint("TOPLEFT", 0, -22)
    help:SetWidth(ROW_W)
    help:SetJustifyH("LEFT")
    W.TextColor(help, C.muted)
    help:SetText("While a rule is on, every alt of yours that is spawned keeps sending that item: by mail to this character "
        .. "every 5 minutes, or into its guild bank every 30 seconds. Soulbound items and profession tools are never sent.")

    local box = W.MakeEditBox(pane, 240, 24)
    box:SetPoint("TOPLEFT", 0, -56)
    box.placeholder = box:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    box.placeholder:SetPoint("LEFT", 8, 0)
    box.placeholder:SetText("Shift-click an item link, or type its ID")
    W.TextColor(box.placeholder, C.faint)
    ui.linkBoxes = ui.linkBoxes or {}
    table.insert(ui.linkBoxes, box)

    local mode = "mail"
    local modeBtn = W.MakeTextBtn(pane, 96, 24, "Mail to me")
    modeBtn:SetPoint("LEFT", box, "RIGHT", 6, 0)
    modeBtn:SetScript("OnClick", function(self)
        mode = mode == "bank" and "mail" or "bank"
        self:SetText(mode == "bank" and "Guild bank" or "Mail to me")
    end)
    W.HookTip(modeBtn, "Where it goes", "Mail to this character, or the alt's guild bank. Click to switch.")

    local addBtn = W.MakeTextBtn(pane, 80, 24, "Add rule")
    addBtn:SetPoint("LEFT", modeBtn, "RIGHT", 6, 0)
    addBtn:SetScript("OnClick", function()
        box:ClearFocus()
        local id = ItemIdFrom(box:GetText())
        if not id then
            Print("Shift-click an item link into the box, or type an item ID.")
            return
        end
        local rules = W.MyRules()
        for _, rule in ipairs(rules) do
            if rule.id == id then
                rule.mode, rule.on = mode, true
                SyncRules(); RefreshRules()
                return
            end
        end
        table.insert(rules, { id = id, mode = mode, on = true })
        box:SetText("")
        if box.placeholder then box.placeholder:Show() end
        SyncRules(); RefreshRules()
        SetStatus(ItemLabel(id) .. ": rule added.")
    end)

    local allOff = W.MakeTextBtn(pane, 80, 24, "All off")
    allOff:SetPoint("LEFT", addBtn, "RIGHT", 6, 0)
    W.HookTip(allOff, "All off", "Turn every rule off.")
    allOff:SetScript("OnClick", function()
        for _, rule in ipairs(W.MyRules()) do rule.on = false end
        SyncRules(); RefreshRules()
    end)

    ui.rulesEmpty = W.EmptyText(pane, "AltBotsFontBody")
    ui.rulesEmpty:SetPoint("CENTER", 0, -40)
    W.TextColor(ui.rulesEmpty, C.muted)
    ui.rulesEmpty:SetText("No rules yet.\nShift-click an item into the box above, pick Mail or Guild bank, and press Add rule.")
    local sf, child = W.MakeScroll("AltBotsRulesScroll", pane, ROW_W)
    sf:SetPoint("TOPLEFT", 0, -92)
    sf:SetPoint("BOTTOMRIGHT", -18, 0)
    ui.rulesChild = child
end

BuildQuestPane = function(body)
    local pane = CreateFrame("Frame", nil, body)
    pane:SetAllPoints()
    pane:Hide()
    ui.questPane = pane

    local left
    left, ui.questListEmpty = W.MakeSidebar(pane, "Party", "Invite a bot to your group to see its quests here.")
    local lsf, lc = W.MakeScroll("AltBotsQuestListScroll", left, 168)
    lsf:SetPoint("TOPLEFT", 6, -28)
    lsf:SetPoint("BOTTOMRIGHT", -16, 6)
    ui.questListChild = lc

    local right = CreateFrame("Frame", nil, pane)
    right:SetPoint("TOPLEFT", left, "TOPRIGHT", 12, 0)
    right:SetPoint("BOTTOMRIGHT", 0, 0)

    ui.questPickHint = W.EmptyText(right, "AltBotsFontBody")
    ui.questPickHint:SetPoint("CENTER", 0, 20)
    W.TextColor(ui.questPickHint, C.muted)
    ui.questPickHint:SetText("Select a party member on the left.")

    local detail = CreateFrame("Frame", nil, right)
    detail:SetAllPoints()
    detail:Hide()
    ui.questDetail = detail

    ui.questName = detail:CreateFontString(nil, "OVERLAY", "AltBotsFontTitle")
    ui.questName:SetPoint("TOPLEFT", 0, -4)

    ui.questRefreshAllBtn = W.MakeTextBtn(detail, 80, 24, "Refresh all")
    ui.questRefreshAllBtn:SetPoint("TOPRIGHT", -4, 0)
    W.HookTip(ui.questRefreshAllBtn, "Refresh all", "Ask every grouped bot for a fresh quest log.")
    ui.questRefreshAllBtn:SetScript("OnClick", function()
        W.RequestAllQuests()
        RefreshQuests()
    end)

    ui.questRefreshBtn = W.MakeTextBtn(detail, 70, 24, "Refresh")
    ui.questRefreshBtn:SetPoint("RIGHT", ui.questRefreshAllBtn, "LEFT", -4, 0)
    W.HookTip(ui.questRefreshBtn, "Refresh", "Ask this bot for its current quest log. The open log also refreshes itself every " .. QUEST_AUTO_REFRESH .. " seconds.")
    ui.questRefreshBtn:SetScript("OnClick", function()
        if ui.questOverview then
            W.RequestAllQuests()
            RefreshQuests()
        elseif selectedQuestGuid then
            W.RequestQuests(selectedQuestGuid)
            SetStatus("Refreshing quests for " .. (selectedQuestName or "bot") .. "...")
            RefreshQuests()
        else
            SetStatus("Select a party member first.")
        end
    end)

    ui.questShareBtn = W.MakeTextBtn(detail, 100, 24, "Share quests")
    ui.questShareBtn:SetPoint("RIGHT", ui.questRefreshBtn, "LEFT", -4, 0)
    ui.questShareBtn:SetActive(true)
    W.HookTip(ui.questShareBtn, "Share quests", "Give every quest in your log to your own alts in your group, even ones "
        .. "they are not eligible for (level, class, race or prerequisites). Quests you accept are shared the same way "
        .. "automatically. Also /alt share.")
    ui.questShareBtn:SetScript("OnClick", W.ShareQuestsWithAlts)

    ui.questSearch = W.MakeEditBox(detail, 170, 22)
    ui.questSearch:SetPoint("TOPRIGHT", -4, -34)
    ui.questSearch.placeholder = ui.questSearch:CreateFontString(nil, "OVERLAY", "AltBotsFontSmall")
    ui.questSearch.placeholder:SetPoint("LEFT", 8, 0)
    ui.questSearch.placeholder:SetText("Filter quests")
    ui.questSearch:SetScript("OnTextChanged", function() RefreshQuests() end)

    local qh, qr = W.SectionHeader(detail, "Quest log")
    qh:SetPoint("TOPLEFT", 0, -38)
    qr:SetPoint("LEFT", qh, "RIGHT", 8, 0)
    qr:SetPoint("RIGHT", ui.questSearch, "LEFT", -8, 0)
    ui.questHeader = qh

    local qsf, qc = W.MakeScroll("AltBotsQuestScroll", detail, 440)
    qsf:SetPoint("TOPLEFT", 0, -64)
    qsf:SetPoint("BOTTOMRIGHT", -18, 2)
    ui.questScroll = qsf
    ui.questChild = qc

    ui.questEmpty = W.EmptyText(detail, "AltBotsFontBody")
    ui.questEmpty:SetPoint("CENTER", qsf, "CENTER", 0, 0)
    ui.questEmpty:SetWidth(380)
    W.TextColor(ui.questEmpty, C.muted)
    ui.questEmpty:SetText("")
end

function W.QuestSummary(log)
    local total, ready, failed = #log.list, 0, 0
    for _, q in ipairs(log.list) do
        if q.status == 1 then ready = ready + 1 elseif q.status == 2 then failed = failed + 1 end
    end
    local parts = { total .. " quest" .. (total == 1 and "" or "s") }
    if ready > 0 then table.insert(parts, ready .. " ready to turn in") end
    if failed > 0 then table.insert(parts, failed .. " failed") end
    return table.concat(parts, "  |  "), total, ready
end

function W.UpdateQuestBadge()
    if not ui.tabs or not ui.tabs.quests then return end
    local ready = 0
    for _, m in ipairs(W.PartyMembers()) do
        local log = m.guid and questLog[m.guid]
        if log then
            local _, _, r = W.QuestSummary(log)
            ready = ready + r
        end
    end
    ui.tabs.quests.badge:SetText(ready > 0 and (W.Hex(C.good) .. ready .. "|r") or "")
end

-- Called by HandleIncoming (which sits above `ui`) once a full QUESTEND log is in.
OnQuestLogArrived = function(guid, log)
    if guid == selectedQuestGuid and not ui.questOverview then
        if ui.activeTab == "quests" and not ui.questQuiet then
            local n = #log.list
            SetStatus((selectedQuestName or "Bot") .. ": " .. n .. " quest" .. (n == 1 and "" or "s") .. ".")
        end
        ui.questQuiet = nil
    end
    W.UpdateQuestBadge()
    RefreshQuests()
end

function W.PlaceQuestRow(y, width, text, stateText, r, g, b, st, progress, onEnter)
    local row = W.TakeRow(ui.pools.questRows, ui.questChild, W.MakeQuestRow)
    row:ClearAllPoints()
    row:SetPoint("TOPLEFT", 0, y)
    row:SetWidth(width)
    row.title:SetWidth(width - 140)
    row.title:SetText(text)
    row.title:SetTextColor(r, g, b)
    row.state:SetText(stateText)
    row.state:SetTextColor(st[1], st[2], st[3])
    row.prog:SetValue(progress or 0)
    if (progress or 0) >= 1 then
        row.prog:SetStatusBarColor(C.good[1], C.good[2], C.good[3])
    else
        row.prog:SetStatusBarColor(C.accent[1], C.accent[2], C.accent[3])
    end
    row:SetScript("OnEnter", onEnter)
    return y - 25
end

function W.PlaceObjRow(y, width, text, countText, color, countColor)
    local orow = W.TakeRow(ui.pools.questObjRows, ui.questChild, W.MakeQuestObjRow)
    orow:ClearAllPoints()
    orow:SetPoint("TOPLEFT", 0, y)
    orow:SetWidth(width)
    orow.text:SetText(text)
    W.TextColor(orow.text, color)
    orow.count:SetText(countText or "")
    W.TextColor(orow.count, countColor or color)
    return y - 16
end

function W.DrawBotQuestLog(log, childW, filter)
    local botLevel = log.botLevel
    if (not botLevel or botLevel <= 0) and selectedQuestUnit then botLevel = UnitLevel(selectedQuestUnit) end
    local qy, shown = 0, 0
    for _, q in ipairs(log.list) do
        if W.QuestMatches(q, filter) then
            shown = shown + 1
            local r, g, b = W.QuestLevelColor(q.level, botLevel)
            local stateText, st = W.QuestStateText(q)
            local quest = q
            qy = W.PlaceQuestRow(qy, childW, "[" .. q.level .. "] " .. q.title, stateText, r, g, b,
                { st[2], st[3], st[4] }, W.QuestProgress(q), function(self) W.ShowQuestTip(self, quest, botLevel) end)
            if #q.objectives == 0 then
                qy = W.PlaceObjRow(qy, childW, q.status == 1 and "Complete - ready to turn in"
                    or (q.status == 2 and "Quest failed" or "No tracked objectives (talk to an NPC or deliver)"),
                    "", C.faint)
            end
            for _, obj in ipairs(q.objectives) do
                local done = obj.have >= obj.need
                local countText = obj.kind == "e" and (done and "Done" or "Not yet") or (obj.have .. "/" .. obj.need)
                qy = W.PlaceObjRow(qy, childW, W.ObjectiveName(obj), countText,
                    done and C.good or { 0.85, 0.86, 0.9 }, done and C.good or C.text)
            end
            qy = qy - 8
        end
    end
    return qy, shown
end

function W.DrawQuestOverview(members, childW, filter)
    local withLog, waiting, silent = {}, {}, {}
    for _, m in ipairs(members) do
        local log = m.guid and questLog[m.guid]
        if log then
            table.insert(withLog, { member = m, log = log })
        elseif m.guid and questPending[m.guid] and (GetTime() - questPending[m.guid]) <= QUEST_TIMEOUT then
            table.insert(waiting, m.name or "?")
        else
            table.insert(silent, m.name or "?")
        end
    end

    local quests, order = {}, {}
    for _, entry in ipairs(withLog) do
        for _, q in ipairs(entry.log.list) do
            local agg = quests[q.id]
            if not agg then
                agg = { id = q.id, title = q.title, level = q.level, holders = {}, has = {} }
                quests[q.id] = agg
                table.insert(order, agg)
            end
            table.insert(agg.holders, { member = entry.member, quest = q, botLevel = entry.log.botLevel })
            agg.has[entry.member.guid] = true
        end
    end
    table.sort(order, function(a, b)
        if a.level ~= b.level then return a.level < b.level end
        return a.title < b.title
    end)

    local missingSomewhere = 0
    for _, agg in ipairs(order) do
        if #agg.holders < #withLog then missingSomewhere = missingSomewhere + 1 end
    end
    local parts = { #order .. " quest" .. (#order == 1 and "" or "s") .. " across " .. #withLog .. " bot" .. (#withLog == 1 and "" or "s") }
    if missingSomewhere > 0 then table.insert(parts, missingSomewhere .. " not on every bot") end
    if #waiting > 0 then table.insert(parts, "Loading...") end
    ui.questHeader:SetText("All quests  |  " .. table.concat(parts, "  |  "))

    local qy, shown = 0, 0
    if #waiting > 0 or #silent > 0 then
        local note = {}
        if #waiting > 0 then table.insert(note, "Loading: " .. table.concat(waiting, ", ")) end
        if #silent > 0 then table.insert(note, "No answer: " .. table.concat(silent, ", ")) end
        qy = W.PlaceObjRow(qy, childW, table.concat(note, "    "), "", C.faint) - 4
    end
    for _, agg in ipairs(order) do
        local q0 = agg.holders[1].quest
        if W.QuestMatches(q0, filter) then
            shown = shown + 1
            local ready, sumProgress = 0, 0
            for _, h in ipairs(agg.holders) do
                if h.quest.status == 1 then ready = ready + 1 end
                sumProgress = sumProgress + W.QuestProgress(h.quest)
            end
            local everyone = #agg.holders >= #withLog
            local stateText = #agg.holders .. "/" .. #withLog .. " bots"
            if ready == #agg.holders then stateText = stateText .. ", all ready" elseif ready > 0 then
                stateText = stateText .. ", " .. ready .. " ready"
            end
            local level0 = agg.holders[1].botLevel
            local r, g, b = W.QuestLevelColor(agg.level, level0)
            local first = agg.holders[1]
            qy = W.PlaceQuestRow(qy, childW, "[" .. agg.level .. "] " .. agg.title, stateText, r, g, b,
                everyone and C.good or C.warn, sumProgress / math.max(1, #agg.holders), function(self)
                    W.ShowQuestTip(self, first.quest, first.botLevel, first.member.name)
                end)
            for _, h in ipairs(agg.holders) do
                local text, st = W.QuestStateText(h.quest)
                qy = W.PlaceObjRow(qy, childW, W.ClassColorText(h.member.unit, h.member.name or "?"), text,
                    C.text, { st[2], st[3], st[4] })
            end
            if not everyone then
                local missing = {}
                for _, entry in ipairs(withLog) do
                    if not agg.has[entry.member.guid] then table.insert(missing, entry.member.name or "?") end
                end
                qy = W.PlaceObjRow(qy, childW, "Missing: " .. table.concat(missing, ", "), "", C.warn)
            end
            qy = qy - 8
        end
    end
    if #withLog == 0 then
        ui.questEmpty:Show()
        ui.questEmpty:SetText(#waiting > 0 and "Loading quest logs..." or
            "No quest logs yet. Only bots in your group answer; press Refresh all.")
    elseif shown == 0 then
        ui.questEmpty:Show()
        ui.questEmpty:SetText(#order == 0 and "None of your grouped bots have quests." or "No quest matches that filter.")
    else
        ui.questEmpty:Hide()
    end
    return qy
end

function W.MakeOverviewRow(parent)
    local row = W.MakeQuestListRow(parent)
    row.classIcon:SetTexture("Interface\\Icons\\INV_Misc_Book_09")
    row.classIcon:SetTexCoord(0.08, 0.92, 0.08, 0.92)
    row.dot:Hide()
    return row
end

-- Draws the member list and the selected bot's log. `opening` = the tab was just shown:
-- pick someone if nobody is selected, and ask for their log if we have none yet.
RefreshQuests = function(opening)
    if not win or not ui.questListChild then return end
    W.HidePool(ui.pools.questListRows)
    if ui.questOverviewRow then ui.questOverviewRow:Hide() end
    local members = W.PartyMembers()
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
        if opening and not ui.questOverview then
            -- Follow the Party tab's pick, else the first member.
            local pick = members[1]
            for _, m in ipairs(members) do
                if selectedPartyGuid and m.guid == selectedPartyGuid then pick = m end
            end
            selectedQuestGuid, selectedQuestName, selectedQuestUnit = pick.guid, pick.name, pick.unit
        end
    end
    if opening then
        if ui.questOverview then
            for _, m in ipairs(members) do
                if m.guid and not questLog[m.guid] and not questPending[m.guid] then W.RequestQuests(m.guid, true) end
            end
        elseif selectedQuestGuid and not questLog[selectedQuestGuid] and not questPending[selectedQuestGuid] then
            W.RequestQuests(selectedQuestGuid)
        end
    end

    if not ui.questOverviewRow then
        ui.questOverviewRow = W.MakeOverviewRow(ui.questListChild)
    end
    local ov = ui.questOverviewRow
    ov:ClearAllPoints()
    ov:SetPoint("TOPLEFT", 0, 0)
    ov:Show()
    ov.name:SetText("All bots")
    W.TextColor(ov.name, C.text)
    W.SetRowSelected(ov, ui.questOverview)
    local totalReady = 0
    for _, m in ipairs(members) do
        local log = m.guid and questLog[m.guid]
        if log then
            local _, _, ready = W.QuestSummary(log)
            totalReady = totalReady + ready
        end
    end
    ov.count:SetText(totalReady > 0 and (W.Hex(C.good) .. totalReady .. "|r ready") or "")
    ov:SetScript("OnClick", function()
        ui.questOverview = true
        selectedQuestGuid, selectedQuestName, selectedQuestUnit = nil, nil, nil
        W.RequestAllQuests()
        RefreshQuests()
    end)
    ov:SetScript("OnEnter", function(self)
        W.Tip(self, "All bots", "Every grouped bot's quests side by side, with who is missing each one. "
            .. "Handy after Share quests.")
    end)
    ov:SetScript("OnLeave", function() GameTooltip:Hide() end)

    local y = -40
    for _, m in ipairs(members) do
        local row = W.TakeRow(ui.pools.questListRows, ui.questListChild, W.MakeQuestListRow)
        row:ClearAllPoints()
        row:SetPoint("TOPLEFT", 0, y)
        row.name:SetText(W.ClassColorText(m.unit, m.name or "?"))
        W.SetClassIcon(row.classIcon, W.ClassFileOf(m.unit))
        W.SetDot(row.dot, UnitIsConnected(m.unit) and "green" or "gray")
        local log = m.guid and questLog[m.guid]
        if log then
            local _, total, ready = W.QuestSummary(log)
            row.count:SetText(ready > 0 and (W.Hex(C.good) .. ready .. "|r/" .. total) or tostring(total))
        else
            row.count:SetText("")
        end
        W.SetRowSelected(row, not ui.questOverview and selectedQuestGuid ~= nil and m.guid == selectedQuestGuid)
        local guid, name, unit = m.guid, m.name, m.unit
        row:SetScript("OnClick", function()
            ui.questOverview = nil
            selectedQuestGuid, selectedQuestName, selectedQuestUnit = guid, name, unit
            W.RequestQuests(guid)
            SetStatus("Loading quests for " .. (name or "bot") .. "...")
            RefreshQuests()
        end)
        row:SetScript("OnEnter", nil)
        y = y - 34
    end
    ui.questListChild:SetHeight(math.max(1, -y + 4))

    W.HidePool(ui.pools.questRows)
    W.HidePool(ui.pools.questObjRows)
    local childW = (ui.questScroll:GetWidth() or 0)
    if childW < 200 then childW = 440 end
    ui.questChild:SetWidth(childW)
    local filter = W.QuestFilter()

    if ui.questOverview then
        ui.questPickHint:Hide()
        ui.questDetail:Show()
        ui.questName:SetText("All bots")
        local qy = W.DrawQuestOverview(members, childW, filter)
        ui.questChild:SetHeight(math.max(1, -qy + 4))
        return
    end

    if not selectedQuestGuid then
        ui.questDetail:Hide()
        ui.questPickHint:Show()
        return
    end
    ui.questPickHint:Hide()
    ui.questDetail:Show()
    ui.questName:SetText(W.ClassColorText(selectedQuestUnit, selectedQuestName or "?"))

    local guid = selectedQuestGuid
    local log = questLog[guid]
    local pending = questPending[guid]
    local timedOut = pending and (GetTime() - pending) > QUEST_TIMEOUT

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

    local summary = W.QuestSummary(log)
    if pending and not timedOut then summary = summary .. "  |  Refreshing..." end
    ui.questHeader:SetText("Quest log  |  " .. summary)

    if #log.list == 0 then
        ui.questEmpty:Show()
        ui.questEmpty:SetText((selectedQuestName or "This bot") .. " has no quests.")
        ui.questChild:SetHeight(1)
        return
    end

    local qy, shown = W.DrawBotQuestLog(log, childW, filter)
    if shown == 0 then
        ui.questEmpty:Show()
        ui.questEmpty:SetText("No quest matches that filter.")
    else
        ui.questEmpty:Hide()
    end
    ui.questChild:SetHeight(math.max(1, -qy + 4))
end

ShowTab = function(id)
    EnsureWindow()
    W.HidePopup()
    ui.activeTab = id
    AltBotsDB.tab = id
    ui.altsPane:Hide(); ui.cmdPane:Hide(); ui.partyPane:Hide(); ui.questPane:Hide(); ui.rulesPane:Hide()
    W.SetTabState(ui.tabs.alts, id == "alts")
    W.SetTabState(ui.tabs.commands, id == "commands")
    W.SetTabState(ui.tabs.party, id == "party")
    W.SetTabState(ui.tabs.quests, id == "quests")
    W.SetTabState(ui.tabs.rules, id == "rules")
    W.UpdateHeaderSummary()
    W.UpdateQuestBadge()
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
    elseif id == "rules" then
        ui.rulesPane:Show(); RefreshRules()
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
        SyncRules()
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
        if ui.questOverview and not ui.questTimedOut then
            for _, m in ipairs(W.PartyMembers()) do
                local sent = m.guid and questPending[m.guid]
                if sent and now - sent > QUEST_TIMEOUT and now - sent < QUEST_TIMEOUT + 1 then
                    ui.questTimedOut = true
                    RefreshQuests()
                    break
                end
            end
        end
        if ui.questOverview and now >= questAutoAt then
            for _, m in ipairs(W.PartyMembers()) do
                if m.guid and not questPending[m.guid] then W.RequestQuests(m.guid, true) end
            end
            questAutoAt = now + QUEST_AUTO_REFRESH
        elseif guid and questLog[guid] and not questPending[guid] and now >= questAutoAt then
            W.RequestQuests(guid, true)
        end
    end
    if win and win:IsShown() and ui.activeTab == "commands" and now >= (ui.cardsAt or 0) then
        ui.cardsAt = now + 0.25
        W.UpdateCmdCards()
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
        W.UpdateHeaderSummary()
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
        W.CreateMinimapButton()
        if W.GetQuickBarShown() then SetQuickBarShown(true) end
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
            W.UpdateHeaderSummary()
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
    end
    if win:IsShown() then
        W.HidePopup()
        win:Hide()
    else
        win:Show()
        ShowTab(ui.activeTab or AltBotsDB.tab or "alts")
        SetStatus("Ready.")
    end
end

local USAGE = "/alt  open window | /alt invite|solo|despawn <name> | /alt list | /alt sync | /alt prune | /alt share"
    .. " | /alt add <name> <guid> | /alt remove <name> | /alt clear"
    .. " | /alt follow|stay|attack|stopattack|pull|collect <name|all> | /alt farm <name|all> [radius] | /alt farmstop <name|all>"
    .. " | /alt inventory <name|all> | /alt bar | /alt scale <0.6-1.5> | /alt reset | /alt minimap"

-- Key bindings (Bindings.xml) call these.
BINDING_HEADER_ALTBOTS = "AltBots"
BINDING_NAME_ALTBOTS_TOGGLE = "Toggle the AltBots window"
BINDING_NAME_ALTBOTS_QUICKBAR = "Show or hide the quick bar"
BINDING_NAME_ALTBOTS_FOLLOW = "Grouped alts: follow"
BINDING_NAME_ALTBOTS_STAY = "Grouped alts: stay"
BINDING_NAME_ALTBOTS_ATTACK = "Grouped alts: attack"
BINDING_NAME_ALTBOTS_STOPATTACK = "Grouped alts: stop attack"
BINDING_NAME_ALTBOTS_PULL = "Grouped alts: pull your target"
BINDING_NAME_ALTBOTS_COLLECT = "Grouped alts: collect the nearest pickup"
BINDING_NAME_ALTBOTS_SHARE = "Share your quests with grouped alts"

function AltBots_Toggle() ToggleWindow() end
function AltBots_ToggleQuickBar() SetQuickBarShown(not W.GetQuickBarShown()) end
function AltBots_Order(verb) W.RunOrder(verb, ALL_GROUPED) end
function AltBots_Share()
    W.ShareQuestsWithAlts()
    Print("Sharing your quests with your grouped alts...")
end

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
            Print(W.AltName(n, info) .. " = " .. tostring(info.guid) .. " (" .. (info.source or "login") .. ")"
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
    elseif cmd == "farmstop" or cmd == "follow" or cmd == "stay" or cmd == "attack" or cmd == "stopattack" or cmd == "pull" or cmd == "inventory" or cmd == "collect" then
        local n = arg:match("^(%S+)$")
        if not n then Print("Usage: /alt " .. cmd .. " <name|all>") return end
        Order(cmd, OrderArg(n))
    elseif cmd == "share" or cmd == "sharequests" then
        W.ShareQuestsWithAlts()
        Print("Sharing your quests with your grouped alts...")
    elseif cmd == "bar" or cmd == "quickbar" then
        SetQuickBarShown(not W.GetQuickBarShown())
        Print(W.GetQuickBarShown() and "Quick bar shown. Drag its blue edge to move it, right-click it to hide."
            or "Quick bar hidden.")
    elseif cmd == "scale" then
        local v = tonumber(arg)
        if not v then
            Print("Window scale is " .. (AltBotsDB.scale or 1) .. ". Usage: /alt scale <0.6-1.5>")
            return
        end
        if v < 0.6 then v = 0.6 elseif v > 1.5 then v = 1.5 end
        AltBotsDB.scale = v
        W.ApplyScale()
        Print("Window scale set to " .. v .. ".")
    elseif cmd == "reset" then
        AltBotsDB.winPos, AltBotsDB.quickBarPos, AltBotsDB.scale = nil, nil, nil
        if win then W.RestorePosition(win, "winPos", { "CENTER", "CENTER", 0, 0 }) end
        if quickBar then W.RestorePosition(quickBar, "quickBarPos", { "BOTTOM", "BOTTOM", 0, 160 }) end
        W.ApplyScale()
        Print("Window position, quick bar position and scale reset.")
    elseif cmd == "minimap" then
        W.SetMinimapShown(not W.GetMinimapShown())
        Print(W.GetMinimapShown() and "Minimap button shown." or "Minimap button hidden.")
    elseif cmd == "help" then
        Print(USAGE)
    else
        Print(USAGE)
    end
end
