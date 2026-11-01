"""Validate and package generic signed P1S firmware; never package setup secrets."""
import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

IMAGE_NAME = "p1s-heat-controller-esp32-4mb.bin"
SLOT_BYTES = 0x1F0000
IDENTITY = struct.Struct("<II24s24s24s32sI")
PARTITIONS = {
    "nvs": (1, 2, 0x9000, 0x6000), "phy_init": (1, 1, 0xF000, 0x1000),
    "device_cfg": (1, 2, 0x10000, 0x6000), "otadata": (1, 0, 0x16000, 0x2000),
    "ota_0": (0, 0x10, 0x20000, SLOT_BYTES), "ota_1": (0, 0x11, 0x210000, SLOT_BYTES),
}

def require(condition, message):
    if not condition:
        raise ValueError(message)

def stable_version(version):
    require(isinstance(version, str) and 0 < len(version) < 32 and
        re.fullmatch(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", version),
        "version must be a stable major.minor.patch value")
    require(all(int(v) <= 0xFFFFFFFF for v in version.split(".")), "version component overflow")

def text(field):
    require(b"\0" in field, "unterminated image identity")
    return field.split(b"\0", 1)[0].decode("ascii")

def verify_signature(image, public_key):
    require(Path(public_key).is_file() and Path(public_key).stat().st_size == 64, "expected raw ECDSA public key")
    result = subprocess.run([sys.executable, "-m", "espsecure", "verify-signature", "--version", "1",
        "--keyfile", str(public_key), str(image)], capture_output=True, text=True)
    require(result.returncode == 0, "firmware signature verification failed")

def image_metadata(image, public_key, version, personal=False):
    stable_version(version)
    image = Path(image)
    require(image.is_file() and 468 <= image.stat().st_size <= SLOT_BYTES, "signed firmware exceeds slot or is too small")
    blob = image.read_bytes()
    require(blob[0] == 0xE9 and struct.unpack_from("<H", blob, 12)[0] == 0, "image is not classic ESP32 firmware")
    require(struct.unpack_from("<I", blob, 32)[0] == 0xABCD5432, "missing application descriptor")
    require(text(blob[48:80]) == version and text(blob[80:112]) == "p1s_heat_controller", "application version/project mismatch")
    require(re.match(r"^v6\.1(?:[.\-]|$)", text(blob[144:176])), "firmware must use ESP-IDF 6.1")
    magic, fmt, product, hardware, layout, embedded, flags = IDENTITY.unpack_from(blob, 288)
    require(magic == 0x31533150 and fmt == 2, "unsupported signed identity")
    require(flags == (3 if personal else 0), "signed image purpose/credentials do not match this operation")
    require((text(product), text(hardware), text(layout), text(embedded)) ==
        ("p1s-heat-controller", "esp32-wroom-32", "p1s-4m-ota-v1", version), "wrong signed board/layout/version")
    verify_signature(image, public_key)
    from esptool.bin_image import LoadFirmwareImage
    try:
        LoadFirmwareImage("esp32", blob).verify()
    except Exception as exc:
        raise ValueError("invalid ESP32 application image") from exc
    return {"format": 1, "product": "p1s-heat-controller", "hardware": "esp32-wroom-32",
        "layout": "p1s-4m-ota-v1", "version": version, "filename": IMAGE_NAME,
        "size": len(blob), "sha256": hashlib.sha256(blob).hexdigest()}

def validate_layout(build):
    blob = (build / "partition_table/partition-table.bin").read_bytes()
    actual = {}
    for position in range(0, len(blob) - 31, 32):
        magic, kind, sub, offset, size, label, flags = struct.unpack_from("<HBBII16sI", blob, position)
        if magic in (0xFFFF, 0xEBEB):
            break
        require(magic == 0x50AA and flags == 0, "invalid or unsupported partition entry")
        name = text(label)
        require(name not in actual, "duplicate partition")
        actual[name] = (kind, sub, offset, size)
    require(actual == PARTITIONS, "build does not use the fixed 4 MB OTA layout")
    bootloader = build / "bootloader/bootloader.bin"
    require(bootloader.is_file() and 0 < bootloader.stat().st_size <= 0x7000,
            "bootloader overlaps partition table")

def validate_generic_build(build, version):
    description = json.loads((build / "project_description.json").read_text(encoding="utf-8"))
    require(description.get("target") == "esp32" and description.get("project_name") == "p1s_heat_controller"
        and description.get("project_version") == version, "wrong project/target/version")
    config = json.loads((build / "config/sdkconfig.json").read_text(encoding="utf-8"))
    require(config.get("P1S_WIFI_SSID") == "" and config.get("P1S_WIFI_PASSWORD") == "",
            "release contains personal Wi-Fi configuration")
    require(config.get("IDF_TARGET") == "esp32" and config.get("ESPTOOLPY_FLASHSIZE") == "4MB"
        and config.get("P1S_RELAY_ACTIVE_LOW") is True, "wrong hardware profile")
    for key in ("BOOTLOADER_APP_ROLLBACK_ENABLE", "SECURE_SIGNED_APPS_NO_SECURE_BOOT", "SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT"):
        require(config.get(key) is True, "required update protection is disabled")
    require(not config.get("SECURE_BOOT") and not config.get("BOOTLOADER_APP_ANTI_ROLLBACK"),
            "release must not introduce hardware security fuse changes")
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    entries = [line.split("=", 1)[1] for line in cache.splitlines() if line.startswith("P1S_BOOTSTRAP_CREDENTIALS:")]
    require(entries == ["OFF"], "personal bootstrap builds cannot be released")
    validate_layout(build)

def validate_bootstrap(build_dir, public_key, version):
    build = Path(build_dir)
    description = json.loads((build / "project_description.json").read_text(encoding="utf-8"))
    config = json.loads((build / "config/sdkconfig.json").read_text(encoding="utf-8"))
    require(description.get("target") == "esp32" and description.get("project_name") == "p1s_heat_controller"
        and description.get("project_version") == version, "wrong bootstrap project/target/version")
    require(config.get("ESPTOOLPY_FLASHSIZE") == "4MB" and config.get("P1S_RELAY_ACTIVE_LOW") is True,
            "wrong bootstrap hardware profile")
    require(config.get("SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT") is True
        and config.get("BOOTLOADER_APP_ROLLBACK_ENABLE") is True and not config.get("SECURE_BOOT")
        and not config.get("BOOTLOADER_APP_ANTI_ROLLBACK"), "wrong bootstrap security settings")
    ssid, password = config.get("P1S_WIFI_SSID", ""), config.get("P1S_WIFI_PASSWORD", "")
    require(isinstance(ssid, str) and 1 <= len(ssid.encode("utf-8")) <= 32 and isinstance(password, str)
        and (len(password.encode("utf-8")) == 0 or 8 <= len(password.encode("utf-8")) <= 63),
        "bootstrap needs valid local Wi-Fi credentials")
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    entries = [line.split("=", 1)[1] for line in cache.splitlines() if line.startswith("P1S_BOOTSTRAP_CREDENTIALS:")]
    require(entries == ["ON"], "expected an explicit personal bootstrap build")
    validate_layout(build)
    validate_build_image(build)
    return image_metadata(build / IMAGE_NAME, public_key, version, personal=True)

def validate_build_image(build):
    # espsecure v1 appends its 4-byte version and 64-byte ECDSA signature.
    signed = (build / IMAGE_NAME).read_bytes()
    unsigned = (build / "p1s_heat_controller.bin").read_bytes()
    require(len(signed) >= 68 and signed[:-68] == unsigned,
            "signed firmware does not match this build's application output")

def verify_package(package_dir, public_key, version):
    directory = Path(package_dir)
    require(directory.is_dir() and {p.name for p in directory.iterdir()} == {IMAGE_NAME, "firmware.json"},
            "release folder must contain exactly firmware.json and the signed application")
    manifest = json.loads((directory / "firmware.json").read_text(encoding="utf-8"))
    expected = image_metadata(directory / IMAGE_NAME, public_key, version)
    require(manifest == expected, "manifest does not describe the signed image")
    return manifest

def package_firmware(build_dir, public_key, output_dir, version):
    build, output = Path(build_dir), Path(output_dir)
    stable_version(version)
    validate_generic_build(build, version)
    validate_build_image(build)
    manifest = image_metadata(build / IMAGE_NAME, public_key, version)
    require(output.resolve() != build.resolve(), "package directory must be separate from build output")
    if output.exists():
        require(output.is_dir() and all(p.is_file() and p.name in {IMAGE_NAME, "firmware.json"}
            for p in output.iterdir()), "refusing to overwrite a folder containing other files")
    output.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(build / IMAGE_NAME, output / IMAGE_NAME)
    (output / "firmware.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return verify_package(output, public_key, version)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--verify-package", type=Path)
    parser.add_argument("--check-bootstrap", type=Path)
    parser.add_argument("--public-key", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    try:
        if args.check_bootstrap:
            manifest = validate_bootstrap(args.check_bootstrap, args.public_key, args.version)
        elif args.verify_package:
            manifest = verify_package(args.verify_package, args.public_key, args.version)
        else:
            require(args.build_dir is not None and args.output is not None, "build directory and output are required")
            manifest = package_firmware(args.build_dir, args.public_key, args.output, args.version)
        print(json.dumps(manifest, indent=2))
    except (ValueError, OSError, KeyError) as exc:
        parser.exit(1, f"Package rejected: {exc}\n")

if __name__ == "__main__":
    main()
