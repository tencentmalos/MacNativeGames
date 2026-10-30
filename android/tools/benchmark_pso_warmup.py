#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare bounded PSO workers using startup Litep and unprofiled scene cadence."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time
from types import SimpleNamespace
from validate_scene import ROOT, PACKAGE, SERVICE
sys.path.insert(0,str(ROOT/'porting/tools'))
from profiler import Target


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--serial',required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--workers',type=int,nargs='+',default=[1,4])
    p.add_argument('--seconds',type=int,default=20)
    args=p.parse_args()
    if not all(1<=n<=8 for n in args.workers) or not 5<=args.seconds<=120:
        p.error('workers=1..8, seconds=5..120')
    args.output.mkdir(parents=True,exist_ok=False)
    t=Target(SimpleNamespace(adb='adb',serial=args.serial,package=PACKAGE,
        service=SERVICE,process_suffix=':game',expected_pid=None))
    def ctl(*cmd):
        for attempt in range(80):
            try:return t.control(cmd)
            except ValueError as e:
                if str(e)!='emulation_session_busy' or attempt==79:raise
                time.sleep(.05)
    props=('debug.mng.pso_workers','debug.mng.profile_startup','debug.mng.autoload')
    previous={key:t.adb('shell','getprop',key).strip() for key in props}
    report=dict(serial=args.serial,cases=[],state='running',
        cache_policy='existing driver cache and PSO list preserved; not a cold-driver-cache test',
        scene_identity='autoload latest save; exact map and pose not inferred')
    def cache_hashes():
        paths=t.adb('shell','run-as',PACKAGE,'find','no_backup/game-assets/botw160/shader-bundles','-maxdepth','2','-name','vk-pipeline-list.bin').splitlines()
        return [t.adb('shell','run-as',PACKAGE,'sha256sum',path).strip() for path in paths if path.startswith('no_backup/game-assets/botw160/shader-bundles/')]
    try:
        for index,workers in enumerate(args.workers):
            directory=args.output/f'{index}-workers-{workers}';directory.mkdir()
            t.adb('shell','am','force-stop',PACKAGE)
            t.args.expected_pid=None
            for key,value in zip(props,(workers,1,1)):t.adb('shell','setprop',key,str(value))
            case=dict(workers=workers,input_lists=cache_hashes());report['cases'].append(case)
            t.adb('shell','am','start','-n',PACKAGE+'/org.macnative.android.PlatformActivity','--ez','test-launch','true')
            deadline=time.monotonic()+180
            while time.monotonic()<deadline:
                try:
                    if t.args.expected_pid is None:t.args.expected_pid=t.pid()
                    state=ctl('renderer_schedule','status')
                    case['pid']=t.args.expected_pid
                    if (int(state.get('pso_submitted','0'))>0 and
                        int(state.get('pso_built','0'))+int(state.get('pso_failed','0'))==int(state['pso_submitted']) and
                        int(state.get('pso_active','-1'))==0):
                        case['warmup']=state;break
                except (ValueError,subprocess.CalledProcessError):pass
                time.sleep(.2)
            else:raise RuntimeError('PSO warmup never reached a complete bounded state')
            if state['pso_workers']!=str(workers):raise RuntimeError('Wrong worker policy')
            capture=ctl('profiler_capture','status')
            if capture.get('capture_mode')!='file':raise RuntimeError('Startup file capture missing')
            capture_id=capture['capture_id']
            ctl('profiler_capture','stop');deadline=time.monotonic()+60
            while time.monotonic()<deadline:
                capture=ctl('profiler_capture','status')
                if capture.get('capture_id')!=capture_id:raise RuntimeError('Startup capture replaced')
                if capture.get('capture_state')=='ready':break
                if capture.get('capture_state')=='failed':raise RuntimeError('Startup capture failed')
                time.sleep(.2)
            else:raise RuntimeError('Startup capture did not finish')
            case['capture']=t.collect('file',capture_id,directory/'litep')
            ctl('profiler_ring','stop')
            scene=directory/'scene'
            result=subprocess.run([sys.executable,str(ROOT/'android/tools/validate_scene.py'),
                '--serial',args.serial,'--existing','--seconds',str(args.seconds),'--timeout','240',
                '--output',str(scene)],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
            (directory/'scene-validation.txt').write_text(result.stdout)
            case['scene']=json.loads((scene/'result.json').read_text())
            if result.returncode:raise RuntimeError('Scene validation failed')
            case['final_schedule']=ctl('renderer_schedule','status')
            case['output_lists']=cache_hashes()
            apk=t.adb('shell','pm','path',PACKAGE).strip().removeprefix('package:')
            case['apk_sha256']=t.adb('shell','sha256sum',apk).split()[0]
            (directory/'native-game.log').write_text(t.adb('exec-out','run-as',PACKAGE,'cat','files/native-botw160/native-game.log'))
            (args.output/'result.json').write_text(json.dumps(report,indent=2))
            print(json.dumps(dict(workers=workers,pid=case['pid'],fps=case['scene']['fps'],warmup=case['warmup'])),flush=True)
        report['state']='ready'
    except Exception as e:
        report.update(state='failed',error=str(e));print(str(e),file=sys.stderr)
    finally:
        try:
            if t.args.expected_pid:
                ctl('profiler_capture','stop')
                ctl('profiler_ring','stop')
        except Exception as e:report['cleanup_error']=str(e)
        for key,value in previous.items():
            try:t.adb('shell','setprop',key,value or "''")
            except Exception as e:report.setdefault('restore_errors',[]).append(str(e))
        report['restored_properties']=previous
        # A restored property affects the next process. Report the live policy
        # separately instead of claiming the current worker count was changed.
        (args.output/'result.json').write_text(json.dumps(report,indent=2))
    return 0 if report['state']=='ready' else 1

if __name__=='__main__':raise SystemExit(main())
