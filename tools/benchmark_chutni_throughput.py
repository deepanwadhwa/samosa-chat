#!/usr/bin/env python3
"""Test-only orchestration; no Python dependency in the native memory pipeline."""
from pathlib import Path
import json,subprocess,shutil,os,time,threading
import argparse
parser=argparse.ArgumentParser(description="Benchmark the native pipeline with generated, uniquely hashed fixtures.")
parser.add_argument('--fixtures',type=Path,required=True,help='Generated Opening Samples fixture directory')
parser.add_argument('--output',type=Path,required=True,help='Fresh output directory; existing directories are refused')
parser.add_argument('--workers',type=int,default=6,choices=range(1,7))
parser.add_argument('--case',choices=['native','mixed','all'],default='all')
parser.add_argument('--enrich-binary',type=Path)
parser.add_argument('--service',type=Path)
parser.add_argument('--summarizer',type=Path)
args=parser.parse_args()
repo=Path(__file__).resolve().parents[1];base=args.output.resolve();original=args.fixtures.resolve()
if base.exists():parser.error('--output must be a new directory')
base.mkdir(parents=True)
engine=args.enrich_binary or repo/'build/test-chutni-sampling'
service=args.service or repo/'build/chutni-mcp'
summarizer=args.summarizer or repo/'build/samosa-summarizer'
def run(case,names,workers=args.workers):
 root=base/case;root.mkdir(exist_ok=True)
 for i,name in enumerate(names):
  destination=root/f'{i:03d}-{name}';shutil.copyfile(original/name,destination)
  # Unique bytes prevent content-addressed cache reuse between repeated layouts.
  marker=f'Generated unique fixture {case} record {i}.\n'.encode()
  if destination.suffix=='.txt':destination.write_bytes(marker+destination.read_bytes())
  else:
   with destination.open('ab') as output:output.write(b'\n% '+marker)
 store=Path(str(root)+'.chutni')
 if store.exists():shutil.rmtree(store)
 home=base/(case+'-home');home.mkdir(exist_ok=True)
 r=subprocess.run([str(service),'--call','chutni_folder_activate',json.dumps({'path':str(root),'confirmed':True,'metadata_only':True,'register':False})],capture_output=True,text=True)
 assert r.returncode==0,r.stdout+r.stderr
 env={**os.environ,'SAMOSA_HOME':str(home),'SAMOSA_READ_CACHE_DIR':str(home/'fresh-cache'),'SAMOSA_CHUTNI_WORKERS':str(workers),'SAMOSA_CHUTNI_SERVICE':str(service),'SAMOSA_EXTRACT':str(Path.home()/'.samosa/current/bin/samosa-extract'),'SAMOSA_OCR':str(Path.home()/'.samosa/current/bin/samosa-ocr'),'SAMOSA_SUMMARIZER_ENGINE':str(summarizer),'SAMOSA_SUMMARIZER_MODEL':str(Path.home()/'.samosa/current/models/native-summarizer/samosa-text-summarization-Q8_0.gguf')}
 p=subprocess.Popen([str(engine),'--real-enrich',str(root),str(store)],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
 peak=0
 while p.poll() is None:
  raw=subprocess.check_output(['ps','-Ao','pid=,ppid=,rss='],text=True);rows=[list(map(int,r.split())) for r in raw.splitlines()];desc={p.pid}
  for _ in range(4):desc|={pid for pid,parent,rss in rows if parent in desc}
  peak=max(peak,sum(rss for pid,parent,rss in rows if pid in desc));time.sleep(.15)
 out,err=p.communicate();(base/(case+'-stderr.log')).write_text(err)
 assert p.returncode==0,out+err
 data=json.loads(out);data.update(case=case,workers=workers,files_per_second=round(data['files']/data['seconds'],3),peak_process_tree_rss_mib=round(peak/1024,1),cold=True)
 (base/(case+'.json')).write_text(json.dumps(data,indent=2));print(json.dumps(data),flush=True)
if args.case in ('native','all'):run('unique-native32',['Long Unicode text.txt','Native pages.pdf']*16)
if args.case in ('mixed','all'):run('unique-mixed24',['Long Unicode text.txt','Native pages.pdf','Mixed text and scans.pdf','Large first page.pdf','Blank pages.pdf','Single scanned image.png']*4)
