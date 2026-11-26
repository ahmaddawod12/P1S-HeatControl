import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from validate_firebase_config import validate_firebase_config


class FirebaseConfigTests(unittest.TestCase):
    def setUp(self):
        self.config = {
            "P1S_FIREBASE_ENABLED": True,
            "MBEDTLS_HAVE_TIME_DATE": True,
            "P1S_FIREBASE_DATABASE_URL": "https://fixture-default-rtdb.firebaseio.com",
            "P1S_FIREBASE_API_KEY": "fixture-key",
            "P1S_FIREBASE_PAIRING_URL": "https://fixture.web.app/api/pairing",
            "P1S_FIREBASE_SETUP_PASSPHRASE": "fixture-setup",
        }

    def test_accepts_a_pilot_without_compiled_wifi_credentials(self):
        validate_firebase_config(self.config)

    def test_firebase_disabled_needs_no_project_settings(self):
        validate_firebase_config({"P1S_FIREBASE_ENABLED": False})

    def test_pilot_requires_usable_firebase_settings(self):
        cases = {
            "MBEDTLS_HAVE_TIME_DATE": (False,),
            "P1S_FIREBASE_DATABASE_URL": (
                "http://fixture.firebaseio.com", "https://fixture.example", "https://fixture.firebaseio.com/path",
            ),
            "P1S_FIREBASE_API_KEY": ("key?param", "a" * 129),
            "P1S_FIREBASE_PAIRING_URL": (
                "http://fixture.web.app/api/pairing", "https://fixture.web.app/api/pairing/",
            ),
            "P1S_FIREBASE_SETUP_PASSPHRASE": ("short", "é" * 32),
        }
        for field, invalid_values in cases.items():
            missing = self.config.copy()
            del missing[field]
            with self.subTest(field=field, missing=True):
                with self.assertRaisesRegex(ValueError, field):
                    validate_firebase_config(missing)
            for value in ("", *invalid_values):
                with self.subTest(field=field, value=value):
                    with self.assertRaisesRegex(ValueError, field):
                        validate_firebase_config({**self.config, field: value})
