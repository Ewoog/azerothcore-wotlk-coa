import argparse
import hashlib
import json
from pathlib import Path
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
}
NOT_PLAYABLE = 0x01
CAN_MOUNT = 0x04

CHR_RACES = 'DBFilesClient/ChrRaces.dbc'
CHAR_BASE_INFO = 'DBFilesClient/CharBaseInfo.dbc'
CAMERAS = 'DBFilesClient/UICameraAppearanceChrRaces.dbc'
ARCHIVE_MEMBERS = {
    'patch-M.MPQ': [CHR_RACES, CAMERAS],
    'patch-T.MPQ': [CHAR_BASE_INFO],
}


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
    found = set()
    for record in dbc.records:
        race = struct.unpack_from('<I', record)[0]
        if race not in RACES:
            continue
        if race in found:
            raise ValueError(f'Duplicate ChrRaces row {race}')
        found.add(race)
        settings = RACES[race]
        flags = struct.unpack_from('<I', record, 4)[0]
        struct.pack_into('<II', record, 4, (flags & ~NOT_PLAYABLE) | CAN_MOUNT, settings['faction'])
        struct.pack_into('<I', record, 7 * 4, settings['language'])
        struct.pack_into('<I', record, 13 * 4, settings['alliance'])
    if found != set(RACES):
        raise ValueError(f'ChrRaces lacks rows {sorted(set(RACES) - found)}')
    return dbc.pack()


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


PATCHERS = {CHR_RACES: patch_chr_races, CHAR_BASE_INFO: extend_char_base_info, CAMERAS: extend_cameras}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def package_archive(baseline, output, executable, members):
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
            run('add', '--overwrite', '--game', 'wow-wotlk', '--path', member.replace('/', '\\'), candidate, patched)
            wanted[member.lower()] = sha(patched)
        run('extract', '--keep', '--output', after, candidate)
        if inventory(after) != wanted:
            raise RuntimeError(f'{baseline.name} readback differed from the expected member inventory')
        if sha(baseline) != original_hash:
            raise RuntimeError('Baseline changed while building; discard this candidate and retry')
        shutil.copyfile(candidate, output)
    return dict(archive=baseline.name, baseline_sha256=original_hash, candidate_sha256=sha(output),
                changed_members=members)


def package(client_data, output_directory, executable):
    client_data, output_directory = Path(client_data), Path(output_directory)
    executable = str(Path(executable).resolve())
    reports = [package_archive(client_data / name, output_directory / name, executable, members)
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
