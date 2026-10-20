#!/usr/bin/env python3
"""Measure only our compiled process. Workloads use a disposable SQLite fixture."""
import json
import os
from pathlib import Path
import queue
import select
import signal
import sqlite3
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parent.parent
BINARY = ROOT / 'dist/puff'
HZ = os.sysconf('SC_CLK_TCK')


def snapshot(pid):
    values={}
    for line in Path(f'/proc/{pid}/status').read_text().splitlines():
        if line.startswith(('VmRSS:','VmHWM:','Threads:')):
            key,value,*_=line.split()
            values[key.rstrip(':')]=int(value)
    # The executable name contains no spaces; fields 14/15 are user/system ticks.
    stat=Path(f'/proc/{pid}/stat').read_text().split()
    values['ticks']=int(stat[13])+int(stat[14])
    try:
        for line in Path(f'/proc/{pid}/smaps_rollup').read_text().splitlines():
            if line.startswith('Pss:'):
                values['PssKiB']=int(line.split()[1])
    except PermissionError:
        pass
    return values


def start(env,*args):
    begun=time.monotonic()
    process=subprocess.Popen([str(BINARY),*args],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,bufsize=1)
    lines=queue.Queue()
    def reader():
        for line in process.stdout:
            lines.put((time.monotonic(),line.strip()))
    threading.Thread(target=reader,daemon=True).start()
    return process,lines,begun


def until(process,lines,marker,seconds=20):
    end=time.monotonic()+seconds
    model_times=[]
    recent=[]
    while time.monotonic()<end:
        try:
            timestamp,line=lines.get(timeout=.1)
        except queue.Empty:
            if process.poll() is not None:
                raise RuntimeError(process.stderr.read() or '\n'.join(recent[-8:]) or 'Compiled application exited early')
            continue
        recent.append(line)
        if line.startswith('MEASURE model_ms '):
            model_times.append(float(line.split()[-1]))
        if line.startswith(marker):
            return timestamp,line,model_times
    raise TimeoutError(f'No {marker} event')


def main():
    output={'platform':'Fedora 44 x86_64','display_session':os.environ.get('XDG_SESSION_TYPE','unknown'),'gdk_backend':os.environ.get('GDK_BACKEND','toolkit default'),'gsk_renderer':os.environ.get('GSK_RENDERER','toolkit default'),'method':'/proc process RSS/HWM and CPU ticks; GtkColumnView model timing. One fresh process per run; warm filesystem cache. No cache flushing or privileged measurements.','limits':{'rows_per_page':200,'page_bytes':4194304,'cell_bytes':262144,'max_tabs':4},'startup':{},'workloads':[]}
    with tempfile.TemporaryDirectory(prefix='puff-measure-') as directory:
        root=Path(directory)
        env=os.environ.copy()
        env.update(PUFF_MEASURE='1',XDG_CONFIG_HOME=str(root/'config'))
        process,lines,begun=start(env)
        try:
            timestamp,line,_=until(process,lines,'MEASURE ready_ms ')
            output['startup']['process_to_mapped_ms']=(timestamp-begun)*1000
            output['startup']['application_to_mapped_ms']=float(line.split()[-1])
            time.sleep(3)
            samples=[]
            for _ in range(3):
                before=snapshot(process.pid);time.sleep(3);after=snapshot(process.pid)
                samples.append(100*(after['ticks']-before['ticks'])/HZ/3)
            output['startup'].update(idle=after,idle_cpu_one_core_percent=sorted(samples)[1],idle_cpu_samples_percent=samples)
        finally:
            process.terminate();process.wait(timeout=5)
        path=root/'large.sqlite'
        connection=sqlite3.connect(path)
        connection.execute('CREATE TABLE t(id INTEGER PRIMARY KEY,value TEXT)')
        connection.executemany('INSERT INTO t VALUES (?,?)',((i,'representative data '*4) for i in range(100000)))
        connection.commit();connection.close()
        for count in (1,4):
            env['PUFF_MEASURE_TABS']=str(count)
            process,lines,begun=start(env,'--self-test',str(path))
            try:
                _,_,models=until(process,lines,'MEASURE large_page')
                time.sleep(.4)
                loaded=snapshot(process.pid)
                if count==1 and os.environ.get('PUFF_SCREENSHOT'):
                    try:
                        subprocess.run(['import','-window','My Puff',str(ROOT/'docs/puff-native.png')],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=False,timeout=1)
                    except subprocess.TimeoutExpired:
                        pass
                _,_,more_models=until(process,lines,'MEASURE released')
                time.sleep(.25)
                released=snapshot(process.pid)
                until(process,lines,'MEASURE disconnected')
                time.sleep(.4)
                disconnected=snapshot(process.pid)
                code=process.wait(timeout=10)
                if code:
                    raise RuntimeError(process.stderr.read())
                output['workloads'].append({'connections_and_tabs':count,'fixture_rows':100000,'visible_page_rows':200,'loaded':loaded,'result_released':released,'all_tabs_disconnected':disconnected,'model_update_ms':models+more_models})
            finally:
                if process.poll() is None:
                    process.terminate();process.wait(timeout=5)
    (ROOT/'docs/native-performance.json').write_text(json.dumps(output,indent=2)+'\n')
    print(json.dumps(output,indent=2))

if __name__=='__main__':
    main()
