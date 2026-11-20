"""Sign a firmware build and publish it to the server for OTA.

    cd firmware && pio run          # after bumping FW_VERSION in include/config.h
    python tools/sign_firmware.py   # reads FW_VERSION, signs .pio/build/esp32dev/firmware.bin

The signature is ECDSA P-256 / SHA-256 over "riseq-fw:v<version>:<sha256hex>",
so neither the image nor its version number can be changed without the key.
"""
import argparse
import base64
import hashlib
import json
import re
import shutil
from pathlib import Path

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

ROOT = Path(__file__).resolve().parent.parent

ap = argparse.ArgumentParser()
ap.add_argument("--bin", default=ROOT / "firmware/.pio/build/esp32dev/firmware.bin", type=Path)
ap.add_argument("--key", default=ROOT / "keys/ota_private.pem", type=Path)
ap.add_argument("--out", default=ROOT / "server/firmware", type=Path)
ap.add_argument("--version", type=int, help="defaults to FW_VERSION in firmware/include/config.h")
args = ap.parse_args()

version = args.version
if version is None:
    m = re.search(r"#define\s+FW_VERSION\s+(\d+)", (ROOT / "firmware/include/config.h").read_text())
    version = int(m.group(1))

image = args.bin.read_bytes()
digest = hashlib.sha256(image).hexdigest()
key = serialization.load_pem_private_key(args.key.read_bytes(), password=None)
signature = key.sign(f"riseq-fw:v{version}:{digest}".encode(), ec.ECDSA(hashes.SHA256()))

args.out.mkdir(parents=True, exist_ok=True)
shutil.copyfile(args.bin, args.out / "firmware.bin")
manifest = {"version": version, "size": len(image), "sha256": digest,
            "signature": base64.b64encode(signature).decode()}
(args.out / "manifest.json").write_text(json.dumps(manifest, indent=2))
print(f"published v{version} ({len(image)} bytes, sha256 {digest[:16]}…) to {args.out}")
