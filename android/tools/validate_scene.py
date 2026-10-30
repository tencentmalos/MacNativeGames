#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Launch native BOTW, await verified save loading, and measure present cadence."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time
from types import SimpleNamespace

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'porting/tools'))
from profiler import Target

PACKAGE='org.macnative.android.botw'
SERVICE='org.macnative.android.PortDebugService'

def scene_sample(autoload, render):
    # Save opening proves only that loading began. Require sustained rendered
    # gameplay workload as a second gate; this is not a semantic scene detector.
    return (autoload.get('stop_reason')=='1' and int(render.get('frame','0'))>0
            and float(render.get('sample_age_ms','inf'))<500
            and int(render.get('draws','0'))>=1000
            and int(render.get('dispatches','0'))>0
            and int(render.get('pipelines_created','0'))==0
            and int(render.get('shaders_loaded','0'))==0)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial',required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--adb',default='adb')
    mode=parser.add_mutually_exclusive_group()
    mode.add_argument('--restart',action='store_true')
    mode.add_argument('--existing',action='store_true',help='Validate the current game PID without relaunching')
    parser.add_argument('--seconds',type=int,default=30)
    parser.add_argument('--timeout',type=int,default=180)
    parser.add_argument('--settle',type=int,default=10)
    args=parser.parse_args()
    if not 5<=args.seconds<=120 or not 30<=args.timeout<=600 or not 3<=args.settle<=60:
        parser.error('seconds=5..120, timeout=30..600, settle=3..60')
    args.output.mkdir(parents=True,exist_ok=False)
    target=Target(SimpleNamespace(adb=args.adb,serial=args.serial,package=PACKAGE,
        service=SERVICE,process_suffix=':game',expected_pid=None))
    def adb(*command): return target.adb(*command)
    def control(command):
        for attempt in range(80):
            try:return target.control(command)
            except ValueError as error:
                if str(error)!='emulation_session_busy' or attempt==79:raise
                time.sleep(min(.02*(attempt+1),.25))
    def pid():
        result=subprocess.run([args.adb,'-s',args.serial,'shell','pidof',PACKAGE+':game'],
                              capture_output=True,text=True,timeout=10)
        return result.stdout.strip() if result.returncode==0 else ''
    report={'serial':args.serial,'package':PACKAGE,'state':'starting',
            'scene_identity':'latest_save; exact map/pose not inferred',
            'scene_gate':'save opened, >=1000 draws, dispatches, warm shaders, fresh presents',
            'started_unix':time.time()}
    previous=adb('shell','getprop','debug.mng.autoload').strip()
    if previous not in ('','0','1'):raise RuntimeError('Unexpected existing debug.mng.autoload value')
    events=(args.output/'events.jsonl').open('x')
    try:
        current=pid()
        if args.existing and not current:raise RuntimeError('No existing game process')
        if current and not args.existing:
            if not args.restart:raise RuntimeError('Game already running; --restart is required to replace this session')
            adb('shell','am','force-stop',PACKAGE)
        if not args.existing:
            adb('shell','setprop','debug.mng.autoload','1')
            # A surviving launch Activity handles test-launch only in onCreate.
            # With no game process, recreate its task so an ordinary return to
            # the launch screen cannot leave the automatic scene gate waiting.
            adb('shell','am','start','-f','0x10000000','--activity-clear-task',
                '-n',PACKAGE+'/org.macnative.android.PlatformActivity','--ez','test-launch','true')
        def is_foreground():
            foreground=adb('shell','dumpsys','window')
            valid=any('mCurrentFocus=' in line and PACKAGE+'/org.macnative.android.GameActivity' in line
                      for line in foreground.splitlines())
            if not valid:(args.output/'unexpected-window.txt').write_text(foreground)
            return valid
        deadline=time.monotonic()+args.timeout
        stable_since=None;last_frame=0;ready=False
        while time.monotonic()<deadline:
            current=pid()
            if current and not report.get('pid'):
                if not current.isdecimal():raise RuntimeError('Ambiguous game process')
                report['pid']=current;target.args.expected_pid=current
            if report.get('pid') and current!=report['pid']:
                raise RuntimeError('Game process exited or changed during loading')
            try:
                auto=control(['autoload_status'])
                render=control(['renderer_status'])
            except (ValueError,subprocess.CalledProcessError) as error:
                events.write(json.dumps({'time':time.time(),'retry':str(error)})+'\n');events.flush()
                time.sleep(1);continue
            if auto.get('stop_reason') in ('2','3'):
                raise RuntimeError('Auto-load stopped: '+('user input' if auto['stop_reason']=='2' else 'timeout'))
            events.write(json.dumps({'time':time.time(),'autoload':auto,'renderer':render})+'\n');events.flush()
            frame=int(render.get('frame','0'))
            if scene_sample(auto,render) and frame>last_frame and is_foreground():
                if stable_since is None:stable_since=time.monotonic()
                if time.monotonic()-stable_since>=args.settle:ready=True;break
            else:stable_since=None
            last_frame=frame;time.sleep(1)
        if not ready:raise RuntimeError('Timed out before save-load and gameplay workload gates completed')
        def require_foreground():
            if not is_foreground():
                raise RuntimeError('GameActivity is not foreground')
        require_foreground()
        if control(['profiler_ring','status']).get('recording')!='0':
            raise RuntimeError('Profiler recording is active; refusing an unlabelled cadence baseline')
        report['configuration']=control(['renderer_schedule','status'])
        # guest_modules emits structured JSON, so preserve the unmodified reply.
        modules=adb('shell','dumpsys','activity','service',PACKAGE+'/'+SERVICE,'guest_modules')
        (args.output/'guest-modules.txt').write_text(modules)
        snapshot=json.loads(next(line.strip() for line in modules.splitlines() if line.strip().startswith('{')))
        main_module=next(module for module in snapshot['modules'] if module['name']=='main')
        if (str(snapshot['process_id'])!=report['pid'] or snapshot['profile']!='botw160' or
            main_module['build_id'].lower().rstrip('0')!='8e9978d50bdd20b4c8395a106c27ffde'):
            raise RuntimeError('Unexpected process/title/main module identity')
        report['main_build_id']=main_module['build_id']
        samples=[]
        for i in range(args.seconds+1):
            if i%5==0:require_foreground()
            begin=time.monotonic();render=control(['renderer_status']);end=time.monotonic()
            if not scene_sample({'stop_reason':'1'},render):raise RuntimeError('Gameplay/warmup gate changed during cadence sampling')
            row={'begin':begin,'end':end,'renderer':render};samples.append(row)
            events.write(json.dumps(row)+'\n');events.flush()
            if i<args.seconds:time.sleep(max(0,1-(end-begin)))
        points=[((s['begin']+s['end'])/2-float(s['renderer']['sample_age_ms'])/1000,int(s['renderer']['frame'])) for s in samples]
        elapsed=points[-1][0]-points[0][0];count=points[-1][1]-points[0][1]
        report.update(state='ready',elapsed_s=elapsed,successful_presents=count,fps=count/elapsed,
            five_second_fps=[(points[i+5][1]-points[i][1])/(points[i+5][0]-points[i][0]) for i in range(0,len(points)-5,5)],
            metric='successful native Vulkan presents; not compositor display timestamps',
            identity=samples[0]['renderer'].get('title'),profiler_recording=False)
    except Exception as error:
        report.update(state='failed',error=str(error))
    finally:
        events.close()
        try:
            if not args.existing:adb('shell','setprop','debug.mng.autoload',previous or "''")
        except Exception as error:report['property_restore_error']=str(error)
        try:(args.output/'native-game.log').write_text(adb('exec-out','run-as',PACKAGE,'tail','-n','300','files/native-botw160/native-game.log'))
        except Exception as error:report['log_error']=str(error)
        (args.output/'result.json').write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2))
    return 0 if report['state']=='ready' else 1

if __name__=='__main__':raise SystemExit(main())
