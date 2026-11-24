from pathlib import Path
import re
import struct
import unittest

from package_custom_races_client import (FRAME_OVERLAY_SOURCE, OVERLAY_SOURCE, RACES, extend_cameras,
                                         extend_char_base_info, extend_character_create_xml, extend_frame_toc,
                                         extend_start_outfits, patch_chr_races)


ROOT = Path(__file__).resolve().parents[1]
SQL = ROOT / 'data/sql/updates/pending_db_world/rev_20261123_01_custom_playable_races.sql'


def dbc(records, fields, size, strings=b'\0'):
    return struct.pack('<4sIIII', b'WDBC', len(records), fields, size, len(strings)) + b''.join(records) + strings


def chr_race(race, flags, faction=1, language=7, alliance=2):
    values = [0] * 69
    values[0], values[1], values[2], values[7], values[13] = race, flags, faction, language, alliance
    return struct.pack('<69I', *values)


def records(data, size):
    count = struct.unpack_from('<I', data, 4)[0]
    return [data[20 + i * size:20 + (i + 1) * size] for i in range(count)]


class ChrRacesTest(unittest.TestCase):
    def source(self):
        rows = [chr_race(1, 12), chr_race(2, 12, 2, 1, 1), chr_race(40, 15)]
        rows += [chr_race(race, 1 if race == 9 else 5) for race in RACES]
        return dbc(rows, 69, 276)

    def test_custom_races_become_playable_on_their_side(self):
        patched = {struct.unpack_from('<I', row)[0]: struct.unpack('<69I', row)
                   for row in records(patch_chr_races(self.source()), 276)}
        for race, settings in RACES.items():
            row = patched[race]
            self.assertEqual(row[1] & 0x01, 0)
            self.assertTrue(row[1] & 0x04)
            self.assertEqual((row[2], row[7], row[13]),
                             (settings['faction'], settings['language'], settings['alliance']))
        self.assertEqual(patched[40][1], 15)
        self.assertEqual(patched[1][1:3], (12, 1))

    def test_missing_or_duplicate_rows_fail(self):
        with self.assertRaises(ValueError):
            patch_chr_races(dbc([chr_race(1, 12)], 69, 276))
        source = self.source()
        with self.assertRaises(ValueError):
            patch_chr_races(dbc(records(source, 276) + [chr_race(9, 1)], 69, 276))


class CharBaseInfoTest(unittest.TestCase):
    def test_custom_races_get_their_base_race_classes(self):
        pairs = [(1, 1), (1, 2), (2, 1), (2, 3), (3, 4), (8, 5), (1, 32)]
        source = dbc([bytes(pair) for pair in pairs], 2, 2, b'')
        result = [tuple(row) for row in records(extend_char_base_info(source), 2)]
        self.assertEqual(result[:len(pairs)], pairs)
        for race, settings in RACES.items():
            expected = sorted(cls for base, cls in pairs if base == settings['base'])
            self.assertEqual([cls for r, cls in result if r == race], expected)

    def test_already_extended_file_fails(self):
        with self.assertRaises(ValueError):
            extend_char_base_info(dbc([bytes((1, 1)), bytes((2, 1)), bytes((3, 1)), bytes((8, 1)), bytes((9, 1))],
                                      2, 2, b''))


class CameraTest(unittest.TestCase):
    def test_custom_races_reuse_base_race_cameras(self):
        rows = [struct.pack('<5I', i + 1, race, sex, 7, 100 + i)
                for i, (race, sex) in enumerate([(1, 1), (1, 0), (2, 1), (3, 0), (8, 1)])]
        result = [struct.unpack('<5I', row) for row in records(extend_cameras(dbc(rows, 5, 20, b'')), 20)]
        ids = [row[0] for row in result]
        self.assertEqual(len(ids), len(set(ids)))
        worgen = [row[2:] for row in result if row[1] == 13]
        self.assertEqual(worgen, [(1, 7, 100), (0, 7, 101)])


XML = '''<Ui>\r
\t<Script file="CharacterCreate.lua"/>\r
\t<Frame name="CharCreateRaceButtonsFrame">\r
\t\t<CheckButton name="CharCreateRaceButton10" inherits="CharCreateRaceButtonTemplate" id="10">\r
\t\t</CheckButton>\r
\t\t<CheckButton name="CharCreateRaceButton11" inherits="CharCreateRaceButtonTemplate" id="11">\r
\t\t\t<Anchors><Anchor point="TOPLEFT" relativeTo="CharCreateRaceButton10"/></Anchors>\r
\t\t</CheckButton>\r
\t</Frame>\r
</Ui>\r
'''


