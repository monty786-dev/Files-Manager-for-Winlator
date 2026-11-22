# Files (Winlator/Wine 🍷) – native C file manager

Made & developed by Monty · https://github.com/Montydev786/Winlator-Wine-Graphical-Interface-Components-applications
Support us and give a ⭐ Star.

A native Win32/GDI file manager for Winlator / Wine. One C file, no runtime needed.

## Build the .exe on GitHub (no PC tools needed)
1. Create a new GitHub repository.
2. Upload ALL files from this folder, keeping the structure:
   `winshell_fm.c`, `app.rc`, `app.ico`, `Makefile`, `.gitignore`, `.github/workflows/build.yml`.
   (The `.github` folder must be uploaded exactly with that path.)
3. Open the **Actions** tab → **Build WinShellFM** → **Run workflow** (it also runs on every push).
4. When it is green, open the run → **Artifacts** → download **WinShellFM**
   (contains `WinShellFM_x86.exe`, `WinShellFM_x64.exe` and build logs).
5. If it is red: open the run page, the **Compiler errors** box shows the first errors.
   Copy them (or `build_x86.log` from the artifact) and send them for a fix.
6. Release build: push a tag such as `v1.0`; the exes are attached to the Release.

Local build on Linux: `sudo apt install gcc-mingw-w64 binutils-mingw-w64` then `make`.

## Run in Winlator
Copy the exe to a folder visible to the container (e.g. Downloads → `D:` / `Z:`), then run it from
Winlator's file picker or add it as a shortcut. Use the x86 build if unsure; x64 needs a 64-bit container.
Use *Create shortcut* in the app to put a launcher on the Wine desktop.

## Optional helpers
- RAR, 7z, bz2, xz etc.: place `7z.exe` (7-Zip) next to the app or install it. Creating RAR needs `rar.exe`.
- ZIP, TAR, TAR.GZ, GZ, ISO extract and ZIP/TAR/TGZ compress are built in.

## Known limitations
- Not compile-tested on real Windows/MinGW by the author; if the Action fails, send the error summary.
- PDF preview is text only (no page rendering; scanned/encrypted PDFs show an error).
- Legacy .doc/.xls/.ppt are not previewed (docx/xlsx/pptx only).
- Video uses Windows MCI; codecs depend on your Wine/Winlator build. Video thumbnails are an icon with a play badge.
- Previews are built on the main thread, so huge files can stall briefly.
- PNG custom icons appear on an opaque background. ISO support is untested.
- Kruti Dev conversion from the Python original is not ported.
- "Full screen" preview is a borderless topmost window.
