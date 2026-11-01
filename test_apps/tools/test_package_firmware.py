import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from package_firmware import package_firmware, verify_package, validate_bootstrap

NAME = "p1s-heat-controller-esp32-4mb.bin"
PARTITIONS = [
    ("nvs", 1, 2, 0x9000, 0x6000), ("phy_init", 1, 1, 0xF000, 0x1000),
    ("device_cfg", 1, 2, 0x10000, 0x6000), ("otadata", 1, 0, 0x16000, 0x2000),
    ("ota_0", 0, 0x10, 0x20000, 0x1F0000), ("ota_1", 0, 0x11, 0x210000, 0x1F0000),
]

def secure(*args):
    subprocess.run([sys.executable, "-m", "espsecure", *map(str, args)], check=True, capture_output=True)

class PackageFirmwareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.keys = tempfile.TemporaryDirectory()
        cls.private = Path(cls.keys.name) / "fixture_private.pem"
        cls.public = Path(cls.keys.name) / "fixture_public.bin"
        secure("generate-signing-key", "--version", "1", cls.private)
        secure("extract-public-key", "--keyfile", cls.private, cls.public)

    @classmethod
    def tearDownClass(cls):
        cls.keys.cleanup()

    def setUp(self):
        import esptool.bin_image as image
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.build = Path(self.temp.name) / "build"
        self.output = Path(self.temp.name) / "package"
        for directory in (self.build / "config", self.build / "partition_table", self.build / "bootloader"):
            directory.mkdir(parents=True)
        self.config = {"IDF_TARGET": "esp32", "ESPTOOLPY_FLASHSIZE": "4MB",
            "BOOTLOADER_APP_ROLLBACK_ENABLE": True, "SECURE_SIGNED_APPS_NO_SECURE_BOOT": True,
            "SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT": True, "SECURE_BOOT": False,
            "P1S_WIFI_SSID": "", "P1S_WIFI_PASSWORD": "", "P1S_RELAY_ACTIVE_LOW": True}
        self.write_config()
        (self.build / "CMakeCache.txt").write_text("P1S_BOOTSTRAP_CREDENTIALS:BOOL=OFF\n")
        (self.build / "project_description.json").write_text(json.dumps({"project_name": "p1s_heat_controller",
            "project_version": "1.0.0", "target": "esp32", "app_bin": "p1s_heat_controller.bin"}))
        table = b"".join(struct.pack("<HBBII16sI", 0x50AA, kind, sub, offset, size, name.encode(), 0)
            for name, kind, sub, offset, size in PARTITIONS)
        (self.build / "partition_table/partition-table.bin").write_bytes(table + b"\xff" * 32)
        (self.build / "bootloader/bootloader.bin").write_bytes(b"boot")
        data = bytearray(768)
        struct.pack_into("<I", data, 0, 0xABCD5432)
        data[16:48] = b"1.0.0".ljust(32, b"\0")
        data[48:80] = b"p1s_heat_controller".ljust(32, b"\0")
        data[112:144] = b"v6.1".ljust(32, b"\0")
        data[256:372] = struct.pack("<II24s24s24s32sI", 0x31533150, 2,
            b"p1s-heat-controller", b"esp32-wroom-32", b"p1s-4m-ota-v1", b"1.0.0", 0)
        firmware = image.ESP32FirmwareImage()
        firmware.entrypoint = 0x40080000
        firmware.flash_size_freq = 0x20
        firmware.flash_mode = 2
        firmware.segments = [image.ImageSegment(0x3F400020, bytes(data))]
        self.firmware, self.data = firmware, data
        self.sign_image()

    def sign_image(self, personal=False):
        # Same product/version and valid signer, but a signed personal-purpose
        # marker and a compiled seed. Generic metadata must not mask this.
        struct.pack_into("<I", self.data, 368, 3 if personal else 0)
        if personal:
            self.data[400:424] = b"fixture-network-password"
        self.firmware.segments[0].data = bytes(self.data)
        unsigned = self.build / "p1s_heat_controller.bin"
        self.firmware.save(str(unsigned))
        secure("sign-data", "--version", "1", "--keyfile", self.private,
               "--output", self.build / NAME, unsigned)

    def test_rejects_personal_signed_image_with_generic_metadata(self):
        self.sign_image(personal=True)
        with self.assertRaises(ValueError):
            package_firmware(self.build, self.public, self.output, "1.0.0")

    def test_publish_rejects_personal_image_with_matching_manifest(self):
        import hashlib
        manifest = package_firmware(self.build, self.public, self.output, "1.0.0")
        self.sign_image(personal=True)
        blob = (self.build / NAME).read_bytes()
        (self.output / NAME).write_bytes(blob)
        manifest.update(size=len(blob), sha256=hashlib.sha256(blob).hexdigest())
        (self.output / "firmware.json").write_text(json.dumps(manifest))
        with self.assertRaises(ValueError): verify_package(self.output, self.public, "1.0.0")

    def test_signed_image_must_match_build_application(self):
        (self.build / "p1s_heat_controller.bin").write_bytes(b"different-build-output")
        with self.assertRaises(ValueError):
            package_firmware(self.build, self.public, self.output, "1.0.0")

    def write_config(self):
        (self.build / "config/sdkconfig.json").write_text(json.dumps(self.config))

    def test_signed_asset_manifest_matches_bytes(self):
        import hashlib
        manifest = package_firmware(self.build, self.public, self.output, "1.0.0")
        self.assertEqual({p.name for p in self.output.iterdir()}, {NAME, "firmware.json"})
        blob = (self.output / NAME).read_bytes()
        self.assertEqual(manifest["sha256"], hashlib.sha256(blob).hexdigest())
        self.assertEqual(manifest["size"], len(blob))
        self.assertEqual(verify_package(self.output, self.public, "1.0.0"), manifest)

    def test_rejects_bootstrap_or_secret_config(self):
        (self.build / "CMakeCache.txt").write_text("P1S_BOOTSTRAP_CREDENTIALS:BOOL=ON\n")
        with self.assertRaises(ValueError): package_firmware(self.build, self.public, self.output, "1.0.0")
        (self.build / "CMakeCache.txt").write_text("P1S_BOOTSTRAP_CREDENTIALS:BOOL=OFF\n")
        self.config["P1S_WIFI_PASSWORD"] = "fixture-personal-password"
        self.write_config()
        with self.assertRaises(ValueError): package_firmware(self.build, self.public, self.output, "1.0.0")

    def test_rejects_wrong_key_or_unsigned_image(self):
        private = Path(self.temp.name) / "untrusted.pem"
        public = Path(self.temp.name) / "untrusted.bin"
        secure("generate-signing-key", "--version", "1", private)
        secure("extract-public-key", "--keyfile", private, public)
        with self.assertRaisesRegex(ValueError, "signature"):
            package_firmware(self.build, public, self.output, "1.0.0")
        signed = self.build / NAME
        original = signed.read_bytes()
        signed.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
        with self.assertRaisesRegex(ValueError, "signature"):
            package_firmware(self.build, self.public, self.output, "1.0.0")
        (self.build / NAME).write_bytes((self.build / "p1s_heat_controller.bin").read_bytes())
        with self.assertRaises(ValueError): package_firmware(self.build, self.public, self.output, "1.0.0")

    def test_rejects_wrong_board_version_layout_or_oversize(self):
        self.config["IDF_TARGET"] = "esp32c3"; self.write_config()
        with self.assertRaises(ValueError): package_firmware(self.build, self.public, self.output, "1.0.0")
        self.config["IDF_TARGET"] = "esp32"; self.write_config()
        with self.assertRaises(ValueError): package_firmware(self.build, self.public, self.output, "1.0.1")
        (self.build / NAME).write_bytes(b"x" * (0x1F0000 + 1))
        with self.assertRaises(ValueError): package_firmware(self.build, self.public, self.output, "1.0.0")

    def test_rejects_changed_partition_layout(self):
        table = self.build / "partition_table/partition-table.bin"
        blob = bytearray(table.read_bytes())
        struct.pack_into("<I", blob, 4 * 32 + 4, 0x30000)
        table.write_bytes(blob)
        with self.assertRaisesRegex(ValueError, "layout"):
            package_firmware(self.build, self.public, self.output, "1.0.0")

    def test_bootstrap_requires_explicit_local_credentials(self):
        with self.assertRaises(ValueError): validate_bootstrap(self.build, self.public, "1.0.0")
        self.config["P1S_WIFI_SSID"] = "fixture-network"
        self.config["P1S_WIFI_PASSWORD"] = "fixture-password"
        self.write_config()
        (self.build / "CMakeCache.txt").write_text("P1S_BOOTSTRAP_CREDENTIALS:BOOL=ON\n")
        self.sign_image(personal=True)
        self.assertEqual(validate_bootstrap(self.build, self.public, "1.0.0")["version"], "1.0.0")
        with self.assertRaises(ValueError): package_firmware(self.build, self.public, self.output, "1.0.0")

    def test_publish_allowlist_excludes_source(self):
        package_firmware(self.build, self.public, self.output, "1.0.0")
        (self.output / "private_source.c").write_text("not for publication")
        with self.assertRaises(ValueError): verify_package(self.output, self.public, "1.0.0")

if __name__ == "__main__":
    unittest.main()
