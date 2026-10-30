#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Restart-bound UMA allocation A/B, preserving private game data and overrides."""
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
    p.add_argument('--serial',required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--adb',default='adb');p.add_argument('--seconds',type=int,default=30)
    p.add_argument('--first-policy',choices=['cached','coherent'],help='Otherwise reuse the current renderer allocation policy')
    args=p.parse_args()
    if not 5<=args.seconds<=120:p.error('seconds=5..120')
    args.output.mkdir(parents=True,exist_ok=False)
    target=Target(SimpleNamespace(adb=args.adb,serial=args.serial,package=PACKAGE,service=SERVICE,process_suffix=':game',expected_pid=None))
    previous=target.adb('shell','getprop','debug.mng.upload_cached').strip()
    if previous not in ('','0','1'):raise RuntimeError('Unexpected upload_cached override')
    report=dict(cases=[],state='running',allocation_requires_restart=True)
    artifact_id=None
    try:
        # Read the live policy instead of duplicating the backend's default.
        wanted=(args.first_policy=='cached') if args.first_policy else target.control(['renderer_schedule','status'])['upload_cached']=='1'
        for label,enabled in [('policy_a',wanted),('opposite',not wanted),('policy_b',wanted)]:
            target.adb('shell','setprop','debug.mng.upload_cached','1' if enabled else '0')
            directory=args.output/label
            subprocess.run([sys.executable,str(ROOT/'android/tools/validate_scene.py'),
                '--serial',args.serial,'--adb',args.adb,'--restart','--seconds',str(args.seconds),
                '--settle','15','--output',str(directory)],stdout=subprocess.DEVNULL,check=True)
            case=json.loads((directory/'result.json').read_text());case['label']=label
            if case['configuration'].get('upload_cached')!=str(int(enabled)):
                raise RuntimeError('Allocation policy was not applied')
            report['cases'].append(case);target.args.expected_pid=case['pid']
            print(json.dumps(dict(label=label,pid=case['pid'],upload_cached=enabled,fps=case['fps'])),flush=True)
            case['capture_start']=target.control(['profiler_capture','file','256','3']);artifact_id=case['capture_start']['capture_id']
            for _ in range(100):
                time.sleep(.2);status=target.control(['profiler_capture','status'])
                if status.get('capture_id')!=artifact_id:raise RuntimeError('Capture ownership changed')
                if status.get('capture_state')=='ready':break
                if status.get('capture_state')=='failed':raise RuntimeError('Capture failed: '+str(status))
            else:raise RuntimeError('Capture did not finalize')
            case['capture']=target.collect('file',artifact_id,directory/'litep');artifact_id=None
            log=target.adb('exec-out','run-as',PACKAGE,'cat','files/native-botw160/native-game.log')
            (directory/'native-full.log').write_text(log)
            case['memory_evidence']=[line for line in log.splitlines() if 'NVN caps memory types' in line or 'NVN arena chunk' in line]
            (args.output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
        report['state']='ready'
    except Exception as error:report.update(state='failed',error=str(error))
    finally:
        if artifact_id:
            try:
                if target.control(['profiler_capture','status']).get('capture_id')==artifact_id:
                    target.control(['profiler_capture','stop'])
            except Exception as error:report['capture_stop_error']=str(error)
        try:target.adb('shell','setprop','debug.mng.upload_cached',previous or "''")
        except Exception as error:report['restore_error']=str(error)
        (args.output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(state=report['state'],error=report.get('error'))),flush=True)
    return 0 if report['state']=='ready' else 1


if __name__=='__main__':raise SystemExit(main())
