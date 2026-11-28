CFLAGS = -O2 -s -std=gnu11 -w -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-error=implicit-function-declaration
LIBS = -lgdi32 -luser32 -lshell32 -lcomdlg32 -lcomctl32 -lole32 -luuid -lshlwapi -ladvapi32 -lmsimg32 -lwinmm -static

all: WinShellFM_x86.exe WinShellFM_x64.exe FilesViewer_x86.exe FilesViewer_x64.exe

app_x86.o: app.rc app.ico
	i686-w64-mingw32-windres app.rc -O coff -o $@
app_x64.o: app.rc app.ico
	x86_64-w64-mingw32-windres app.rc -O coff -o $@

WinShellFM_x86.exe: winshell_fm.c app_x86.o
	i686-w64-mingw32-gcc $(CFLAGS) -municode -mwindows winshell_fm.c app_x86.o -o $@ $(LIBS)

WinShellFM_x64.exe: winshell_fm.c app_x64.o
	x86_64-w64-mingw32-gcc $(CFLAGS) -municode -mwindows winshell_fm.c app_x64.o -o $@ $(LIBS)

FilesViewer_x86.exe: winshell_fm.c app_x86.o
	i686-w64-mingw32-gcc $(CFLAGS) -DVIEWER_EXE -municode -mwindows winshell_fm.c app_x86.o -o $@ $(LIBS)

FilesViewer_x64.exe: winshell_fm.c app_x64.o
	x86_64-w64-mingw32-gcc $(CFLAGS) -DVIEWER_EXE -municode -mwindows winshell_fm.c app_x64.o -o $@ $(LIBS)

clean:
	rm -f WinShellFM_*.exe FilesViewer_*.exe app_*.o

.PHONY: all clean
