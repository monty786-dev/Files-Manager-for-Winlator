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

## Viewer app and Games tab (newest)
- **Files Viewer** (`FilesViewer_x86.exe` / `FilesViewer_x64.exe`): a separate small program built from the same source. Put it in the same folder as the main exe. Right-click a file > **Open in Viewer** (or drag a file onto the Viewer exe). It opens a normal resizable window with zoom, fit, page navigation, text selection and video controls. It never changes your settings. If the Viewer exe is missing, the main exe starts itself with `--viewer`.
- Slides (.pptx) now draw their pictures, coloured shapes (rectangle, rounded, ellipse, triangle, diamond, lines), outlines, theme colours, slide background and grouped shapes. Word (.docx) pages draw inline pictures. Not drawn: gradients, rotated shapes, shadows, charts/SmartArt, master/layout artwork, pictures inside PDF files, Excel drawings.
- **Games tab** (side pane > Games): add a game by choosing its save **folder or file(s)**. *Backup saves...* writes a .zip (with a manifest of the original locations). *Restore a saved backup...* asks whether to use the original location or another save folder, confirms before replacing files, and re-adds the game to the list. Game entries are part of the full Backup/Restore.

## What is new in this version
- Recycle Bin (own implementation, on/off in Settings > General). Delete = move to bin, Shift+Delete = permanent. Side-pane entry "Recycle Bin" with Restore / Empty.
- Confirmation boxes for copy, move (paste and drag-drop), delete, and when a preview is too large or cannot load (on/off in Settings).
- Crash guard: on an unexpected crash the app logs it, tells you and restarts in safe mode (no thumbnails/previews).
- Preview: "Fit to width" and "Fit whole page" buttons, big-icon details panel for folders, drives and unsupported files, size limit with "Load anyway".
- Video: first frame is shown (no black box), more formats, auto-detected external player (VLC, mpv, MPC-HC, WMP) via the "open externally" button; choose your own in Settings > Preview.
- Built-in coloured icon for every file extension; custom icons accept png/jpg/bmp/gif/tif/ico/exe/dll with transparency.
- This PC / devices page is drawn per look (Windows tiles with usage bars, macOS/Ubuntu big icons, Commander list).
- Toolbar elements (buttons, address, search) are arranged differently per look.
- Layout editor (Style menu, Settings > Look): drag to move, drag the corner handle to resize, right-click to hide/recolour/reset, add launcher buttons, save/load your own layout themes, navigation pane left or right.
- Computer tab (name selectable in Settings): Start Menu, Desktop, Program Files, system tools and anything you add; reorder with Move up/down.
- Tree view: arrows on drives and folders, auto-reveals the current folder, double-click expands.
- Full backup/restore (zip): settings, custom icons, fonts and desktop shortcuts; restore puts shortcuts back on the Wine desktop and in the Computer tab.
- Desktop shortcut for any file or folder (right-click > Create shortcut on Desktop).
- Double-click blank space = up one folder (on/off); adjustable blank drag-select area (Settings).
- 7-Zip / WinRAR auto-detected (x86 and x64 installs, registry, Program Files, next to the app).

## Optional helpers
- RAR, 7z, bz2, xz etc.: place `7z.exe` (7-Zip) next to the app or install it. Creating RAR needs `rar.exe`.
- ZIP, TAR, TAR.GZ, GZ, ISO extract and ZIP/TAR/TGZ compress are built in.

## Known limitations
- Not compile-tested on real Windows/MinGW by the author; if the Action fails, send the error summary.
- Video preview needs working Wine media codecs; if it stays black use the external player button.
- PDF preview is text only (no page rendering; scanned/encrypted PDFs show an error).
- Legacy .doc/.xls/.ppt are not previewed (docx/xlsx/pptx only).
- Video uses Windows MCI; codecs depend on your Wine/Winlator build. Video thumbnails are an icon with a play badge.
- Previews are built on the main thread, so huge files can stall briefly.
- PNG custom icons appear on an opaque background. ISO support is untested.
- Kruti Dev conversion from the Python original is not ported.
- "Full screen" preview is a borderless topmost window.
