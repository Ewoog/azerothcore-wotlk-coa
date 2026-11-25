import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile


RACES = {
    9: dict(base=2, faction=2, language=1, alliance=1),
    12: dict(base=8, faction=116, language=1, alliance=1),
    13: dict(base=1, faction=1, language=7, alliance=0),
    16: dict(base=1, faction=1, language=7, alliance=0),
    19: dict(base=2, faction=2, language=1, alliance=1),
    20: dict(base=3, faction=3, language=7, alliance=0),
    21: dict(base=3, faction=3, language=7, alliance=0, source=19),
}
COPIED_RACES = {race: settings['source'] for race, settings in RACES.items() if 'source' in settings}
NOT_PLAYABLE = 0x01
CAN_MOUNT = 0x04

CHR_RACES = 'DBFilesClient/ChrRaces.dbc'
CHAR_BASE_INFO = 'DBFilesClient/CharBaseInfo.dbc'
CAMERAS = 'DBFilesClient/UICameraAppearanceChrRaces.dbc'
START_OUTFITS = 'DBFilesClient/CharStartOutfit.dbc'
SECTIONS = 'DBFilesClient/CharSections.dbc'
HAIR_GEOSETS = 'DBFilesClient/CharHairGeosets.dbc'
FACIAL_HAIR = 'DBFilesClient/CharacterFacialHairStyles.dbc'
HAIR_TEXTURES = 'DBFilesClient/CharHairTextures.dbc'
VARIATIONS = 'DBFilesClient/CharVariations.dbc'
BARBER_STYLES = 'DBFilesClient/BarberShopStyle.dbc'
CHARACTER_CREATE_XML = 'Interface/GlueXML/CharacterCreate.xml'
OVERLAY = 'Interface/GlueXML/CoACustomRaces.lua'
OVERLAY_SOURCE = Path(__file__).resolve().parents[1] / 'client' / OVERLAY
FRAME_TOC = 'Interface/FrameXML/FrameXML.toc'
FRAME_OVERLAY = 'Interface/FrameXML/CoACustomRaces.lua'
FRAME_OVERLAY_SOURCE = Path(__file__).resolve().parents[1] / 'client' / FRAME_OVERLAY
FRAME_TOC_MARKER = '## add new modules above here'
ARCHIVE_MEMBERS = {
    'patch-M.MPQ': [CHR_RACES, CAMERAS, START_OUTFITS, SECTIONS, HAIR_GEOSETS, FACIAL_HAIR, HAIR_TEXTURES, VARIATIONS,
                    BARBER_STYLES],
    'patch-T.MPQ': [CHAR_BASE_INFO],
    'patch-B.MPQ': [CHARACTER_CREATE_XML, FRAME_TOC],
}
ARCHIVE_ADDITIONS = {
    'patch-B.MPQ': {OVERLAY: OVERLAY_SOURCE, FRAME_OVERLAY: FRAME_OVERLAY_SOURCE},
}
STOCK_RACE_BUTTONS = 11
SERVER_MEMBERS = {START_OUTFITS, SECTIONS}
RACE_BUTTONS = 10 + len(RACES)


class Dbc:
    def __init__(self, data, fields=None, record_size=None):
        magic, count, field_count, size, strings = struct.unpack_from('<4sIIII', data)
        if magic != b'WDBC':
            raise ValueError('Not a WDBC file')
        if fields is not None and field_count != fields or record_size is not None and size != record_size:
            raise ValueError(f'Unexpected DBC layout: {field_count} fields, {size} byte records')
        if len(data) != 20 + count * size + strings:
            raise ValueError('DBC size does not match its header')
        self.field_count, self.record_size = field_count, size
        self.records = [bytearray(data[20 + i * size:20 + (i + 1) * size]) for i in range(count)]
        self.strings = data[20 + count * size:]

    def pack(self):
        header = struct.pack('<4sIIII', b'WDBC', len(self.records), self.field_count, self.record_size,
                             len(self.strings))
        return header + b''.join(self.records) + self.strings


