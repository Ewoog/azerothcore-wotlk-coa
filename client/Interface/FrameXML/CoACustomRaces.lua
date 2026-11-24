local CUSTOM_RACES = {
    ["Goblin"] = { id = 9, base = "Orc" },
    ["Zandalari Troll"] = { id = 12, base = "Troll" },
    ["Worgen"] = { id = 13, base = "Human" },
    ["KulTiran"] = { id = 16, base = "Human" },
    ["Vulpera"] = { id = 19, base = "Orc" },
    ["Pandaren"] = { id = 20, base = "Dwarf" },
}

local function Alias(key, baseKey)
    AtlasInfo[key] = AtlasInfo[key] or AtlasInfo[baseKey]
end

for raceFile, race in pairs(CUSTOM_RACES) do
    Alias("DressUpBackground-" .. raceFile, "DressUpBackground-" .. race.base)
    Alias("transmog-background-race-" .. strlower(raceFile), "transmog-background-race-" .. strlower(race.base))
    Enum.Race[raceFile] = Enum.Race[raceFile] or race.id
    Enum.RaceFile[race.id] = Enum.RaceFile[race.id] or raceFile
end

local stockDressUpTexturePath = DressUpTexturePath
function DressUpTexturePath(fileName, atlas)
    fileName = fileName or select(2, UnitRace("player"))
    local race = fileName and CUSTOM_RACES[fileName]
    return stockDressUpTexturePath(race and race.base or fileName, atlas)
end