class CharacterCreateXmlTest(unittest.TestCase):
    def test_overlay_loads_after_stock_script_and_buttons_follow_the_last_stock_button(self):
        text = extend_character_create_xml(XML.encode()).decode()
        self.assertIn('<Script file="CharacterCreate.lua"/>\r\n\t<Script file="CoACustomRaces.lua"/>', text)
        positions = [text.index(f'name="CharCreateRaceButton{index}"') for index in range(10, 17)]
        self.assertEqual(positions, sorted(positions))
        self.assertIn('id="16"', text)
        self.assertNotIn('id="17"', text)
        self.assertNotIn('\n\n', text.replace('\r', ''))

    def test_unknown_or_already_patched_sources_fail(self):
        patched = extend_character_create_xml(XML.encode())
        for source in (patched, XML.replace('CharacterCreate.lua', 'Other.lua').encode(),
                       XML.replace('CharCreateRaceButton11', 'CharCreateRaceButtonX').encode()):
            with self.assertRaises(ValueError):
                extend_character_create_xml(source)

    def test_overlay_supports_sixteen_races(self):
        text = OVERLAY_SOURCE.read_text(encoding='utf-8')
        self.assertIn(f'MAX_RACES = {10 + len(RACES)}', text)


TOC = '..\\SharedXML\\AtlasInfo.lua\nDressUpFrame.xml\n\n## add new modules above here\nData\\SpellRanks.lua\n'


class FrameTocTest(unittest.TestCase):
    def test_overlay_loads_before_the_module_marker(self):
        lines = extend_frame_toc(TOC.encode()).decode().split('\n')
        self.assertEqual(lines[lines.index('## add new modules above here') - 1], 'CoACustomRaces.lua')
        self.assertLess(lines.index('DressUpFrame.xml'), lines.index('CoACustomRaces.lua'))

    def test_patched_or_unknown_toc_fails(self):
        for source in (extend_frame_toc(TOC.encode()), TOC.replace('## add new modules above here', '').encode()):
            with self.assertRaises(ValueError):
                extend_frame_toc(source)

    def test_frame_overlay_covers_every_custom_race(self):
        text = FRAME_OVERLAY_SOURCE.read_text(encoding='utf-8')
        ids = sorted(int(value) for value in re.findall(r'id = (\d+)', text))
        self.assertEqual(ids, sorted(RACES))


def outfit(record_id, race, cls, sex, item):
    values = [0] * 72
    values[0] = item
    return struct.pack('<I4B72I', record_id, race, cls, sex, 0, *values)


class StartOutfitTest(unittest.TestCase):
    def test_custom_races_get_missing_base_race_outfits_only(self):
        rows = [outfit(1, 2, 1, 0, 100), outfit(2, 2, 30, 0, 130), outfit(3, 2, 30, 1, 131),
                outfit(4, 19, 1, 0, 900), outfit(5, 1, 30, 0, 230), outfit(6, 8, 30, 1, 330),
                outfit(7, 3, 30, 0, 430)]
        result = {}
        ids = []
        for row in records(extend_start_outfits(dbc(rows, 77, 296, b'')), 296):
            ids.append(struct.unpack_from('<I', row)[0])
            result[tuple(row[4:7])] = struct.unpack_from('<I', row, 8)[0]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertEqual(result[(19, 1, 0)], 900)
        self.assertEqual(result[(19, 30, 0)], 130)
        self.assertEqual(result[(19, 30, 1)], 131)
        self.assertEqual(result[(13, 30, 0)], 230)
        self.assertEqual(result[(12, 30, 1)], 330)
        self.assertEqual(result[(20, 30, 0)], 430)
        self.assertNotIn((13, 1, 0), result)

    def test_complete_file_fails(self):
        with self.assertRaises(ValueError):
            extend_start_outfits(dbc([outfit(1, 5, 1, 0, 1)], 77, 296, b''))


class ServerDataTest(unittest.TestCase):
    def test_client_settings_match_the_server_migration(self):
        text = SQL.read_text(encoding='utf-8')
        mapping = dict(map(int, pair) for pair in re.findall(r'^\((\d+), (\d+)\)[,;]$', text, re.M))
        self.assertEqual(mapping, {race: settings['base'] for race, settings in RACES.items()})
        rows = re.findall(r'^\((\d+), (\d+), (\d+), \d+, \d+, \d+, \'\', (\d+), \d+, \d+, \d+, \'\', \d+, (\d+),',
                          text, re.M)
        server = {int(r[0]): dict(faction=int(r[2]), language=int(r[3]), alliance=int(r[4])) for r in rows}
        self.assertEqual(server, {race: {k: v for k, v in settings.items() if k != 'base'}
                                  for race, settings in RACES.items()})


if __name__ == '__main__':
    unittest.main()
