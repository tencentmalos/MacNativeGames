#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded native Vulkan W0/publication/Invalidate ABAB, separating cadence and Litep."""
import argparse
import json
import re
from pathlib import Path
import sys
import time
from types import SimpleNamespace
sys.path.insert(0,str(Path(__file__).resolve().parent))
from benchmark_translation import cadence
from validate_scene import ROOT,PACKAGE,SERVICE,scene_sample
sys.path.insert(0,str(ROOT/'porting/tools'))
from profiler import Target


def stable_cadence(samples):
    """Predeclared gate, never remove transient windows after comparing FPS."""
    values=cadence(samples)['five_second_fps']
    if len(values)<12:return False
    tail=values[-12:];mean=sum(tail)/len(tail)
    return (max(tail)-min(tail))/mean<.05 and abs(sum(tail[:6])-sum(tail[6:]))/(6*mean)<.02

def soak_progress(samples, requested_seconds, complete=False):
    """Keep interrupted evidence separate from a completed long-window gate."""
    result=dict(state='complete' if complete else 'interrupted',
                requested_seconds=requested_seconds,sample_count=len(samples))
    if len(samples)>=2:result.update(cadence(samples))
    return result

STABLE=('adaptive','batch_ops','present_transition','present_capable','range_invalidate',
        'preflight_buffer_filter','feedback_snapshot','defer_fragment_barrier','pipeline_memo',
        'perf_sample','read_preflight','range_preflight','read_range_cache','present_worker',
        'descriptor_plan','image_read_fast','upload_cached','pipeline_early','pso_workers',
        'preparation_validate','preparation_plan','ordered_translation','live_range_flush')
PROFILERS=(('profiler_ring','recording'),('profiler_hle','hle'),('profiler_guest','guest'),
           ('profiler_detail','detail'),('profiler_tags','tags'),('profiler_address','address'))
