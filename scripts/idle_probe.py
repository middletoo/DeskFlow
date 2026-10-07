"""Measure Host + ordinary index helper with no live user clipboard data."""
import ctypes,json,pathlib,subprocess,time,os,uuid
from ctypes import wintypes
root=pathlib.Path(__file__).resolve().parents[1]
out=root/'artifacts'/'validation';fixture=out/('idle-'+uuid.uuid4().hex);data=fixture/'data';empty=fixture/'empty'
data.mkdir(parents=True,exist_ok=True);empty.mkdir(parents=True,exist_ok=True)
duration=65
host=subprocess.Popen([str(root/'build'/'Release'/'DeskFlow.exe'),'--idle-test',str(duration),'--data',str(data)],creationflags=subprocess.CREATE_NO_WINDOW)
worker=subprocess.Popen([str(root/'build'/'Release'/'DeskIndex.exe'),'--data',str(data),'--root',str(empty),'--parent',str(host.pid)],creationflags=subprocess.CREATE_NO_WINDOW)
user=ctypes.WinDLL('user32');kernel=ctypes.WinDLL('kernel32');psapi=ctypes.WinDLL('psapi')
callback_type=ctypes.WINFUNCTYPE(wintypes.BOOL,wintypes.HWND,wintypes.LPARAM)
user.EnumWindows.argtypes=[callback_type,wintypes.LPARAM]
user.GetClassNameW.argtypes=[wintypes.HWND,wintypes.LPWSTR,ctypes.c_int]
user.GetWindowThreadProcessId.argtypes=[wintypes.HWND,ctypes.POINTER(wintypes.DWORD)]
user.PostMessageW.argtypes=[wintypes.HWND,wintypes.UINT,wintypes.WPARAM,wintypes.LPARAM]
kernel.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD];kernel.OpenProcess.restype=wintypes.HANDLE
kernel.CloseHandle.argtypes=[wintypes.HANDLE]
class Memory(ctypes.Structure):
    _fields_=[('cb',wintypes.DWORD),('pageFaults',wintypes.DWORD),('peakWorkingSet',ctypes.c_size_t),('workingSet',ctypes.c_size_t),('peakPaged',ctypes.c_size_t),('paged',ctypes.c_size_t),('peakNonPaged',ctypes.c_size_t),('nonPaged',ctypes.c_size_t),('pagefile',ctypes.c_size_t),('peakPagefile',ctypes.c_size_t),('private',ctypes.c_size_t)]
psapi.GetProcessMemoryInfo.argtypes=[wintypes.HANDLE,ctypes.POINTER(Memory),wintypes.DWORD]
kernel.GetProcessTimes.argtypes=[wintypes.HANDLE,ctypes.POINTER(wintypes.FILETIME),ctypes.POINTER(wintypes.FILETIME),ctypes.POINTER(wintypes.FILETIME),ctypes.POINTER(wintypes.FILETIME)]
handles=[kernel.OpenProcess(0x0410,False,pid) for pid in (host.pid,worker.pid)]
def sample():
    values=[]
    for handle in handles:
        m=Memory();m.cb=ctypes.sizeof(m);psapi.GetProcessMemoryInfo(handle,ctypes.byref(m),m.cb)
        c,e,k,u=[wintypes.FILETIME() for _ in range(4)];kernel.GetProcessTimes(handle,ctypes.byref(c),ctypes.byref(e),ctypes.byref(k),ctypes.byref(u))
        cpu=((k.dwHighDateTime<<32)+k.dwLowDateTime+(u.dwHighDateTime<<32)+u.dwLowDateTime)/1e7
        values.append({'privateBytes':m.private,'workingSet':m.workingSet,'cpuSeconds':cpu})
    return values
try:
    time.sleep(2)
    found=[]
    @callback_type
    def locate(window,_):
        pid=wintypes.DWORD();user.GetWindowThreadProcessId(window,ctypes.byref(pid))
        name=ctypes.create_unicode_buffer(128);user.GetClassNameW(window,name,128)
        if pid.value==host.pid and name.value=='DeskFlowPanel':found.append(window);return False
        return True
    user.EnumWindows(locate,0)
    if not found:raise RuntimeError('Owned host window not found')
    hwnd=found[0]
    user.PostMessageW(hwnd,0x0010,0,0) # Hide this host; retain its normal initialized UI resources.
    start=time.monotonic();initial=sample();samples=[]
    while time.monotonic()-start<60:
        time.sleep(1);samples.append(sample())
    end=time.monotonic();final=sample()
    cpu=sum(final[i]['cpuSeconds']-initial[i]['cpuSeconds'] for i in range(2))
    report={'durationSeconds':round(end-start,3),'logicalProcessors':os.cpu_count(),'normalizedCpuPercent':cpu/(end-start)/os.cpu_count()*100,'aggregatePrivateBytesMax':max(sum(p['privateBytes'] for p in point) for point in samples),'aggregateWorkingSetBytesMax':max(sum(p['workingSet'] for p in point) for point in samples),'samples':len(samples),'conditions':'Initialized native UI hidden; ordinary helper watches empty synthetic directory; clipboard notifications suppressed from reading user data; no active OCR/translation/index rebuild','initial':initial,'final':final}
    (out/'idle-probe.json').write_text(json.dumps(report,indent=2),encoding='utf8')
    assert report['aggregatePrivateBytesMax']<=80*1024*1024,report
    assert report['normalizedCpuPercent']<=.2,report
    print(json.dumps(report))
finally:
    for h in handles:
        if h:kernel.CloseHandle(h)
    host.wait(timeout=10);worker.wait(timeout=10)
