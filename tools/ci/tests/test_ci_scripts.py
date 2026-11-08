"""Unit tests for the release tooling in tools/ci/. Stdlib only:

    python3 -m unittest discover -s tools/ci/tests -v
"""

import hashlib
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

CI = Path(__file__).resolve().parents[1]
REPO = CI.parents[1]
sys.path.insert(0, str(CI))

import merge_firmware  # noqa: E402
import restore_pio_build_scripts  # noqa: E402


def run(script: str, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(CI / script), *args],
                          capture_output=True, text=True)


def write_release(dist: Path, env: str, version: str, chip: str = "esp32s3",
                  payload: bytes = b"\xe9" + b"\0" * 63, sha: str | None = None) -> None:
    image = dist / f"{env}-{version}.bin"
    image.write_bytes(payload)
    (dist / f"{env}-{version}.json").write_text(json.dumps({
        "env": env, "chip": chip, "flash_size": "16MB", "file": image.name,
        "sha256": sha or hashlib.sha256(payload).hexdigest(),
    }))


class MergeFirmwareTest(unittest.TestCase):
    def test_chip_comes_from_the_bootloader_extended_header(self):
        with tempfile.TemporaryDirectory() as d:
            for chip_id, name in [(0x0009, "esp32s3"), (0x000D, "esp32c6")]:
                header = bytearray(24)
                header[0] = 0xE9
                struct.pack_into("<H", header, 12, chip_id)
                p = Path(d) / f"{name}.bin"
                p.write_bytes(bytes(header))
                self.assertEqual(merge_firmware.chip_from_bootloader(p), name)

    def test_non_image_bootloader_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "junk.bin"
            p.write_bytes(b"\0" * 24)
            with self.assertRaises(ValueError):
                merge_firmware.chip_from_bootloader(p)

    def test_app_offset_is_the_first_app_partition(self):
        def entry(ptype, subtype, offset, size, label):
            return struct.pack("<HBBII16sI", 0x50AA, ptype, subtype, offset, size,
                               label.encode(), 0)
        table = (entry(1, 2, 0x9000, 0x5000, "nvs")
                 + entry(1, 0, 0xE000, 0x2000, "otadata")
                 + entry(0, 0x10, 0x10000, 0x640000, "app0")
                 + entry(0, 0x11, 0x650000, 0x640000, "app1")
                 + b"\xeb\xeb" + b"\xff" * 14 + b"\0" * 16  # md5 entry
                 + b"\xff" * 32)
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "partitions.bin"
            p.write_bytes(table)
            self.assertEqual(merge_firmware.app_offset_from_partitions(p), 0x10000)

    def test_frequency_is_normalized_like_the_platform(self):
        self.assertEqual(merge_firmware.normalize_freq("80000000L"), "80m")
        self.assertEqual(merge_firmware.normalize_freq(40000000), "40m")


