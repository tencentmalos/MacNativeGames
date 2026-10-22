#!/usr/bin/env python3
"""Cross-platform counterpart of publish_native.sh: build totk_xlate with NativeAOT for any .NET runtime identifier.

publish_native.sh builds the macos-arm64 program the app ships. This script builds the same sources (the vendored
Ryujinx translator, patches/*.patch, Service.cs, Program.cs) for another runtime identifier, so the offline half of
the pipeline (prepare_shader_seed.py, batch translation, reflection) runs on Windows or Linux as well. The app itself
still needs Apple silicon; nothing here changes it.

NativeAOT needs the platform linker: on Windows, Visual Studio's C++ build tools (found through vswhere); on Linux,
clang and the zlib development files. The first run needs network access for the NativeAOT compiler package.

usage: publish_native.py --rid win-x64 [--dotnet dotnet] [--output DIR]
       -> games/totk/prebuilt/<rid>/totk_xlate[.exe] and its .sha256 (not committed, see .gitignore)
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--rid', required=True, help='.NET runtime identifier, e.g. win-x64, linux-x64, osx-arm64')
    parser.add_argument('--dotnet', default='dotnet')
    parser.add_argument('--output', type=Path, help='default: games/totk/prebuilt/<rid>')
    args = parser.parse_args()

    output = args.output or REPO / 'games/totk/prebuilt' / args.rid
    exe = 'totk_xlate.exe' if args.rid.startswith('win-') else 'totk_xlate'
    with tempfile.TemporaryDirectory(prefix='totk_xlate_') as tmp:
        work = Path(tmp)
        ryujinx = work / 'ryujinx-src'
        shutil.copytree(REPO / 'vendor/ryujinx', ryujinx, ignore=shutil.ignore_patterns('obj', 'bin', '.DS_Store'))
        for patch in sorted((HERE / 'patches').glob('*.patch')):
            # git apply works outside a repository and needs no separate patch tool on Windows.
            subprocess.run(['git', 'apply', '-p1', '--whitespace=nowarn', str(patch)], cwd=ryujinx, check=True)
        project = work / 'xlate'
        project.mkdir()
        for source in list(HERE.glob('*.cs')) + [HERE / 'totk_xlate.csproj']:
            shutil.copy2(source, project)
        out = work / 'out'
        env = dict(os.environ)
        if os.name == 'nt' and not shutil.which('vswhere'):
            # The NativeAOT linker lookup calls vswhere.exe by name; an install that never put it on PATH otherwise
            # ends up with vswhere's error text spliced into the link command.
            installer = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer'
            env['PATH'] = f'{installer}{os.pathsep}{env.get("PATH", "")}'
        subprocess.run([args.dotnet, 'publish', str(project / 'totk_xlate.csproj'), '-c', 'Release', '-r', args.rid,
                        '-p:PublishAot=true', '-p:InvariantGlobalization=true', f'-p:RyuSrc={ryujinx}',
                        '-o', str(out)], check=True, env=env)
        output.mkdir(parents=True, exist_ok=True)
        shutil.copy2(out / exe, output / exe)
    digest = hashlib.sha256((output / exe).read_bytes()).hexdigest()
    (output / (exe + '.sha256')).write_text(digest + '\n')
    print(f'built {output / exe} (sha256 {digest})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
