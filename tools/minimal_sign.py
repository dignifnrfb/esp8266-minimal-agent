#!/usr/bin/env python3
"""Create/verify ESP8266 RSA2048 signed images. Never prints private key data."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

TARGET = 'esp8266:nodemcuv2:4m1m'
MAX_PACKAGE = 1048576

def _crypto():
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding, rsa
    return hashes, serialization, padding, rsa

def create_key(private_path, public_path):
    _, serialization, _, rsa = _crypto()
    private_path, public_path = Path(private_path), Path(public_path)
    if private_path.resolve() == public_path.resolve():
        raise ValueError('Private and public paths must differ')
    if private_path.exists() or public_path.exists():
        raise FileExistsError('Refusing to overwrite a signing key')
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    # Exclusive creation avoids accidentally replacing a release signing key.
    with private_path.open('xb') as out:
        out.write(key.private_bytes(serialization.Encoding.PEM,
                  serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    with public_path.open('xb') as out:
        out.write(key.public_key().public_bytes(serialization.Encoding.PEM,
                  serialization.PublicFormat.SubjectPublicKeyInfo))

def signed_bytes(image, private_key_pem):
    hashes, serialization, padding, rsa = _crypto()
    key = serialization.load_pem_private_key(private_key_pem, password=None)
    if not isinstance(key, rsa.RSAPrivateKey) or key.key_size != 2048:
        raise ValueError('RSA2048 private key required')
    if len(image) < 8 or image[0] != 0xe9:
        raise ValueError('Expected an uncompressed ESP8266 application image')
    signature = key.sign(image, padding.PKCS1v15(), hashes.SHA256())
    package = image + signature + struct.pack('<I', len(signature))
    if len(package) > MAX_PACKAGE:
        raise ValueError('Signed package exceeds firmware limit')
    return package

def verify_bytes(package, public_key_pem):
    hashes, serialization, padding, rsa = _crypto()
    key = serialization.load_pem_public_key(public_key_pem)
    if not isinstance(key, rsa.RSAPublicKey) or key.key_size != 2048:
        raise ValueError('RSA2048 public key required')
    if len(package) < 268 or len(package) > MAX_PACKAGE or package[0] != 0xe9:
        raise ValueError('Invalid package size or image magic')
    size = struct.unpack('<I', package[-4:])[0]
    if size != 256:
        raise ValueError('Invalid signature trailer')
    image = package[:-260]
    key.verify(package[-260:-4], image, padding.PKCS1v15(), hashes.SHA256())
    return len(image)

def manifest(package, version, path):
    import re
    if not re.fullmatch(r'[A-Za-z0-9_.-]{1,64}', version):
        raise ValueError('Invalid release version')
    if not re.fullmatch(r'/v1/firmware/[A-Za-z0-9_./-]+', path) or len(path) > 160 or '..' in path or '//' in path:
        raise ValueError('Invalid artifact path')
    return {'type': 'firmware_update', 'args': {
        'path': path, 'version': version, 'target': TARGET,
        'size': len(package), 'sha256': hashlib.sha256(package).hexdigest()}}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    keygen = sub.add_parser('keygen')
    keygen.add_argument('--private', required=True, type=Path)
    keygen.add_argument('--public', required=True, type=Path)
    sign = sub.add_parser('sign')
    sign.add_argument('--private', required=True, type=Path)
    sign.add_argument('--image', required=True, type=Path)
    sign.add_argument('--out', required=True, type=Path)
    sign.add_argument('--version', required=True)
    sign.add_argument('--path', required=True, help='/v1/firmware/<release>.bin.signed')
    verify = sub.add_parser('verify')
    verify.add_argument('--public', required=True, type=Path)
    verify.add_argument('--image', required=True, type=Path)
    args = parser.parse_args()
    try:
        if args.action == 'keygen':
            create_key(args.private, args.public)
            print(json.dumps({'created': True, 'public_key_file': str(args.public)}))
        elif args.action == 'sign':
            if args.out.exists() or args.out.with_suffix(args.out.suffix + '.job.json').exists():
                raise FileExistsError('Release output already exists')
            package = signed_bytes(args.image.read_bytes(), args.private.read_bytes())
            record = manifest(package, args.version, args.path)
            with args.out.open('xb') as out:
                out.write(package)
            args.out.with_suffix(args.out.suffix + '.job.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
            print(json.dumps({'signed': True, 'bytes': len(package), 'sha256': record['args']['sha256']}))
        else:
            size = verify_bytes(args.image.read_bytes(), args.public.read_bytes())
            print(json.dumps({'verified': True, 'application_bytes': size}))
    except Exception as exc:
        parser.exit(1, 'Signing operation failed (%s); key material was not printed.\n' % type(exc).__name__)

if __name__ == '__main__':
    main()