def patch_chr_races(data):
    dbc = Dbc(data, fields=69, record_size=276)
    rows = {}
    for record in dbc.records:
        race = struct.unpack_from('<I', record)[0]
        if race in rows:
            raise ValueError(f'Duplicate ChrRaces row {race}')
        rows[race] = record
    existing = set(COPIED_RACES) & set(rows)
    if existing:
        raise ValueError(f'ChrRaces already has rows {sorted(existing)}')
    for race, source in COPIED_RACES.items():
        if source not in rows:
            raise ValueError(f'ChrRaces lacks row {source} to copy for race {race}')
        copy = bytearray(rows[source])
        struct.pack_into('<I', copy, 0, race)
        dbc.records.append(copy)
        rows[race] = copy
    missing = set(RACES) - set(rows)
    if missing:
        raise ValueError(f'ChrRaces lacks rows {sorted(missing)}')
    for race, settings in RACES.items():
        record = rows[race]
        flags = struct.unpack_from('<I', record, 4)[0]
        struct.pack_into('<II', record, 4, (flags & ~NOT_PLAYABLE) | CAN_MOUNT, settings['faction'])
        struct.pack_into('<I', record, 7 * 4, settings['language'])
        struct.pack_into('<I', record, 13 * 4, settings['alliance'])
    dbc.records.sort(key=lambda record: struct.unpack_from('<I', record)[0])
    return dbc.pack()


def copy_race_rows(data, fields, race_field, id_field=None):
    dbc = Dbc(data, fields=fields, record_size=fields * 4)
    races = [struct.unpack_from('<I', record, race_field * 4)[0] for record in dbc.records]
    if set(COPIED_RACES) & set(races):
        raise ValueError(f'Rows already exist for race {sorted(set(COPIED_RACES) & set(races))}')
    next_id = None
    if id_field is not None:
        next_id = max(struct.unpack_from('<I', record, id_field * 4)[0] for record in dbc.records) + 1
    for race, source in COPIED_RACES.items():
        sources = [record for record, owner in zip(list(dbc.records), races) if owner == source]
        if not sources:
            raise ValueError(f'No rows for race {source} to copy for race {race}')
        for record in sources:
            copy = bytearray(record)
            struct.pack_into('<I', copy, race_field * 4, race)
            if next_id is not None:
                struct.pack_into('<I', copy, id_field * 4, next_id)
                next_id += 1
            dbc.records.append(copy)
    return dbc.pack()


def extend_sections(data):
    return copy_race_rows(data, fields=10, race_field=1, id_field=0)


def extend_hair_geosets(data):
    return copy_race_rows(data, fields=6, race_field=1, id_field=0)


def extend_facial_hair(data):
    return copy_race_rows(data, fields=8, race_field=0)


def extend_hair_textures(data):
    return copy_race_rows(data, fields=8, race_field=1, id_field=0)


def extend_variations(data):
    return copy_race_rows(data, fields=6, race_field=0)


def extend_barber_styles(data):
    return copy_race_rows(data, fields=40, race_field=37, id_field=0)


def extend_char_base_info(data):
    dbc = Dbc(data, fields=2, record_size=2)
    pairs = [tuple(record) for record in dbc.records]
    present = set(pairs)
    for race, settings in RACES.items():
        if any(pair[0] == race for pair in present):
            raise ValueError(f'CharBaseInfo already has rows for race {race}')
        classes = [cls for base, cls in pairs if base == settings['base']]
        if not classes:
            raise ValueError(f'CharBaseInfo has no rows for base race {settings["base"]}')
        dbc.records.extend(bytearray((race, cls)) for cls in sorted(set(classes)))
    return dbc.pack()


