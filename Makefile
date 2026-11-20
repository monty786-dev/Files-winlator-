CFLAGS = -O2 -s -std=gnu11 -w -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-error=implicit-function-declaration
LIBS = -lgdi32 -luser32 -lshell32 -lcomdlg32 -lcomctl32 -lole32 -luuid -lshlwapi -ladvapi32 -lmsimg32 -static

all: WinShellFM_x86.exe WinShellFM_x64.exe

WinShellFM_x86.exe: winshell_fm.c
	i686-w64-mingw32-gcc $(CFLAGS) -municode -mwindows $< -o $@ $(LIBS)

WinShellFM_x64.exe: winshell_fm.c
	x86_64-w64-mingw32-gcc $(CFLAGS) -municode -mwindows $< -o $@ $(LIBS)

clean:
	rm -f WinShellFM_*.exe

.PHONY: all clean
