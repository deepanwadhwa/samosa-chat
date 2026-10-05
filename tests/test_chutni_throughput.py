#!/usr/bin/env python3
"""Generated inputs only: bounded concurrency, atomic storage and version drift."""
import json, os, sqlite3, subprocess, tempfile, time
from pathlib import Path

repo = Path(__file__).resolve().parents[1]
build = repo / os.environ.get('BUILD_DIR', 'build')
with tempfile.TemporaryDirectory(prefix='samosa-native-pipeline-') as temporary:
    base = Path(temporary).resolve()
    def call(tool, arguments, success=True):
        result = subprocess.run([str(build/'chutni-mcp'), '--call', tool, json.dumps(arguments)], text=True, capture_output=True)
        assert (result.returncode == 0) == success, result.stdout + result.stderr
        return json.loads(result.stdout)
    timings = []
    for workers in (1, 4):
        root = base/f'files-{workers}'; root.mkdir()
        for i in range(12): (root/f'generated-{i:02d}.pdf').write_text(f'Generated fixture {i}')
        activation = call('chutni_folder_activate', {'path':str(root),'confirmed':True,'register':False,'metadata_only':True})
        home = base/f'home-{workers}';home.mkdir()
        env = {**os.environ,'SAMOSA_HOME':str(home),'SAMOSA_READ_CACHE_DIR':str(home/'cache'),
               'SAMOSA_CHUTNI_WORKERS':str(workers),'SAMOSA_CHUTNI_SERVICE':str(build/'chutni-mcp'),
               'SAMOSA_EXTRACT':str(build/'test-chutni-sampling'),'SAMOSA_SAMPLE_TEST_MODE':'short',
               'SAMOSA_SAMPLE_TEST_DELAY_MS':'100','SAMOSA_SUMMARIZER_ENGINE':str(build/'test_fake_native_summarizer'),
               'SAMOSA_SUMMARIZER_MODEL':str(repo/'tests/fixtures/native-summarizer/model.gguf')}
        process = subprocess.Popen([str(build/'test-chutni-sampling'),'--real-enrich',str(root),activation['store_path']],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        progress_file = home/'chutni/scopes/11111111111111111111111111111111/progress.json'
        max_active = 0; concurrent = False
        while process.poll() is None:
            try:
                progress = json.loads(progress_file.read_text())
                max_active = max(max_active,progress.get('active_workers',0))
                concurrent |= len(progress.get('active_files',[])) > 1
            except (FileNotFoundError,json.JSONDecodeError): pass
            time.sleep(.01)
        stdout, stderr = process.communicate()
        assert process.returncode == 0, stdout+stderr
        result = json.loads(stdout);timings.append(result['seconds'])
        assert result['files'] == result['summaries'] == 12 and result['failures'] == 0,result
        assert max_active <= workers and (workers == 1 or concurrent),(max_active,workers)
        db = sqlite3.connect(Path(activation['store_path'])/'catalog.sqlite')
        assert db.execute("select count(*) from artifacts where artifact_kind='summary_short' and status='active'").fetchone()[0] == 12
        assert db.execute("select count(*) from artifacts a join derivations d using(derivation_id) join producers p using(producer_id) where a.artifact_kind='summary_short' and p.producer_kind='model' and p.model_id is not null").fetchone()[0] == 12
        db.close()
    assert timings[1] < timings[0]*.65,timings
    # One hash verification and transaction across parser/model producers.
    root=base/'atomic';root.mkdir();file=root/'record.txt';file.write_text('Original generated content')
    store=call('chutni_folder_activate',{'path':str(root),'confirmed':True,'register':False,'metadata_only':True})['store_path']
    source=call('chutni_list_sources',{'store_path':store,'source_path':str(root),'include_identity':True})['sources'][0]
    parser={'text':'generated reading','artifact_kind':'extracted_text','operation':'read_sample','producer_name':'Native parser','producer_version':'1','runtime':'native','app_name':'Samosa','app_version':'test'}
    model={'text':'generated summary','artifact_kind':'summary_short','operation':'summarize','producer_name':'Native T5','model_id':'test-model','model_revision':'1','app_name':'Samosa','app_version':'test'}
    request={'store_path':store,'source_path':str(file),'source_content_hash':source['content_hash'],'confirmed':True,'outputs':[parser,model]}
    result=call('chutni_put_file_outputs',request)
    assert result['source_verifications']==1 and result['outputs_written']==2,result
    result=call('chutni_put_file_outputs',request)
    assert result['outputs_reused']==2,result
    # A valid first item followed by an invalid producer must roll back all writes.
    changed={**parser,'text':'must roll back','operation':'new_reader'}
    broken={**model,'model_revision':None}
    call('chutni_put_file_outputs',{**request,'outputs':[changed,broken]},success=False)
    db=sqlite3.connect(Path(store)/'catalog.sqlite')
    assert db.execute("select count(*) from artifacts where inline_text='must roll back'").fetchone()[0]==0
    assert db.execute("select count(*) from artifacts where status='active' and artifact_kind in ('extracted_text','summary_short')").fetchone()[0]==2
    db.close()
    file.write_text('Changed source bytes')
    call('chutni_put_file_outputs',request,success=False)
    print(json.dumps({'test':'native_pipeline','serial_seconds':timings[0],'parallel_seconds':timings[1],'speedup':round(timings[0]/timings[1],2),'atomicity':'pass','source_change':'pass'}))
