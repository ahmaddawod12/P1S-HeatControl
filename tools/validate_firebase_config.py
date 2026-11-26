"""Validate the settings needed to boot a Firebase-enabled controller."""

import argparse
import json
import re
from pathlib import Path


HTTPS_URL = re.compile(
    r"https://(?P<host>[A-Za-z0-9](?:[A-Za-z0-9.-]{0,251}[A-Za-z0-9])?)(?P<path>/[A-Za-z0-9/_.-]*)?"
)


def validate_firebase_config(config):
    if config.get("P1S_FIREBASE_ENABLED") is not True:
        return

    if config.get("MBEDTLS_HAVE_TIME_DATE") is not True:
        raise ValueError("enable CONFIG_MBEDTLS_HAVE_TIME_DATE to verify Firebase certificate validity dates")

    # Match the firmware's URL and credential bounds.
    database = config.get("P1S_FIREBASE_DATABASE_URL")
    database_url = HTTPS_URL.fullmatch(database) if isinstance(database, str) else None
    if (not database_url or database_url["path"] or
            not database_url["host"].endswith((".firebaseio.com", ".firebasedatabase.app"))):
        raise ValueError("P1S_FIREBASE_DATABASE_URL must be an HTTPS Realtime Database URL without a path")

    api_key = config.get("P1S_FIREBASE_API_KEY")
    if not isinstance(api_key, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", api_key):
        raise ValueError("P1S_FIREBASE_API_KEY is missing or invalid")

    pairing = config.get("P1S_FIREBASE_PAIRING_URL")
    if (not isinstance(pairing, str) or len(pairing) >= 244 or pairing.endswith("/") or
            not HTTPS_URL.fullmatch(pairing)):
        raise ValueError("P1S_FIREBASE_PAIRING_URL must be an HTTPS URL without a trailing slash")

    passphrase = config.get("P1S_FIREBASE_SETUP_PASSPHRASE")
    if config.get("P1S_FIREBASE_PUBLIC_RELEASE") is True:
        if passphrase:
            raise ValueError("public Firebase firmware must generate its setup passphrase on the device")
        return
    if (not isinstance(passphrase, str) or "\0" in passphrase or
            not 8 <= len(passphrase.encode("utf-8")) <= 63):
        raise ValueError("P1S_FIREBASE_SETUP_PASSPHRASE must be 8 to 63 bytes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sdkconfig_json", type=Path)
    args = parser.parse_args()
    try:
        validate_firebase_config(json.loads(args.sdkconfig_json.read_text(encoding="utf-8")))
    except (OSError, ValueError) as exc:
        parser.exit(1, f"Firebase build configuration rejected: {exc}\n")