DEVICES={'6edb0593':'Odin3','9c2841a4':'AYN Thor'}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--serial',required=True);p.add_argument('--expected-pid',required=True)
    p.add_argument('--expected-apk-sha256',required=True);p.add_argument('--output',required=True,type=Path)
    p.add_argument('--mode',choices=('frontlock','publication','invalidate','lane','hint'),default='frontlock')
    p.add_argument('--seconds',type=int,default=30);p.add_argument('--capture-seconds',type=int,default=3)
    p.add_argument('--settle-seconds',type=int,default=120,
                   help='Unrecorded recovery before ABAB; last 60 s must pass the fixed stability gate')
    p.add_argument('--soak-seconds',type=int,default=0,help='Hint mode only: optional 300..600 s unrecorded ON window')
    a=p.parse_args()
    if a.serial not in DEVICES:p.error('Select an explicitly authorized Odin3 or Thor serial')
    if not 30<=a.seconds<=120 or not 0<=a.capture_seconds<=10:p.error('seconds=30..120, capture-seconds=0..10')
    if not 120<=a.settle_seconds<=300:p.error('settle-seconds=120..300')
    if a.soak_seconds and (a.mode!='hint' or not 300<=a.soak_seconds<=600):p.error('soak-seconds=300..600 in hint mode')
    a.output.mkdir(parents=True,exist_ok=False)
    t=Target(SimpleNamespace(adb='adb',serial=a.serial,package=PACKAGE,service=SERVICE,process_suffix=':game',expected_pid=a.expected_pid))
    report=dict(state='running',mode=a.mode,serial=a.serial,pid=t.pid(),cases=[],
        metric='successful native presents; same current save/pose assumed; no input injection',
        capture_schedule='after_all_cadence_windows')
    owned=False;capture_id=None
    def save(name,data):(a.output/name).write_text(json.dumps(data,indent=2)+'\n')
    def c(*args):
        for attempt in range(80):
            try:
                result=t.control(args)
                # A status fragment may be busy even when the outer renderer
                # lock succeeded. It is not a changed/missing configuration.
                if result.get('ordered_status')=='busy':raise ValueError('emulation_session_busy')
                return result
            except ValueError as e:
                if str(e)!='emulation_session_busy' or attempt==79:raise
                time.sleep(.025)
    def verify_identity():
        assert t.adb('shell','getprop','ro.product.model').strip()==DEVICES[a.serial]
        raw=t.adb('shell','dumpsys','activity','service',PACKAGE+'/'+SERVICE,'guest_modules')
        obj=json.loads(next(s.strip() for s in raw.splitlines() if s.strip().startswith('{')))
        main=next(m for m in obj['modules'] if m['name']=='main')
        assert str(obj['process_id'])==a.expected_pid
        assert main['build_id']=='8e9978d50bdd20b4c8395a106c27ffde00000000000000000000000000000000'
        apk=t.adb('shell','pm','path',PACKAGE).strip();assert apk.startswith('package:/data/app/') and '\n' not in apk
        sha=t.adb('shell','sha256sum',apk.removeprefix('package:')).split()[0]
        assert sha==a.expected_apk_sha256
        (a.output/'guest-modules.txt').write_text(raw);report.update(apk_sha256=sha,main_build_id=main['build_id'])
    def threads():
        for attempt in range(80):
            raw=t.adb('shell','dumpsys','activity','service',PACKAGE+'/'+SERVICE,'threads')
            if 'error=emulation_session_busy' not in raw:break
            time.sleep(.025)
        (a.output/'threads-latest.txt').write_text(raw)
        owner=re.search(r'^SERVICE .*\bpid=(\d+)\b',raw,re.M)
        assert owner and owner[1]==a.expected_pid and t.pid()==a.expected_pid
        assert 'schema=mng.guest-threads.v1' in raw and 'truncated=0' in raw
        rows=[dict(re.findall(r'(\w+)=(\S+)',line)) for line in raw.splitlines() if line.strip().startswith('thread handle=')]
        return dict(process_id=a.expected_pid,threads=rows,raw=raw)
    def foreground():
        raw=t.adb('shell','dumpsys','window')
        if not any('mCurrentFocus=' in s and PACKAGE+'/org.macnative.android.GameActivity' in s for s in raw.splitlines()):
            (a.output/'unexpected-window.txt').write_text(raw);raise RuntimeError('Game lost foreground')
    def check_configuration(enabled):
        actual=c('renderer_schedule','status')
        changes={k:[report['configuration'].get(k),actual.get(k)] for k in STABLE if report['configuration'].get(k)!=actual.get(k)}
        image=c('renderer_flush_image','status')['image_flush']
        if image!=report['image_flush']['image_flush']:changes['image_flush']=[report['image_flush']['image_flush'],image]
        publication=c('renderer_publication','status')
        if a.mode in ('lane','hint') and c('renderer_invalidate','status').get('invalidate_fast')!='1':
            changes['invalidate_fast']=['1','not_enabled']
        if a.mode in ('invalidate','lane','hint') and publication.get('publication_queue')!='1':
            changes['publication_queue']=['1',publication.get('publication_queue')]
        mode=c(control,'status').get(key)
        if mode!=str(int(enabled)):changes[key]=[str(int(enabled)),mode]
        if changes:save('configuration-failure.json',dict(changes=changes,actual=actual));raise RuntimeError('Configuration changed: '+str(changes))
        if a.mode=='hint':
            hint=c(control,'status')
            assert hint['hint_generation']==hint['hint_applied'],hint
            assert hint['hint_failures']=='0',hint
            tids=[int(hint[k]) for k in ('hint_translate_tid','hint_present_tid')]
            assert (all(tids) and hint['hint_supported']=='1') if enabled else not any(tids),hint
        return actual
    control,key={'frontlock':('frontlock_status','frontlock'),
                 'publication':('renderer_publication','publication_queue'),
                 'invalidate':('renderer_invalidate','invalidate_fast'),
                 'lane':('lane_status','lane_enabled'),
                 'hint':('renderer_hint','renderer_hint')}[a.mode]
    def raw_lanes(name):
        for attempt in range(80):
            raw=t.adb('shell','dumpsys','activity','service',PACKAGE+'/'+SERVICE,'lane_status','status')
            if 'schema=mng.lanes.v2' in raw and 'error=' not in raw:
                (a.output/name).write_text(raw);return
            if 'emulation_session_busy' not in raw or attempt==79:raise RuntimeError(raw)
            time.sleep(.025)
    def utilization():
        # Non-root snapshots. CPU is a process share of all observed CPU ticks.
        # gpubusy is the driver's last interval, not a calibrated trace ROI.
        result={}
        for name,args in [('process',('run-as',PACKAGE,'cat','/proc/'+a.expected_pid+'/stat')),
                          ('system',('cat','/proc/stat')),
                          ('gpu',('cat','/sys/class/kgsl/kgsl-3d0/gpubusy'))]:
            try:result[name]=t.adb('shell',*args)
            except Exception as error:result[name+'_unavailable']=str(error)
        return result
    def cpu_share(before,after):
        try:
            # Field 3 follows comm, which itself can contain spaces/parentheses.
            a=before['process'].rsplit(')',1)[1].split();b=after['process'].rsplit(')',1)[1].split()
            assert a[19]==b[19] # starttime, PID reuse guard
            process=sum(int(b[i])-int(a[i]) for i in (11,12))
            # Linux guest ticks are already included in user/nice.
            x=list(map(int,before['system'].splitlines()[0].split()[1:9]))
            y=list(map(int,after['system'].splitlines()[0].split()[1:9]))
            total=sum(y)-sum(x);assert total>0 and process>=0
            return dict(process_ticks=process,system_ticks=total,percent_all_cpu_ticks=100*process/total)
        except (KeyError,ValueError,IndexError,AssertionError) as error:return dict(unavailable=str(error))
    def capture(item,folder):
        nonlocal capture_id
        item['capture_started_unix_ns']=time.time_ns()
        c('frontlock_status','off');c('frontlock_status','reset');c('frontlock_status','on')
        for name in ('profiler_tags','profiler_address','profiler_guest'):c(name,'start')
        cap=c('profiler_capture','file','256',str(a.capture_seconds));capture_id=cap['capture_id'];deadline=time.monotonic()+30
        while time.monotonic()<deadline:
            status=c('profiler_capture','status');assert status['capture_id']==capture_id
            if status['capture_state']=='ready':break
            if status['capture_state']=='failed':raise RuntimeError(str(status))
            time.sleep(.25)
        else:raise RuntimeError('Capture finalize timeout')
        c('frontlock_status','off');item['frontlock_profile']=c('frontlock_status','status')
        for name in ('profiler_tags','profiler_address','profiler_guest'):c(name,'stop')
        item['control_after_profile']=c(control,'status')
        item['capture']=t.collect('file',capture_id,folder);capture_id=None
        item['threads_after']=threads();save('result.json',report)
    try:
        verify_identity();foreground()
        for command,key_name in PROFILERS:assert c(command,'status')[key_name]=='0',command
        assert c('profiler_capture','status')['capture_active']=='0'
        assert c('profiler_tags','status')['installed']=='12'
        report['configuration']=c('renderer_schedule','status');assert report['configuration']['ordered_translation']=='0'
        report['image_flush']=c('renderer_flush_image','status');report['control_before']=c(control,'status')
        report['publication_before']=c('renderer_publication','status')
        if a.mode in ('invalidate','lane','hint'):assert report['publication_before']['publication_queue']=='1'
        if a.mode in ('lane','hint'):assert c('renderer_invalidate','status')['invalidate_fast']=='1'
        report['frontlock_before']=c('frontlock_status','status');assert report['frontlock_before']['frontlock']=='0'
        report['threads_before']=threads();report['ordered_inputs']=c('renderer_ordered_inputs','status')
        owned=True
        c(control,'off')
        recovery=[]
        with (a.output/'recovery.jsonl').open('x') as f:
            for i in range(a.settle_seconds+1):
                if i%5==0:foreground()
                begin=time.monotonic();render=c('renderer_status');end=time.monotonic()
                if not scene_sample({'stop_reason':'1'},render):raise RuntimeError('Recovery gameplay gate changed')
                row=dict(begin=begin,end=end,renderer=render);recovery.append(row)
                f.write(json.dumps(row)+'\n');f.flush()
                if i<a.settle_seconds:time.sleep(max(0,1-(end-begin)))
        report['recovery']=cadence(recovery)
        report['recovery']['stable']=stable_cadence(recovery)
        save('result.json',report)
        if not report['recovery']['stable']:raise RuntimeError('Recovery cadence unstable; no ABAB windows accepted')
        for label,enabled in [('off_a',False),('on_a',True),('off_b',False),('on_b',True)]:
            verify_identity();c(control,'on' if enabled else 'off');time.sleep(3)
            if a.mode=='lane':c(control,'reset')
            case=dict(label=label,enabled=enabled,configuration=check_configuration(enabled),
                      thermal_before=t.adb('shell','dumpsys','thermalservice'))
            case['control_before']=c(control,'status')
            case['publication_before']=c('renderer_publication','status')
            case['utilization_before']=utilization()
            assert c('profiler_ring','status')['recording']=='0'
            if a.mode in ('publication','invalidate','lane','hint'):assert c('frontlock_status','status')['frontlock']=='0'
            if a.mode=='frontlock':c('frontlock_status','reset')
            if a.mode=='lane':raw_lanes(label+'-lanes-before.txt')
            samples=[];case['cadence_started_unix_ns']=time.time_ns()
            with (a.output/(label+'.jsonl')).open('x') as f:
                for i in range(a.seconds+1):
                    if i%5==0:foreground()
                    begin=time.monotonic();render=c('renderer_status');end=time.monotonic()
                    if not scene_sample({'stop_reason':'1'},render):raise RuntimeError('Warm gameplay gate changed')
                    row=dict(begin=begin,end=end,renderer=render);samples.append(row);f.write(json.dumps(row)+'\n');f.flush()
                    if i<a.seconds:time.sleep(max(0,1-(end-begin)))
            case['cadence_finished_unix_ns']=time.time_ns()
            if a.mode=='lane':raw_lanes(label+'-lanes-after.txt')
            case['utilization_after']=utilization()
            case['cpu_share']=cpu_share(case['utilization_before'],case['utilization_after'])
            case['publication_after']=c('renderer_publication','status')
            case.update(cadence(samples));case['configuration_after']=check_configuration(enabled)
            case['frontlock_cadence']=c('frontlock_status','status');case['control_after']=c(control,'status')
            case['thermal_after']=t.adb('shell','dumpsys','thermalservice');report['cases'].append(case)
            print(json.dumps({k:case[k] for k in ('label','fps','successful_presents','elapsed_s')}),flush=True)
            save('result.json',report)
        if a.soak_seconds:
            c(control,'on');time.sleep(3);check_configuration(True)
            soak=dict(thermal_before=t.adb('shell','dumpsys','thermalservice'))
            samples=[];complete=False
            try:
                with (a.output/'soak.jsonl').open('x') as f:
                    for i in range(a.soak_seconds+1):
                        if i%5==0:foreground();assert t.pid()==a.expected_pid
                        begin=time.monotonic();render=c('renderer_status');end=time.monotonic()
                        if not scene_sample({'stop_reason':'1'},render):raise RuntimeError('Soak gameplay gate changed')
                        row=dict(begin=begin,end=end,renderer=render);samples.append(row)
                        if i%30==0:row['thermal']=t.adb('shell','dumpsys','thermalservice')
                        f.write(json.dumps(row)+'\n');f.flush()
                        if i<a.soak_seconds:time.sleep(max(0,1-(time.monotonic()-begin)))
                soak['thermal_after']=t.adb('shell','dumpsys','thermalservice')
                soak['hint_after']=c(control,'status');check_configuration(True)
                complete=True
            finally:
                soak.update(soak_progress(samples,a.soak_seconds,complete))
                report['soak']=soak;save('result.json',report)
        # Streaming can change thermal/DVFS state even after it has stopped.
        # Keep every cadence window ahead of all profiler captures so a capture
        # cannot perturb only the second A/B pair.
        for case in report['cases'] if a.capture_seconds else []:
            if a.mode in ('publication','invalidate') or (a.mode=='lane' and case['enabled']):
                verify_identity();foreground()
                c(control,'on' if case['enabled'] else 'off')
                if a.mode=='lane':c(control,'reset')
                check_configuration(case['enabled']);time.sleep(3)
                capture(case,a.output/case['label'])
        if a.mode=='frontlock' and a.capture_seconds:capture(report,a.output/'profile')
        report['state']='ready'
    except Exception as e:
        report.update(state='failed',error=str(e));raise
    finally:
        if owned:
            try:
                if capture_id and c('profiler_capture','status')['capture_id']==capture_id:c('profiler_capture','stop')
                for name in ('profiler_tags','profiler_address','profiler_guest','profiler_ring'):c(name,'stop')
                c(control,'on' if report['control_before'][key]=='1' else 'off');c('frontlock_status','off')
                report['tags_final']=c('profiler_tags','status');report['frontlock_final']=c('frontlock_status','status')
            except Exception as e:report['cleanup_error']=str(e)
        save('result.json',report)
if __name__=='__main__':main()
