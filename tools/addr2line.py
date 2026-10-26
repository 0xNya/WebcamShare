"""Resolves module-relative offsets to function and source line via dbghelp.

    python -I tools/addr2line.py path/to/module.dll 0x642d [0x...]
"""
import ctypes
import sys
from ctypes import wintypes


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wintypes.ULONG), ('TypeIndex', wintypes.ULONG),
                ('Reserved', ctypes.c_uint64 * 2), ('Index', wintypes.ULONG),
                ('Size', wintypes.ULONG), ('ModBase', ctypes.c_uint64),
                ('Flags', wintypes.ULONG), ('Value', ctypes.c_uint64),
                ('Address', ctypes.c_uint64), ('Register', wintypes.ULONG),
                ('Scope', wintypes.ULONG), ('Tag', wintypes.ULONG),
                ('NameLen', wintypes.ULONG), ('MaxNameLen', wintypes.ULONG),
                ('Name', ctypes.c_char * 512)]


class IMAGEHLP_LINE64(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wintypes.DWORD), ('Key', ctypes.c_void_p),
                ('LineNumber', wintypes.DWORD), ('FileName', ctypes.c_char_p),
                ('Address', ctypes.c_uint64)]


def main():
    module = sys.argv[1]
    dbghelp = ctypes.WinDLL('dbghelp')
    proc = wintypes.HANDLE(0x777)
    dbghelp.SymSetOptions(0x2 | 0x10)
    dbghelp.SymInitializeW(proc, None, False)
    base = 0x10000000
    dbghelp.SymLoadModuleExW(proc, None, module, None, ctypes.c_uint64(base), 0, None, 0)

    for arg in sys.argv[2:]:
        addr = base + int(arg, 16)
        sym = SYMBOL_INFO()
        sym.SizeOfStruct, sym.MaxNameLen = 88, 511
        disp = ctypes.c_uint64()
        name = '?'
        if dbghelp.SymFromAddr(proc, ctypes.c_uint64(addr), ctypes.byref(disp), ctypes.byref(sym)):
            name = f'{sym.Name.decode()}+0x{disp.value:x}'
        line = IMAGEHLP_LINE64()
        line.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINE64)
        d32 = wintypes.DWORD()
        loc = ''
        if dbghelp.SymGetLineFromAddr64(proc, ctypes.c_uint64(addr), ctypes.byref(d32), ctypes.byref(line)):
            loc = f'{line.FileName.decode(errors="replace")}:{line.LineNumber}'
        print(f'{arg}: {name}  {loc}')


if __name__ == '__main__':
    main()
