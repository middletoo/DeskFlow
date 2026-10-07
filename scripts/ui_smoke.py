"""Exercise the native host in an isolated data directory and retain visual evidence."""
import ctypes, json, pathlib, subprocess, time, sys
from PIL import ImageGrab
root=pathlib.Path(__file__).resolve().parents[1]
output=root/'artifacts'/'validation'
output.mkdir(parents=True,exist_ok=True)
history_mode='--history' in sys.argv
data=output/('history-benchmark-data' if history_mode else 'smoke-data')
data.mkdir(exist_ok=True)
started=time.perf_counter()
process=subprocess.Popen([str(root/'build'/'Release'/'DeskFlow.exe'),'--smoke-test','--data',str(data)],creationflags=subprocess.CREATE_NO_WINDOW)
user32=ctypes.WinDLL('user32',use_last_error=True)
user32.FindWindowW.argtypes=[ctypes.c_wchar_p,ctypes.c_wchar_p]
user32.FindWindowW.restype=ctypes.c_void_p
deadline=time.monotonic()+3
hwnd=None
while time.monotonic()<deadline:
    hwnd=user32.FindWindowW('DeskFlowPanel','DeskFlow')
    if hwnd:
        pid=ctypes.c_ulong();user32.GetWindowThreadProcessId.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_ulong)];user32.GetWindowThreadProcessId(hwnd,ctypes.byref(pid))
        if pid.value==process.pid: break
        hwnd=None
    if process.poll() is not None: raise RuntimeError('Host exited before creating the native window')
    time.sleep(.03)
if not hwnd: process.terminate();raise RuntimeError('Native host window did not appear')
window_ms=(time.perf_counter()-started)*1000
class Rect(ctypes.Structure):
    _fields_=[('left',ctypes.c_long),('top',ctypes.c_long),('right',ctypes.c_long),('bottom',ctypes.c_long)]
user32.GetWindowRect.argtypes=[ctypes.c_void_p,ctypes.POINTER(Rect)]
ctypes.windll.user32.SetProcessDPIAware()
user32.SetWindowPos.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_uint]
# Raise only this test process's own window without changing the user's keyboard focus.
user32.SetWindowPos(hwnd,ctypes.c_void_p(-1),0,0,0,0,0x0053)
if history_mode:
    user32.PostMessageW.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.c_size_t,ctypes.c_ssize_t]
    user32.PostMessageW(hwnd,0x0312,2,0)
time.sleep(.5)
rectangle=Rect();user32.GetWindowRect(hwnd,ctypes.byref(rectangle))
ImageGrab.grab(bbox=(rectangle.left,rectangle.top,rectangle.right,rectangle.bottom)).save(output/('native-history.png' if history_mode else 'native-panel.png'))
user32.SetWindowPos(hwnd,ctypes.c_void_p(-2),0,0,0,0,0x0053)
if process.wait(timeout=12)!=0: raise RuntimeError('Host smoke run failed')
report=json.loads((data/'smoke.json').read_text())
report['externalWindowCreationMs']=round(window_ms,1)
(output/('ui-history.json' if history_mode else 'ui-smoke.json')).write_text(json.dumps(report,indent=2),encoding='utf8')
assert report['windowCreated']
assert report['privateBytes']<80*1024*1024, report
print(json.dumps(report,ensure_ascii=False))
