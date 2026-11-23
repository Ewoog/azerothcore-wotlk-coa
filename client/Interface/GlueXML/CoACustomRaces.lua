MAX_RACES = 16

local STOCK_RACE_ICONS = "Interface\\Glues\\CharacterCreate\\UI-CHARACTERCREATE-RACES_Round"
local CUSTOM_RACE_ICON_COORDS = { 0.07, 0.93, 0.07, 0.93 }
local CUSTOM_RACE_ICONS = {
    ["GOBLIN"] = { "achievement_goblinhead", "achievement_femalegoblinhead" },
    ["ZANDALARI TROLL"] = { "achievement_alliedrace_zandalaritroll" },
    ["WORGEN"] = { "achievement_worganhead" },
    ["KULTIRAN"] = { "achievement_alliedrace_kultiranhuman" },
    ["VULPERA"] = { "achievement_alliedrace_vulpera" },
    ["PANDAREN"] = { "achievement_character_pandaren_female" },
}
local BACKGROUND_RACES = {
    ["GOBLIN"] = "Orc",
    ["ZANDALARI TROLL"] = "Troll",
    ["WORGEN"] = "Human",
    ["KULTIRAN"] = "Human",
    ["VULPERA"] = "Orc",
    ["PANDAREN"] = "Dwarf",
}
local COMPACT_ROW_SPACING = 66

for raceEnglishName in pairs(CUSTOM_RACE_ICONS) do
    RACE_ICON_TCOORDS[raceEnglishName .. "_MALE"] = CUSTOM_RACE_ICON_COORDS
    RACE_ICON_TCOORDS[raceEnglishName .. "_FEMALE"] = CUSTOM_RACE_ICON_COORDS
end

local function RaceIcon(raceEnglishName, gender)
    local icons = CUSTOM_RACE_ICONS[raceEnglishName]
    if not icons then
        return STOCK_RACE_ICONS
    end
    local icon = gender == "FEMALE" and icons[2] or icons[1]
    return "Interface\\Icons\\" .. icon
end

local stockSetBackgroundModel = SetBackgroundModel
function SetBackgroundModel(model, name)
    return stockSetBackgroundModel(model, BACKGROUND_RACES[strupper(name)] or name)
end

local stockGetFlavorText = GetFlavorText
function GetFlavorText(tag, sex)
    return stockGetFlavorText(tag, sex) or ""
end

local stockSetRace = CharCreateRaceButtonMixin.SetRace
function CharCreateRaceButtonMixin:SetRace(raceName, raceEnglishName, gender, enabled)
    local icon = RaceIcon(raceEnglishName, gender)
    self:GetNormalTexture():SetTexture(icon)
    self:GetPushedTexture():SetTexture(icon)
    stockSetRace(self, raceName, raceEnglishName, gender, enabled)
end

local function RaceFaction(index)
    local _, faction = GetFactionForRace(index)
    if faction == "Horde" then
        return "Horde"
    end
    return "Alliance"
end

local stockEnumerateRaces = CharacterCreateEnumerateRaces
function CharacterCreateEnumerateRaces(...)
    stockEnumerateRaces(...)
    local count = CharacterCreate.numRaces or 0
    if count <= 11 then
        return
    end

    local rows = { Alliance = 0, Horde = 0 }
    for index = 1, count do
        local button = _G["CharCreateRaceButton" .. index]
        local faction = RaceFaction(index)
        local offset = -75 - rows[faction] * COMPACT_ROW_SPACING
        rows[faction] = rows[faction] + 1
        button:SetFaction(faction)
        button:ClearAllPoints()
        if faction == "Alliance" then
            button:SetPoint("TOPLEFT", CharCreateRaceButtonsFrame, "TOPLEFT", 32, offset)
        else
            button:SetPoint("TOPRIGHT", CharCreateRaceButtonsFrame, "TOPRIGHT", -32, offset)
        end
    end
end

function CharCreateClassButtonMixin:HighlightValidRaces()
    for index = 1, MAX_RACES do
        local button = _G["CharCreateRaceButton" .. index]
        if button and button:IsShown() and button:IsEnabled() then
            if IsRaceClassValid(button:GetID(), self:GetID()) then
                button.Glow.Anim:Play()
            else
                button.Glow.Anim:Stop()
            end
        end
    end
end

function CharCreateClassButtonMixin:StopHighlightingRaces()
    for index = 1, MAX_RACES do
        local button = _G["CharCreateRaceButton" .. index]
        if button then
            button.Glow.Anim:Stop()
        end
    end
end
