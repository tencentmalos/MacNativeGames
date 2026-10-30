#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Same-process Vulkan translation A/B with unprofiled cadence and Litep files."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time
from types import SimpleNamespace
sys.path.insert(0, str(Path(__file__).resolve().parent))
from validate_scene import ROOT, PACKAGE, SERVICE, scene_sample
sys.path.insert(0, str(ROOT/'porting/tools'))
from profiler import Target


def cadence(samples):
    points=[((s['begin']+s['end'])/2-float(s['renderer']['sample_age_ms'])/1000,
             int(s['renderer']['frame'])) for s in samples]
    elapsed=points[-1][0]-points[0][0]
    frames=points[-1][1]-points[0][1]
    if elapsed<=0 or frames<=0:
        raise RuntimeError('No forward present progress')
    return dict(elapsed_s=elapsed, successful_presents=frames, fps=frames/elapsed,
        five_second_fps=[(points[i+5][1]-points[i][1])/(points[i+5][0]-points[i][0])
                         for i in range(0,len(points)-5,5)])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial',required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--adb',default='adb')
    parser.add_argument('--seconds',type=int,default=20)
    parser.add_argument('--capture-seconds',type=int,default=3)
    parser.add_argument('--expected-pid')
    comparison=parser.add_mutually_exclusive_group()
    comparison.add_argument('--pipeline-only',action='store_true')
    comparison.add_argument('--flush-only',action='store_true',help='Live output range Flush off/on/off/on; preserve other controls')
    comparison.add_argument('--plans-only',action='store_true',help='Preparation plans off/on/off/on; preserve all other renderer controls')
    args=parser.parse_args()
    if not 5<=args.seconds<=120 or not 1<=args.capture_seconds<=10:
        parser.error('seconds=5..120; capture-seconds=1..10')
    args.output.mkdir(parents=True,exist_ok=False)
    target=Target(SimpleNamespace(adb=args.adb,serial=args.serial,package=PACKAGE,
        service=SERVICE,process_suffix=':game',expected_pid=args.expected_pid))
    target.args.expected_pid=target.pid()
    def control(*command):
        for attempt in range(40):
            try:return target.control(command)
            except ValueError as error:
                if str(error)!='emulation_session_busy' or attempt==39:raise
                time.sleep(.025)
    report=dict(serial=args.serial,pid=target.args.expected_pid,cases=[],
        metric='successful native Vulkan presents, not compositor display timestamps',
        scene_identity='stationary current save; exact map/pose not inferred',
        limitation='scene gate detects rendering workload, not physical input or camera motion')
    previous=None
    capture_owned=False
    try:
        if control('profiler_ring','status').get('recording')!='0':
            raise RuntimeError('Profiler already recording')
        if control('profiler_capture','status').get('capture_active')=='1':
            raise RuntimeError('Another capture is active')
        if control('profiler_detail','status').get('detail')!='0':
            raise RuntimeError('Detailed CPU tracing is enabled')
        foreground=target.adb('shell','dumpsys','window')
        if not any('mCurrentFocus=' in line and PACKAGE+'/org.macnative.android.GameActivity' in line for line in foreground.splitlines()):
            raise RuntimeError('GameActivity is not foreground')
        modules=target.adb('shell','dumpsys','activity','service',PACKAGE+'/'+SERVICE,'guest_modules')
        (args.output/'guest-modules.txt').write_text(modules)
        identity=json.loads(next(x.strip() for x in modules.splitlines() if x.strip().startswith('{')))
        main_module=next(m for m in identity['modules'] if m['name']=='main')
        if str(identity['process_id'])!=target.args.expected_pid or identity['profile']!='botw160' or main_module['build_id'].lower().rstrip('0')!='8e9978d50bdd20b4c8395a106c27ffde':
            raise RuntimeError('Unexpected game identity')
        report['main_build_id']=main_module['build_id']
        apk=target.adb('shell','pm','path',PACKAGE).strip()
        if '\n' in apk or not apk.startswith('package:/data/app/'):
            raise RuntimeError('Unexpected APK location')
        report['apk_sha256']=target.adb('shell','sha256sum',apk.removeprefix('package:')).split()[0]
        previous=control('renderer_schedule','status');report['initial_configuration']=previous
        stable=('adaptive','batch_ops','present_transition','present_capable','range_invalidate',
                'preflight_buffer_filter','feedback_snapshot','defer_fragment_barrier','pipeline_memo',
                'perf_sample','read_preflight','range_preflight','read_range_cache','present_worker',
                'descriptor_plan','image_read_fast','upload_cached','pipeline_early','pso_workers',
                'preparation_validate','preparation_plan','ordered_translation')
        if args.plans_only and previous.get('preparation_validate') not in (None,'0'):
            raise RuntimeError('Preparation validation is enabled; refusing a cadence comparison')
        controls=('descriptor_plan','image_read_fast','pipeline_early')
        if args.plans_only:controls+=('preparation_plan',)
        if args.flush_only:controls+=('live_range_flush',)
        if args.flush_only and previous.get('ordered_translation')!='0':
            raise RuntimeError('Live range Flush A/B requires serial translation')
        for key in controls:
            if previous.get(key) not in ('0','1'):raise RuntimeError('Unsupported runtime control: '+key)
        # Repeated endpoints expose thermal drift. Intermediate cases attribute
        # each change; every case uses the same process, save and GPU caches.
        cases=[('baseline_a',False,False),('images',False,True),('both_a',True,True),
               ('descriptors',True,False),('baseline_b',False,False),('both_b',True,True)]
        if args.pipeline_only or args.plans_only or args.flush_only:
            cases=[(label,previous['descriptor_plan']=='1',previous['image_read_fast']=='1') for label in ('off_a','on_a','off_b','on_b')]
        report['comparison']='live_range_flush' if args.flush_only else 'preparation_plan' if args.plans_only else ('pipeline_early' if args.pipeline_only else 'descriptors/images')
        for label,descriptors,images in cases:
            if args.flush_only:
                control('renderer_schedule','flush','on' if label.startswith('on_') else 'off')
            elif args.plans_only:
                control('renderer_schedule','plans','on' if label.startswith('on_') else 'off')
            else:
                if args.pipeline_only:control('renderer_schedule','pipeline','on' if label.startswith('on_') else 'off')
                control('renderer_schedule','descriptors','on' if descriptors else 'off')
                control('renderer_schedule','images','on' if images else 'off')
            time.sleep(3)
            case=dict(label=label,configuration=control('renderer_schedule','status'))
            if args.plans_only or args.flush_only:
                expected='1' if label.startswith('on_') else '0'
                if case['configuration'].get('live_range_flush' if args.flush_only else 'preparation_plan')!=expected:
                    raise RuntimeError('Requested renderer control did not apply')
                if any(case['configuration'].get(key)!=previous.get(key) for key in stable if not (args.plans_only and key=='preparation_plan')):
                    raise RuntimeError('Another renderer control changed')
                case['thermal_before']=target.adb('shell','dumpsys','thermalservice')
            samples=[]
            with (args.output/(label+'.jsonl')).open('x') as events:
                for i in range(args.seconds+1):
                    if i%5==0:
                        foreground=target.adb('shell','dumpsys','window')
                        if not any('mCurrentFocus=' in line and PACKAGE+'/org.macnative.android.GameActivity' in line for line in foreground.splitlines()):
                            (args.output/'unexpected-window.txt').write_text(foreground)
                            raise RuntimeError('GameActivity lost foreground')
                    begin=time.monotonic();render=control('renderer_status');end=time.monotonic()
                    if not scene_sample({'stop_reason':'1'},render):raise RuntimeError('Gameplay/warmup gate changed')
                    row=dict(begin=begin,end=end,renderer=render);samples.append(row)
                    events.write(json.dumps(row)+'\n');events.flush()
                    if i<args.seconds:time.sleep(max(0,1-(end-begin)))
            case.update(cadence(samples));report['cases'].append(case)
            if args.plans_only:
                (args.output/(label+'.png')).write_bytes(target.adb('exec-out','screencap','-p',binary=True))
            if args.plans_only or args.flush_only:
                case['thermal_after']=target.adb('shell','dumpsys','thermalservice')
                case['configuration_after']=control('renderer_schedule','status')
                if (case['configuration_after'].get('live_range_flush' if args.flush_only else 'preparation_plan')!=expected or
                    any(case['configuration_after'].get(key)!=previous.get(key) for key in stable if not (args.plans_only and key=='preparation_plan'))):
                    raise RuntimeError('Renderer controls changed during cadence measurement')
            print(json.dumps(dict(label=label,fps=case['fps'])),flush=True)
            # Profile separately: never mix instrumentation overhead into FPS.
            capture=control('profiler_capture','file','256',str(args.capture_seconds));capture_owned=True
            artifact_id=capture['capture_id'];deadline=time.monotonic()+30
            while time.monotonic()<deadline:
                state=control('profiler_capture','status')
                if state.get('capture_id')!=artifact_id:raise RuntimeError('Capture ownership changed')
                if state.get('capture_state')=='ready':break
                if state.get('capture_state')=='failed':raise RuntimeError('Capture failed: '+str(state))
                time.sleep(.25)
            else:raise RuntimeError('Capture did not finalize')
            case['capture']=target.collect('file',artifact_id,args.output/label)
            capture_owned=False
            if control('profiler_ring','status').get('recording')!='0':raise RuntimeError('Capture did not restore recording')
            (args.output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
        report['state']='ready'
    except Exception as error:
        report.update(state='failed',error=str(error))
    finally:
        if capture_owned:
            try:
                if control('profiler_capture','status').get('capture_id')==artifact_id:
                    control('profiler_capture','stop')
            except Exception as error:report['capture_stop_error']=str(error)
        if previous:
            try:
                restore=(('flush','live_range_flush'),) if args.flush_only else (('plans','preparation_plan'),) if args.plans_only else (
                    ('descriptors','descriptor_plan'),('images','image_read_fast'),('pipeline','pipeline_early'))
                for command,key in restore:
                    control('renderer_schedule',command,'on' if previous[key]=='1' else 'off')
                report['restored_configuration']=control('renderer_schedule','status')
            except Exception as error:report['restore_error']=str(error)
        (args.output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(state=report['state'],error=report.get('error'))),flush=True)
    return 0 if report['state']=='ready' else 1


if __name__=='__main__':raise SystemExit(main())