class RestorePioBuildScriptsTest(unittest.TestCase):
    def test_backup_replaces_the_edited_build_script(self):
        with tempfile.TemporaryDirectory() as d:
            mcu = Path(d) / "framework-arduinoespressif32-libs" / "esp32"
            mcu.mkdir(parents=True)
            (mcu / "pioarduino-build.py").write_text("edited")
            (mcu / "pioarduino-build.py.esp32").write_text("original")
            self.assertEqual(restore_pio_build_scripts.restore(Path(d)),
                             [mcu / "pioarduino-build.py"])
            self.assertEqual((mcu / "pioarduino-build.py").read_text(), "original")
            self.assertFalse((mcu / "pioarduino-build.py.esp32").exists())

    def test_a_second_installed_version_is_restored_too(self):
        with tempfile.TemporaryDirectory() as d:
            mcu = Path(d) / "framework-arduinoespressif32-libs@5.5.4" / "esp32"
            mcu.mkdir(parents=True)
            (mcu / "pioarduino-build.py").write_text("edited")
            (mcu / "pioarduino-build.py.esp32").write_text("original")
            self.assertEqual(restore_pio_build_scripts.restore(Path(d)),
                             [mcu / "pioarduino-build.py"])
            self.assertEqual((mcu / "pioarduino-build.py").read_text(), "original")

    def test_only_the_platforms_backup_name_counts(self):
        with tempfile.TemporaryDirectory() as d:
            mcu = Path(d) / "framework-arduinoespressif32-libs" / "esp32s3"
            mcu.mkdir(parents=True)
            (mcu / "pioarduino-build.py").write_text("original")
            (mcu / "pioarduino-build.py.bak").write_text("someone's copy")
            self.assertEqual(restore_pio_build_scripts.restore(Path(d)), [])
            self.assertEqual((mcu / "pioarduino-build.py").read_text(), "original")
            self.assertTrue((mcu / "pioarduino-build.py.bak").exists())

    def test_untouched_packages_are_left_alone(self):
        with tempfile.TemporaryDirectory() as d:
            mcu = Path(d) / "framework-arduinoespressif32-libs" / "esp32s3"
            mcu.mkdir(parents=True)
            (mcu / "pioarduino-build.py").write_text("original")
            self.assertEqual(restore_pio_build_scripts.restore(Path(d)), [])
            self.assertEqual((mcu / "pioarduino-build.py").read_text(), "original")
            self.assertEqual(restore_pio_build_scripts.restore(Path(d) / "missing"), [])


class ManifestTest(unittest.TestCase):
    def test_manifest_lists_every_board_with_hash_and_display_name(self):
        with tempfile.TemporaryDirectory() as d:
            dist = Path(d)
            write_release(dist, "waveshare_amoled_216", "v9.9.9")
            write_release(dist, "waveshare_amoled_18_c6", "v9.9.9", chip="esp32c6")
            r = run("make_manifest.py", "--version", "v9.9.9", "--dist", d,
                    "--expect", '["waveshare_amoled_216", "waveshare_amoled_18_c6"]')
            self.assertEqual(r.returncode, 0, r.stderr)
            m = json.loads((dist / "manifest.json").read_text())
            self.assertEqual(m["version"], "v9.9.9")
            self.assertEqual({b["env"] for b in m["boards"]},
                             {"waveshare_amoled_216", "waveshare_amoled_18_c6"})
            for b in m["boards"]:
                self.assertEqual(b["offset"], 0)
                self.assertNotEqual(b["display_name"], b["env"])
                self.assertEqual(b["sha256"],
                                 hashlib.sha256((dist / b["file"]).read_bytes()).hexdigest())

    def test_hash_mismatch_fails(self):
        with tempfile.TemporaryDirectory() as d:
            write_release(Path(d), "waveshare_amoled_216", "v1.0.0", sha="0" * 64)
            r = run("make_manifest.py", "--version", "v1.0.0", "--dist", d)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("sha256", r.stderr)

    def test_missing_expected_env_fails(self):
        with tempfile.TemporaryDirectory() as d:
            write_release(Path(d), "waveshare_amoled_216", "v1.0.0")
            r = run("make_manifest.py", "--version", "v1.0.0", "--dist", d,
                    "--expect", '["waveshare_amoled_216", "waveshare_lcd_4"]')
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("waveshare_lcd_4", r.stderr)

    def test_unknown_env_ships_under_its_env_name_with_a_warning(self):
        with tempfile.TemporaryDirectory() as d:
            write_release(Path(d), "some_new_board", "v1.0.0")
            r = run("make_manifest.py", "--version", "v1.0.0", "--dist", d)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("warning", r.stderr)
            m = json.loads((Path(d) / "manifest.json").read_text())
            self.assertEqual(m["boards"][0]["display_name"], "some_new_board")

    def test_every_hardware_env_has_a_display_name(self):
        sys.path.insert(0, str(CI))
        import make_manifest
        envs = json.loads(run("hardware_envs.py").stdout)
        self.assertTrue(envs)
        self.assertEqual(sorted(set(envs) - set(make_manifest.DISPLAY_NAMES)), [])


