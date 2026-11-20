# WinShell File Manager (C / Win32)

A native Windows file manager written in C with plain Win32 + GDI. It runs on Windows and under Wine / Winlator
on Android. 12 desktop looks (Windows 10/11/7, macOS, Ubuntu, Zorin, Mint, KDE, ChromeOS, XFCE, Total Commander,
Midnight Commander), tabs, search, image/text preview, and more.

## Get the .exe from GitHub (no PC needed)

1. Create a repository on github.com (New repository -> any name -> Create).
2. Upload the files:
   - **Add file -> Upload files**, add `winshell_fm.c`, `README.md`, `Makefile`, `.gitignore`. Commit.
   - **Add file -> Create new file**. In the name box type exactly `.github/workflows/build.yml`
     (typing the slashes makes the folders). Paste the contents of `build.yml`. Commit.
3. Open the **Actions** tab. A run called **Build WinShellFM** starts on its own.
   (If not: click the workflow name -> **Run workflow**.)
4. Wait 1-2 minutes for a green tick. Open the run and scroll to **Artifacts**.
5. Download **WinShellFM** (a zip). It contains `WinShellFM_x86.exe` and `WinShellFM_x64.exe`.

If the run is red, open the run page: the **Summary** section lists the compiler errors. Copy that block.

### Optional: a permanent download link
Create a tag that starts with `v` (Releases -> Draft a new release -> Choose a tag -> type `v1.0` -> Create new tag
-> Publish). The workflow attaches both .exe files to the release.

## Run it in Winlator

1. Copy `WinShellFM_x86.exe` to your phone (Download folder is fine).
2. In Winlator, open your container. Android's Download folder is normally available as a drive letter
   (commonly `D:`; if not, look under `Z:`). You can also copy the file into `C:\Program Files`.
3. Start the container, then run the exe from the Winlator desktop Start menu -> Run, or add it as a shortcut
   from the container's Shortcuts screen.
4. Use the **x86** build first. The x64 build only runs in a 64-bit container.

Settings are stored in `%PROGRAMDATA%\WinShell\fm_config.json` and the log is `%TEMP%\winshell_fm.log`.

## Build on your own computer (optional)

Install MinGW-w64, then run `make`. It produces `WinShellFM_x86.exe` and `WinShellFM_x64.exe`.

## Keys

Enter open - Backspace up - Alt+Left/Right back/forward - F2 rename - F5 refresh - Del delete -
Ctrl+C/X/V copy/cut/paste - Ctrl+A select all - Ctrl+T/W new/close tab - Ctrl+L address bar - Ctrl+F search -
Ctrl+Shift+N new folder - Ctrl+wheel zoom.

## Not included (compared with the Python version)

Office/PDF/archive preview, archive extract/compress, per-folder colours and custom icons, Open-with file
associations, Kruti Dev conversion, drag-and-drop and drag-box selection.
