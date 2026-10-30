#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Shared coherent-image Flush A/B/A/B with separate Guest/address Litep capture."""
import argparse,json,sys,time
from pathlib import Path
from types import SimpleNamespace
sys.path.insert(0,str(Path(__file__).resolve().parent))
from benchmark_translation import cadence
from validate_scene import ROOT,PACKAGE,SERVICE,scene_sample
sys.path.insert(0,str(ROOT/'porting/tools'))
from profiler import Target

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--serial',required=True);p.add_argument('--expected-pid',required=True);p.add_argument('--output',required=True,type=Path);p.add_argument('--seconds',type=int,default=30);p.add_argument('--capture-seconds',type=int,default=3);a=p.parse_args()
 if not 10<=a.seconds<=120 or not 1<=a.capture_seconds<=10:p.error('seconds=10..120; capture-seconds=1..10')
 a.output.mkdir(parents=True,exist_ok=False)
 t=Target(SimpleNamespace(adb='adb',serial=a.serial,package=PACKAGE,service=SERVICE,process_suffix=':game',expected_pid=a.expected_pid))
 def c(*command):
  for attempt in range(40):
   try:return t.control(command)
   except ValueError as error:
    if str(error)!='emulation_session_busy' or attempt==39:raise
    time.sleep(.025)
 def dump(name,data):(a.output/name).write_text(json.dumps(data,indent=2)+'\n')
 report={'pid':t.pid(),'serial':a.serial,'state':'running','cases':[],'metric':'native presents; elapsed scopes are not on-CPU; same current save/pose assumed, not inferred'};capture_id=None;owned=False
 try:
  assert c('profiler_tags','status')['installed']=='12'
  for command,key in [('profiler_ring','recording'),('profiler_hle','hle'),('profiler_guest','guest'),('profiler_detail','detail'),('profiler_tags','tags'),('profiler_address','address')]:assert c(command,'status')[key]=='0',command
  assert c('profiler_capture','status').get('capture_active')!='1'
  report['configuration']=c('renderer_schedule','status');report['tags_before']=c('profiler_tags','status');report['image_flush_before']=c('renderer_flush_image','status')
  raw=t.adb('shell','dumpsys','activity','service',PACKAGE+'/'+SERVICE,'guest_modules');(a.output/'guest-modules.txt').write_text(raw)
  modules=json.loads(next(line.strip() for line in raw.splitlines() if line.strip().startswith('{')))
  main=next(m for m in modules['modules'] if m['name']=='main')
  assert str(modules['process_id'])==a.expected_pid and main['build_id']=='8e9978d50bdd20b4c8395a106c27ffde00000000000000000000000000000000'
  assert any(x['id']=='botw160.performance-tags-v1' and x['sites']==12 for x in main['guest_patches'])
  report['main_build_id']=main['build_id']
  apk=t.adb('shell','pm','path',PACKAGE).strip();assert apk.startswith('package:/data/app/') and '\n' not in apk
  report['apk_sha256']=t.adb('shell','sha256sum',apk[len('package:'):]).split()[0]
  owned=True
  stable=('adaptive','batch_ops','present_transition','present_capable','range_invalidate',
          'preflight_buffer_filter','feedback_snapshot','defer_fragment_barrier','pipeline_memo',
          'perf_sample','read_preflight','range_preflight','read_range_cache','present_worker',
          'descriptor_plan','image_read_fast','upload_cached','pipeline_early','pso_workers',
          'preparation_validate','preparation_plan','ordered_translation','live_range_flush')
  assert report['configuration']['ordered_translation']=='0'
  assert report['configuration']['live_range_flush']=='1'
  def configuration_check(enabled):
   now=c('renderer_schedule','status')
   differences={k:(report['configuration'].get(k),now.get(k)) for k in stable if now.get(k)!=report['configuration'].get(k)}
   if differences:dump('configuration-failure.json',{'changes':differences,'actual':now});raise RuntimeError('Renderer configuration changed: '+str(differences))
   assert c('renderer_flush_image','status')['image_flush']==str(int(enabled))
   return now
  # Change only the image-only Flush policy in one warmed process. Measure
  # cadence with tracing off, then collect separate Guest/address traces.
  for label,enabled in [('off_a',False),('on_a',True),('off_b',False),('on_b',True)]:
   c('renderer_flush_image','on' if enabled else 'off');c('profiler_tags','stop');c('profiler_address','stop');c('profiler_ring','stop');time.sleep(3)
   configuration=configuration_check(enabled);thermal_before=t.adb('shell','dumpsys','thermalservice')
   samples=[]
   with (a.output/(label+'.jsonl')).open('x') as f:
    for i in range(a.seconds+1):
     if i%5==0:
      window=t.adb('shell','dumpsys','window');assert any('mCurrentFocus=' in x and PACKAGE+'/org.macnative.android.GameActivity' in x for x in window.splitlines()),'Foreground changed'
     begin=time.monotonic();r=c('renderer_status');end=time.monotonic()
     assert scene_sample({'stop_reason':'1'},r),'Warm gameplay gate changed'
     row={'begin':begin,'end':end,'renderer':r};samples.append(row);f.write(json.dumps(row)+'\n');f.flush()
     if i<a.seconds:time.sleep(max(0,1-(end-begin)))
   case={'label':label,'image_flush':enabled,'ring':False,'configuration':configuration,'configuration_after':configuration_check(enabled),'thermal_before':thermal_before,**cadence(samples)};report['cases'].append(case);print(json.dumps({k:case[k] for k in ('label','fps','successful_presents','elapsed_s')}),flush=True)
   c('profiler_ring','stop')
   c('profiler_tags','start');c('profiler_address','start')
   cap=c('profiler_capture','file','256',str(a.capture_seconds));capture_id=cap['capture_id'];deadline=time.monotonic()+30
   while time.monotonic()<deadline:
    state=c('profiler_capture','status');assert state['capture_id']==capture_id
    if state.get('capture_state')=='ready':break
    if state.get('capture_state')=='failed':raise RuntimeError(str(state))
    time.sleep(.25)
   else:raise RuntimeError('Capture finalization timeout')
   case['capture']=t.collect('file',capture_id,a.output/label);capture_id=None
   c('profiler_tags','stop');c('profiler_address','stop')
   case['tags_status']=c('profiler_tags','status');case['flush_status']=c('renderer_flush_image','status');case['thermal']=t.adb('shell','dumpsys','thermalservice');dump('result.json',report)
  report['state']='ready'
 except Exception as ex:report.update(state='failed',error=str(ex));raise
 finally:
  if owned:
   try:
    if capture_id and c('profiler_capture','status').get('capture_id')==capture_id:c('profiler_capture','stop')
    c('profiler_tags','stop');c('profiler_address','stop');c('profiler_ring','stop');c('renderer_flush_image','on' if report['image_flush_before']['image_flush']=='1' else 'off');report['tags_after']=c('profiler_tags','status')
   except Exception as ex:report['cleanup_error']=str(ex)
  dump('result.json',report)
if __name__=='__main__':main()