def extend_cameras(data):
    dbc = Dbc(data, fields=5, record_size=20)
    rows = [list(struct.unpack('<5I', record)) for record in dbc.records]
    if any(row[1] in RACES for row in rows):
        raise ValueError('UICameraAppearanceChrRaces already has rows for a custom race')
    next_id = max(row[0] for row in rows) + 1
    for race, settings in RACES.items():
        base_rows = [row for row in rows if row[1] == settings['base']]
        if not base_rows:
            raise ValueError(f'UICameraAppearanceChrRaces has no rows for base race {settings["base"]}')
        for row in base_rows:
            dbc.records.append(bytearray(struct.pack('<5I', next_id, race, *row[2:])))
            next_id += 1
    return dbc.pack()


def extend_start_outfits(data):
    dbc = Dbc(data, fields=77, record_size=296)
    present = {tuple(record[4:7]) for record in dbc.records}
    next_id = max(struct.unpack_from('<I', record)[0] for record in dbc.records) + 1
    added = 0
    for race, settings in RACES.items():
        for record in list(dbc.records):
            base, cls, sex = record[4:7]
            if base != settings['base'] or (race, cls, sex) in present:
                continue
            copy = bytearray(record)
            struct.pack_into('<I', copy, 0, next_id)
            copy[4] = race
            dbc.records.append(copy)
            present.add((race, cls, sex))
            next_id += 1
            added += 1
    if not added:
        raise ValueError('CharStartOutfit already has every base race outfit for the custom races')
    return dbc.pack()


def extend_character_create_xml(data):
    text = data.decode('utf-8-sig')
    newline = '\r\n' if '\r\n' in text else '\n'
    if 'CoACustomRaces.lua' in text or f'CharCreateRaceButton{STOCK_RACE_BUTTONS + 1}"' in text:
        raise ValueError('CharacterCreate.xml already carries the custom race buttons')
    include = re.compile(r'([ \t]*)<Script\s+file\s*=\s*"CharacterCreate\.lua"\s*/>', re.I)
    matches = include.findall(text)
    if len(matches) != 1:
        raise ValueError('Expected exactly one CharacterCreate.lua script include')
    text = include.sub(lambda m: m.group(0) + newline + m.group(1) + '<Script file="CoACustomRaces.lua"/>', text)
    last = re.compile(r'([ \t]*)<CheckButton\s+name="CharCreateRaceButton%d"[^>]*>.*?</CheckButton>'
                      % STOCK_RACE_BUTTONS, re.S)
    matches = last.findall(text)
    if len(matches) != 1:
        raise ValueError(f'Expected exactly one CharCreateRaceButton{STOCK_RACE_BUTTONS}')

    def buttons(match):
        indent = match.group(1)
        added = [f'{indent}<CheckButton name="CharCreateRaceButton{index}" inherits="CharCreateRaceButtonTemplate" '
                 f'id="{index}"><Anchors><Anchor point="TOPLEFT" relativeTo="CharCreateRaceButtonsFrame" '
                 f'x="32" y="-75"/></Anchors></CheckButton>'
                 for index in range(STOCK_RACE_BUTTONS + 1, RACE_BUTTONS + 1)]
        return match.group(0) + newline + newline.join(added)

    text = last.sub(buttons, text)
    prefix = b'\xef\xbb\xbf' if data.startswith(b'\xef\xbb\xbf') else b''
    return prefix + text.encode('utf-8')


def extend_frame_toc(data):
    text = data.decode('utf-8-sig')
    newline = '\r\n' if '\r\n' in text else '\n'
    lines = text.split(newline)
    if any(line.strip().lower() == 'coacustomraces.lua' for line in lines):
        raise ValueError('FrameXML.toc already loads CoACustomRaces.lua')
    markers = [index for index, line in enumerate(lines) if line.strip() == FRAME_TOC_MARKER]
    if len(markers) != 1:
        raise ValueError(f'Expected exactly one "{FRAME_TOC_MARKER}" line in FrameXML.toc')
    lines.insert(markers[0], 'CoACustomRaces.lua')
    prefix = b'\xef\xbb\xbf' if data.startswith(b'\xef\xbb\xbf') else b''
    return prefix + newline.join(lines).encode('utf-8')


