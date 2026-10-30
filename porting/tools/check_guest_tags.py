#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate canonical Guest symbols against the compiled native tag catalog."""
import argparse,hashlib,json,re
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def check(image=None):
 root=ROOT/'games/botw/guest_patches/performance-tags'
 index=json.loads((root/'index.json').read_text());guards=json.loads((root/'guards.json').read_text())
 shard=index['shards'][0];raw=(root/shard['path']).read_bytes()
 assert hashlib.sha256(raw).hexdigest()==shard['sha256'],'Symbol shard SHA mismatch'
 symbols=json.loads(raw)['symbols'];sites=guards['sites'];assert len(symbols)==len(sites)==12
 compiled=(root.parent/'performance_tags.inc').read_text()
 rows=re.findall(r'\{"Guest\.BOTW\.([^\"]+)",\s*(0x[0-9a-f]+),\s*(0x[0-9a-f]+),\s*"([0-9a-f]{64})"\}',compiled)
 assert len(rows)==len(sites),'Compiled tag count mismatch'
 # These independently reviewed one-word instructions have no PC-relative or
 # control-flow dependency. New instruction forms require an ABI review.
 allowed={0xfc190fe8,0xa9ba6ffc,0xb9455408,0xd107c3ff,0xd10103ff,0xf81b0ff9,0xf81c0ff7,0x6db82beb,0xd10203ff}
 data=image.read_bytes() if image else None
 if data:assert hashlib.sha256(data).hexdigest()==guards['source']['android_rewrite_sha256'],'Wrong private rewritten image'
 for i,(site,symbol,row) in enumerate(zip(sites,symbols,rows)):
  assert site['id']==i and site['name']==symbol['name']==row[0]
  off=int(site['module_offset'],0);word=int(site['first_word'],0)
  assert off==int(symbol['module_offset'],0)==int(row[1],0)
  assert word==int(row[2],0) and word in allowed
  assert site['sha256_first_16']==row[3] and symbol['confidence']=='correlated'
  if data:assert hashlib.sha256(data[off:off+16]).hexdigest()==row[3]
 print('PASS: 12 SHA-pinned semantic symbols, compiled IDs, offsets, instruction relocation guards'+(' and exact private image' if data else ''))
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--private-image',type=Path);check(p.parse_args().private_image)
