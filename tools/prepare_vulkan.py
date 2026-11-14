#!/usr/bin/env python3
"""Fetch pinned Vulkan build dependencies and build glslang for 32-bit Windows.

Sources/tools stay under ignored toolchain/vulkan; no system installation.
"""
import concurrent.futures
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'toolchain/vulkan'
SOURCES = {
    'headers': ('KhronosGroup/Vulkan-Headers', 'vulkan-sdk-1.3.296.0'),
    'volk': ('zeux/volk', 'vulkan-sdk-1.3.296.0'),
    'vma': ('GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator', 'v3.1.0'),
    'reflect': ('KhronosGroup/SPIRV-Reflect', 'vulkan-sdk-1.3.296.0'),
    'glslang': ('KhronosGroup/glslang', '15.1.0'),
}
SHA256 = {
    'headers': '533a8e17e4725a70213dda1790e0c740794f4fa1e8fe3279b53406fcb5cfec97',
    'volk': 'c93e38a457e231b2e9dc9e2f0b63ed6bb5f5bb262b490bd88cedde36f6b2c998',
    'vma': '897ead99d232d35c786d07ad21472a0a15a2c1852e190b68e0b3d507ccc55be5',
    'reflect': '0b25b1beccbbea2c9f605ff9523cd6903776358ac9742e8dfc57b3652cd5648e',
    'glslang': 'c4cc5083dd198640f3ad9d2014ac9b31433495a57cd3fb2420cd4e8ea8eb4b2b',
}

def unpack(name, url):
    target = BASE / name
    marker = target / '.complete'
    if marker.exists():
        if marker.read_text().strip() != SHA256[name]: raise ValueError('Dependency marker mismatch: ' + name)
        return target
    archive = BASE / (name + '.zip')
    if not archive.exists():
        print('Downloading', name, flush=True)
        data = urllib.request.urlopen(url, timeout=120).read()
        archive.write_bytes(data)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != SHA256[name]: raise ValueError('Dependency checksum mismatch: ' + name)
    print(name, 'sha256', digest, flush=True)
    target.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as z:
        names = z.namelist()
        first = {p.split('/')[0] for p in names if p}
        prefix = next(iter(first)) + '/' if len(first) == 1 and any('/' in p for p in names) else ''
        for entry in z.infolist():
            relative = entry.filename.removeprefix(prefix)
            if not relative: continue
            out = (target / relative).resolve()
            if not out.is_relative_to(target.resolve()): raise ValueError('Unsafe archive path')
            if entry.is_dir(): out.mkdir(parents=True, exist_ok=True)
            else:
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_bytes(z.read(entry))
    marker.write_text(digest + '\n', encoding='utf-8')
    return target

def main():
    BASE.mkdir(parents=True, exist_ok=True)
    jobs = [(name, f'https://codeload.github.com/{repo}/zip/refs/tags/{tag}') for name, (repo, tag) in SOURCES.items()]
    with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:
        list(pool.map(lambda job: unpack(*job), jobs))
    subprocess.run([sys.executable, str(ROOT / 'tools/build_glslang.py')], check=True)
    print('Vulkan dependencies ready:', BASE, flush=True)

if __name__ == '__main__': main()