PATCHERS = {CHR_RACES: patch_chr_races, CHAR_BASE_INFO: extend_char_base_info, CAMERAS: extend_cameras,
            START_OUTFITS: extend_start_outfits, SECTIONS: extend_sections, HAIR_GEOSETS: extend_hair_geosets,
            FACIAL_HAIR: extend_facial_hair, HAIR_TEXTURES: extend_hair_textures, VARIATIONS: extend_variations,
            BARBER_STYLES: extend_barber_styles, CHARACTER_CREATE_XML: extend_character_create_xml,
            FRAME_TOC: extend_frame_toc}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def package_archive(baseline, output, executable, members, additions=None):
    baseline, output = Path(baseline).resolve(), Path(output).resolve()
    if baseline == output or output.exists():
        raise ValueError(f'{output} must be a new file, separate from the installed client')
    if not baseline.is_file():
        raise ValueError(f'{baseline} does not exist')
    output.parent.mkdir(parents=True, exist_ok=True)
    original_hash = sha(baseline)

    def run(*args):
        result = subprocess.run([executable, *map(str, args)], capture_output=True)
        if result.returncode:
            raise RuntimeError(result.stderr.decode('utf-8', 'replace') or 'mpqcli operation failed')
        return result.stdout

    def inventory(directory):
        return {str(p.relative_to(directory)).replace('\\', '/').lower(): sha(p)
                for p in directory.rglob('*') if p.is_file() and not p.name.startswith('(')}

    with tempfile.TemporaryDirectory(prefix='custom-races-', dir=output.parent) as temporary:
        temp = Path(temporary)
        before, after, payload = temp / 'before', temp / 'after', temp / 'payload'
        payload.mkdir()
        run('extract', '--keep', '--output', before, baseline)
        wanted = inventory(before)
        candidate = temp / 'candidate.MPQ'
        shutil.copyfile(baseline, candidate)
        for member in members:
            source = next((p for p in before.rglob('*') if p.is_file() and
                           str(p.relative_to(before)).replace('\\', '/').lower() == member.lower()), None)
            if source is None:
                raise ValueError(f'{baseline.name} does not contain {member}')
            patched = payload / Path(member).name
            patched.write_bytes(PATCHERS[member](source.read_bytes()))
            if member in SERVER_MEMBERS:
                server = output.parent / 'server-dbc' / Path(member).name
                server.parent.mkdir(exist_ok=True)
                shutil.copyfile(patched, server)
            run('add', '--overwrite', '--game', 'wow-wotlk', '--path', member.replace('/', '\\'), candidate, patched)
            wanted[member.lower()] = sha(patched)
        for member, source in (additions or {}).items():
            if member.lower() in wanted:
                raise ValueError(f'{baseline.name} already contains {member}')
            run('add', '--game', 'wow-wotlk', '--path', member.replace('/', '\\'), candidate, source)
            wanted[member.lower()] = sha(source)
        run('extract', '--keep', '--output', after, candidate)
        if inventory(after) != wanted:
            raise RuntimeError(f'{baseline.name} readback differed from the expected member inventory')
        if sha(baseline) != original_hash:
            raise RuntimeError('Baseline changed while building; discard this candidate and retry')
        shutil.copyfile(candidate, output)
    return dict(archive=baseline.name, baseline_sha256=original_hash, candidate_sha256=sha(output),
                changed_members=members, added_members=sorted(additions or {}))


def package(client_data, output_directory, executable):
    client_data, output_directory = Path(client_data), Path(output_directory)
    executable = str(Path(executable).resolve())
    reports = [package_archive(client_data / name, output_directory / name, executable, members,
                               ARCHIVE_ADDITIONS.get(name))
               for name, members in ARCHIVE_MEMBERS.items()]
    report = dict(mpqcli_sha256=sha(executable), archives=reports, state='built; not installed or published')
    (output_directory / 'custom-races-client.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report


def main():
    parser = argparse.ArgumentParser(description='Build client archives that make the custom races playable')
    parser.add_argument('--client-data', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--mpqcli', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(package(args.client_data, args.output, args.mpqcli), indent=2))


if __name__ == '__main__':
    main()
