#!/usr/bin/env python3
"""Build pinned glslang's C API with llvm-mingw, without a system CMake install.

Uses the source groups declared by glslang 15.1.0's own CMakeLists files. HLSL,
SPIRV-Tools optimization, executables, and tests are intentionally excluded.
"""
import concurrent.futures
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'toolchain/vulkan'

def source_group(cmake, group):
    match = re.search(r'set\(' + group + r'\s+(.*?)\)', cmake.read_text(encoding='utf-8'), re.S)
    if not match: raise ValueError('Missing upstream source group: ' + group)
    return [cmake.parent / value.replace('${CMAKE_CURRENT_SOURCE_DIR}/', '')
            for value in match[1].split() if value.endswith('.cpp')]

def main():
    source = BASE / 'glslang'
    build = BASE / 'glslang-build'
    obj = build / 'objects'
    obj.mkdir(parents=True, exist_ok=True)
    info = (source / 'build_info.h.tmpl').read_text(encoding='utf-8')
    for key, value in {'major': '15', 'minor': '1', 'patch': '0', 'flavor': ''}.items():
        info = info.replace('@' + key + '@', value)
    generated = build / 'glslang/build_info.h'
    generated.parent.mkdir(parents=True, exist_ok=True)
    if not generated.exists() or generated.read_text(encoding='utf-8') != info:
        generated.write_text(info, encoding='utf-8')
    cmake = source / 'glslang/CMakeLists.txt'
    files = []
    for group in ('GENERICCODEGEN_SOURCES', 'MACHINEINDEPENDENT_SOURCES', 'GLSLANG_SOURCES', 'RESOURCELIMITS_SOURCES'):
        files.extend(source_group(cmake, group))
    files.extend(source_group(source / 'glslang/OSDependent/Windows/CMakeLists.txt', 'OSDEPENDENT_SOURCES'))
    files.extend(source_group(source / 'SPIRV/CMakeLists.txt', 'SPIRV_SOURCES'))
    tc = ROOT / 'toolchain/llvm-mingw-20261006-ucrt-x86_64/bin'
    flags = ['-std=c++17', '-O2', '-DNDEBUG', '-DENABLE_SPIRV=1', '-DENABLE_OPT=0',
             '-fno-strict-aliasing', '-w', '-I' + str(source), '-I' + str(build)]
    def compile_file(path):
        output = obj / (str(path.relative_to(source)).replace('\\', '_').replace('/', '_') + '.o')
        if not output.exists() or output.stat().st_mtime < max(path.stat().st_mtime, Path(__file__).stat().st_mtime):
            print('glslang:', path.relative_to(source), flush=True)
            result = subprocess.run([str(tc / 'i686-w64-mingw32-clang++.exe'), *flags,
                                     '-c', str(path), '-o', str(output)], capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(str(path) + '\n' + result.stderr)
        return str(output)
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        objects = list(pool.map(compile_file, files))
    subprocess.run([str(tc / 'llvm-ar.exe'), 'rcs', str(build / 'glslang/libglslang.a'), *objects], check=True)
    print('32-bit glslang static library ready.', flush=True)

if __name__ == '__main__': main()
