#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Launch one native BOTW backend with explicit private data and driver paths."""
import argparse
import hashlib
import json
import os
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--data-root', type=Path, required=True)
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--backend', choices=['metal', 'vulkan'], default='vulkan')
    parser.add_argument('--driver', type=Path)
    parser.add_argument('--shaders', type=Path)
    parser.add_argument('--shader-archive', type=Path, help='Immutable archive made from the selected XGS/XGV bundle')
    parser.add_argument('--metal-assets', type=Path, help='MacNative prepared build assets directory')
    parser.add_argument('--system-mode', choices=['handheld', 'docked'], default='handheld')
    parser.add_argument('--autoload', action='store_true')
    parser.add_argument('--vsync', choices=['on', 'off'], help='Host display synchronization for backend comparisons')
    parser.add_argument('--fps-limit', type=int, choices=range(30, 241), metavar='30..240',
                        help='Own-clock gameplay ceiling; movies retain 30 Hz')
    parser.add_argument('--gpu-keepalive', choices=['on', 'off'], help='Metal GPU keepalive; Vulkan supports off only')
    parser.add_argument('--gpu-capture', type=Path,
                        help='Arm MoltenVK on-demand capture to a new .gputrace path')
    args = parser.parse_args()
    if args.shader_archive and (args.backend!='vulkan' or not args.shader_archive.is_file()):
        parser.error('--shader-archive requires Vulkan and an existing archive')
    executable = args.build.resolve() / ('mng_botw_' + args.backend)
    data = args.data_root.resolve()
    if not executable.is_file() or not (data / 'rewritten/botw160').is_dir():
        parser.error('missing executable or verified BOTW 1.6.0 rewritten modules')
    if args.backend == 'vulkan' and (not args.driver or not args.driver.is_file() or not args.shaders or not args.shaders.is_dir()):
        parser.error('Vulkan requires --driver and --shaders, including XGV variant manifests')
    if args.backend == 'metal' and (not args.metal_assets or not all(
            (args.metal_assets / name).exists() for name in ['shader-seed', 'shader-libs', 'shader-service/totk_xlate', 'spirv-cross/spirv-cross'])):
        parser.error('Metal requires --metal-assets with prepared shaders and native translator tools')
    metadata = None
    if args.backend == 'metal':
        try:
            metadata = json.loads((args.build / 'mng-metal-assets.json').read_text())
            seed = json.loads((args.metal_assets / 'shader-seed/seed-manifest.json').read_text())
            for field in ('chain', 'version', 'game', 'translator_sha256'):
                if seed[field] != metadata[field]:
                    parser.error('Metal seed identity mismatch: ' + field)
            translator = args.metal_assets / 'shader-service/totk_xlate'
            if hashlib.sha256(translator.read_bytes()).hexdigest() != metadata['translator_sha256']:
                parser.error('Metal translator identity mismatch')
        except (OSError, KeyError, ValueError) as error:
            parser.error('Missing or invalid Metal asset identity: ' + str(error))
    profile = args.profile.resolve()
    profile.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env.update(MNG_PROFILE_DIR=str(profile), TOTK_PROFILE_DIR=str(profile),
               TOTK_OPERATION_MODE=args.system_mode, MNG_RENDER_RESOLUTION='720' if args.system_mode == 'handheld' else '1080',
               TOTK_OUTPUT_RES='0', TOTK_LANG='zh-Hans', TOTK_UI_LANG='zh',
               TOTK_AUTOLOAD='1' if args.autoload else '0')
    env.setdefault('TOTK_SLOT_OWNER', 'native-port')
    env.setdefault('TOTK_GAME_SLOT_DIR', str(data / 'analysis/game-slot'))
    if args.vsync:
        env['MNG_VSYNC'] = '1' if args.vsync == 'on' else '0'
        if args.backend == 'vulkan':
            env['MNG_PRESENT_MODE'] = 'fifo' if args.vsync == 'on' else 'immediate'
    if args.fps_limit is not None:
        env.update(MNG_PRESENT_HZ=str(args.fps_limit), TOTK_UNCAPPED='1',
                   TOTK_UNCAPPED_HZ=str(args.fps_limit), TOTK_UNCAPPED_INTERVAL='1',
                   TOTK_REALTIME='1', TOTK_FPS60='1')
        # This legacy setting disables the own-clock UNCAPPED mode.
        env.pop('DM_PRESENT_HZ', None)
    if args.gpu_keepalive:
        if args.backend == 'vulkan' and args.gpu_keepalive == 'on':
            parser.error('Vulkan has no GPU keepalive implementation')
        env['TOTK_GPU_KEEPALIVE'] = '1' if args.gpu_keepalive == 'on' else '0'
    print('[mng-launch] backend={} vsync={} fps_limit={} gpu_keepalive={} system_mode={}'.format(
        args.backend, args.vsync or 'backend-default', args.fps_limit or 'backend-default',
        args.gpu_keepalive or 'backend-default', args.system_mode), flush=True)
    if args.gpu_capture:
        output = args.gpu_capture.resolve()
        if args.backend != 'vulkan' or output.suffix != '.gputrace' or output.exists():
            parser.error('--gpu-capture requires Vulkan and a new .gputrace path')
        output.parent.mkdir(parents=True, exist_ok=True)
        env.update(MTL_CAPTURE_ENABLED='1', MVK_CONFIG_AUTO_GPU_CAPTURE_SCOPE='3',
                   MVK_CONFIG_AUTO_GPU_CAPTURE_OUTPUT_FILE=str(output))
        print('[mng-capture] armed output=' + str(output), flush=True)
    if args.backend == 'vulkan':
        env.update(MNG_VULKAN_LIBRARY=str(args.driver.resolve()), MNG_SHADER_DIR=str(args.shaders.resolve()))
        if args.shader_archive:env['MNG_SHADER_ARCHIVE']=str(args.shader_archive.resolve())
        env.setdefault('MVK_CONFIG_USE_MTLHEAP', '1')
        # Renderer policy lives in porting/nvn_vulkan/runtime_policy.h.
        # This launcher only supplies platform capabilities and user overrides.
    else:
        # Same GPU path defaults as MacNative's app/launcher.m.
        for option in ('TOTK_GPU_ALLOW_SUBMIT', 'TOTK_GPU_WIDEN', 'TOTK_GPU_BLOCKRT', 'TOTK_GPU_MIDCLEAR'):
            env.setdefault(option, '1')
        env.update(MNG_METAL_ASSETS_DIR=str(args.metal_assets.resolve()),
                   TOTK_SHADER_SEED_DIR=str(args.metal_assets.resolve() / 'shader-seed'),
                   TOTK_SHADER_CACHE=str(profile / 'shader-cache' / metadata['chain']), TOTK_SHADER_SERVICE='native')
    os.chdir(data)
    os.execve(executable, [str(executable), 'rewritten/botw160', 'boot'], env)


if __name__ == '__main__':
    main()
