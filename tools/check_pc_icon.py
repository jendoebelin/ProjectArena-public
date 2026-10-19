"""Check that a PC build carries its icon (every release must): python -I tools/check_pc_icon.py ProjectArena.exe

Exits 1 when the exe has no icon resource (app/src/pc/ProjectArena.rc, embedded by tools/build_pc.bat).
"""
import ctypes
import sys
from ctypes import wintypes as w

k = ctypes.WinDLL('kernel32', use_last_error=True)
k.LoadLibraryExW.restype = ctypes.c_void_p
k.LoadLibraryExW.argtypes = [w.LPCWSTR, ctypes.c_void_p, w.DWORD]
k.FindResourceW.restype = ctypes.c_void_p
k.FindResourceW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
k.SizeofResource.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
k.FreeLibrary.argtypes = [ctypes.c_void_p]
LOAD_LIBRARY_AS_DATAFILE, RT_GROUP_ICON = 2, 14

h = k.LoadLibraryExW(sys.argv[1], None, LOAD_LIBRARY_AS_DATAFILE)
if not h:
    sys.exit('cannot open %s' % sys.argv[1])
r = k.FindResourceW(h, 1, RT_GROUP_ICON)
sizes = (k.SizeofResource(h, r) - 6) // 14 if r else 0
k.FreeLibrary(h)
if not sizes:
    sys.exit('NO ICON in %s: build it with tools/build_pc.bat (it embeds app/src/pc/ProjectArena.ico)' % sys.argv[1])
print('icon: %d sizes in %s' % (sizes, sys.argv[1]))