class WebToolsManifestTest(unittest.TestCase):
    def test_one_single_build_manifest_per_board(self):
        with tempfile.TemporaryDirectory() as d:
            dist = Path(d)
            write_release(dist, "waveshare_amoled_216", "v2.0.0")
            write_release(dist, "waveshare_amoled_216_c6", "v2.0.0", chip="esp32c6")
            self.assertEqual(run("make_manifest.py", "--version", "v2.0.0",
                                 "--dist", d).returncode, 0)
            r = run("make_webtools_manifest.py", "--dist", d,
                    "--base-url", "https://example.invalid/dl/")
            self.assertEqual(r.returncode, 0, r.stderr)
            c6 = json.loads((dist / "webtools-waveshare_amoled_216_c6.json").read_text())
            self.assertEqual(len(c6["builds"]), 1)
            self.assertEqual(c6["builds"][0]["chipFamily"], "ESP32-C6")
            self.assertEqual(c6["builds"][0]["parts"], [{
                "path": "https://example.invalid/dl/waveshare_amoled_216_c6-v2.0.0.bin",
                "offset": 0}])


    def test_nvs_is_split_out_so_an_update_keeps_pairings(self):
        with tempfile.TemporaryDirectory() as d:
            dist = Path(d)
            write_release(dist, "waveshare_amoled_216", "v2.0.0")
            self.assertEqual(run("make_manifest.py", "--version", "v2.0.0",
                                 "--dist", d).returncode, 0)
            # Give the merged image a real partition table: nvs at 0x9000+0x5000.
            image = bytearray(b"\xff" * 0x20000)
            entry = lambda kind, sub, off, size: (b"\xaa\x50" + bytes([kind, sub])
                                                  + struct.pack("<II", off, size)).ljust(32, b"\0")
            table = entry(1, 2, 0x9000, 0x5000) + entry(0, 0x10, 0x10000, 0x10000)
            image[0x8000:0x8000 + len(table)] = table
            (dist / "waveshare_amoled_216-v2.0.0.bin").write_bytes(bytes(image))
            r = run("make_webtools_manifest.py", "--dist", d)
            self.assertEqual(r.returncode, 0, r.stderr)
            web = json.loads((dist / "webtools-waveshare_amoled_216.json").read_text())
            self.assertEqual(web["builds"][0]["parts"], [
                {"path": "waveshare_amoled_216-v2.0.0.part0.bin", "offset": 0},
                {"path": "waveshare_amoled_216-v2.0.0.part1.bin", "offset": 0xE000}])
            self.assertEqual((dist / "waveshare_amoled_216-v2.0.0.part0.bin").read_bytes(),
                             bytes(image[:0x9000]))
            self.assertEqual((dist / "waveshare_amoled_216-v2.0.0.part1.bin").read_bytes(),
                             bytes(image[0xE000:]))


class HardwareEnvsTest(unittest.TestCase):
    def test_native_envs_are_excluded_from_the_hardware_matrix(self):
        ini = ("[env:board_a]\nplatform = https://x/p.zip ; comment\nboard = esp32-s3-devkitc-1\n\n"
               "[env:sim]\nplatform = native\n\n"
               "[env:test_native]\nplatform = native\n")
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "platformio.ini"
            p.write_text(ini)
            self.assertEqual(json.loads(run("hardware_envs.py", "--ini", str(p)).stdout),
                             ["board_a"])
            self.assertEqual(json.loads(run("hardware_envs.py", "--ini", str(p), "--native").stdout),
                             ["sim", "test_native"])
            self.assertEqual(json.loads(run("hardware_envs.py", "--ini", str(p), "--matrix").stdout),
                             [{"env": "board_a", "board": "esp32-s3-devkitc-1"}])


if __name__ == "__main__":
    unittest.main()
