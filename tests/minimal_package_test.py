#!/usr/bin/env python3
"""Host-only signed OTA package checks. No board/server access and no flashing."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = Path(os.environ.get('MINIMAL_FIRMWARE', str(ROOT / '.pio/build/minimal_agent/firmware.bin')))
CORE = Path(os.environ.get('PLATFORMIO_CORE_DIR', str(Path.home() / '.platformio')))
OFFICIAL_SIGNER = CORE / 'packages/framework-arduinoespressif8266/tools/signing.py'
sys.path.insert(0, str(ROOT / 'tools'))
import minimal_sign

class Packages(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='minimal-sign-test-')
        cls.folder = Path(cls.temp.name)
        cls.secret = cls.folder / 'test-private.pem'
        cls.public = cls.folder / 'test-public.pem'
        minimal_sign.create_key(cls.secret, cls.public)
        cls.image = FIRMWARE.read_bytes()
        cls.package = minimal_sign.signed_bytes(cls.image, cls.secret.read_bytes())

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_round_trip_actual_firmware(self):
        self.assertEqual(minimal_sign.verify_bytes(self.package, self.public.read_bytes()), len(self.image))
        self.assertEqual(len(self.package)-len(self.image), 260)

    def test_tampered_firmware_rejected(self):
        tampered = bytearray(self.package); tampered[128] ^= 1
        with self.assertRaises(Exception):
            minimal_sign.verify_bytes(tampered, self.public.read_bytes())

    def test_wrong_signing_key_rejected(self):
        secret, public = self.folder / 'other-private.pem', self.folder / 'other-public.pem'
        minimal_sign.create_key(secret, public)
        with self.assertRaises(Exception):
            minimal_sign.verify_bytes(self.package, public.read_bytes())

    def test_missing_or_corrupt_trailer_rejected(self):
        for bad in (self.image, self.package[:-1], self.package[:-4] + b'\0\0\0\0'):
            with self.assertRaises(Exception):
                minimal_sign.verify_bytes(bad, self.public.read_bytes())

    def test_uncompressed_esp_image_required(self):
        with self.assertRaises(ValueError):
            minimal_sign.signed_bytes(b'not-an-esp-image', self.secret.read_bytes())

    def test_release_metadata_matches_signed_payload(self):
        record = minimal_sign.manifest(self.package, 'minimal-agent-0.1.0', '/v1/firmware/test.bin.signed')
        self.assertEqual(record['args']['size'], len(self.package))
        self.assertEqual(record['args']['sha256'], hashlib.sha256(self.package).hexdigest())
        self.assertEqual(record['args']['target'], minimal_sign.TARGET)

    def test_artifact_cannot_escape_configured_origin(self):
        for path in ('https://other/file', '/v1/firmware/../file', '/v1/firmware//a', '/v1/firmware/a?token=x'):
            with self.assertRaises(ValueError):
                minimal_sign.manifest(self.package, 'version-1', path)

    def test_existing_release_key_not_overwritten(self):
        before = self.secret.read_bytes()
        with self.assertRaises(FileExistsError):
            minimal_sign.create_key(self.secret, self.public)
        self.assertEqual(self.secret.read_bytes(), before)

    def test_official_framework_signer_compatible(self):
        out = self.folder / 'official.bin.signed'
        self.assertTrue(OFFICIAL_SIGNER.is_file(), 'Build first or set PLATFORMIO_CORE_DIR')
        subprocess.run([sys.executable, str(OFFICIAL_SIGNER), '--mode','sign', '--privatekey',str(self.secret),
                        '--bin',str(FIRMWARE), '--out',str(out)], check=True, capture_output=True)
        self.assertEqual(out.read_bytes(), self.package)

if __name__ == '__main__':
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(Packages)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    report = {'tests':result.testsRun,'failures':len(result.failures),'errors':len(result.errors),
              'physical_board_flashed':False,'actual_ota_flash_tested':False}
    report_dir = ROOT / 'artifacts'
    report_dir.mkdir(exist_ok=True)
    (report_dir/'package-tests.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    raise SystemExit(not result.wasSuccessful())
