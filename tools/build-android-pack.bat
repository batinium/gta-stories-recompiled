@echo off
rem [android] Windows: your own disc -> the game library for your phone, in one command (or a double-click).
rem
rem     tools\build-android-pack.bat [vcs|lcs] ["C:\path\to\your game.iso"]
rem
rem Without a disc it asks for one (drag the .iso into the window). Runs tools\android_pack.py, the same script as the
rem launcher's "Prepare for Android" button and tools/build-android-pack.command on macOS (the same steps and messages;
rem docs/ANDROID.md "PC builder: Windows vs macOS"): it checks the tools, offers to download the missing ones (it lists
rem them with their sizes and asks y/N; tools\android.py setup), recompiles the game from your disc, builds the library
rem with the Android NDK and copies it to android-export\<game>\. The game is read from the disc when not given.
rem Python: the release's python\ folder if there is one, else the py launcher, else python on PATH.
rem Keep the folder short (C:\gta): a long path hits Windows' 260-character limit (checked up front).
setlocal
set "GAMEARG="
set "ISO="
set "ASKED="
if /i "%~1"=="vcs" (set "GAMEARG=--game vcs" & shift)
if /i "%~1"=="lcs" (set "GAMEARG=--game lcs" & shift)
set "ISO=%~1"

echo [pack] Prepare for Android: your disc -^> the game library for your phone (tools\android_pack.py).
if not "%ISO%"=="" goto have_iso
set "ASKED=1"
echo [pack] Drag your disc image (.iso) into this window, or type its path, then press Enter:
set /p "ISO="
if not defined ISO goto usage
rem a dragged path comes in quotes
set "ISO=%ISO:"=%"
if not defined ISO goto usage
:have_iso
if exist "%ISO%" goto iso_ok
echo [pack] The disc image was not found: %ISO%
goto usage
:iso_ok

set "ROOT=%~dp0.."
set "PY="
if exist "%ROOT%\python\python.exe" goto use_release_python
rem the py launcher can exist with no Python behind it: try it
py -3 -c "import sys" >nul 2>nul
if not errorlevel 1 goto use_py
python -c "import sys" >nul 2>nul
if not errorlevel 1 goto use_python
echo [pack] No Python found: there is no python folder here and no Python 3 on this computer.
echo [pack] Install Python 3 from https://www.python.org/downloads/ (on Windows tick 'Add python.exe to PATH'; on a Mac also: xcode-select --install), then open a NEW window and run this again.
goto failed

:use_release_python
set PY="%ROOT%\python\python.exe"
goto have_python
:use_py
set "PY=py -3"
goto have_python
:use_python
set "PY=python"

:have_python
echo [pack] Python: %PY%
cd /d "%ROOT%"
if errorlevel 1 goto failed

%PY% tools\android_pack.py %GAMEARG% "%ISO%"
if errorlevel 1 goto failed

echo [pack] Done: copy the file named above (libvcs_game.so.gz) to your phone and import it in the app: Disc ^> Import game library.
echo [pack] It holds game code from your disc: for your own phone only, never share it.
if defined ASKED set /p "_=[pack] Press Enter to close this window. " <con
exit /b 0

:failed
echo [pack] Stopped: see the message above, then run this again.
if defined ASKED set /p "_=[pack] Press Enter to close this window. " <con
exit /b 1

:usage
echo Usage: tools\build-android-pack.bat [vcs^|lcs] "C:\path\to\your game.iso"
echo   vcs = Vice City Stories (USA, SLUS_215.90)    lcs = Liberty City Stories (USA, SLUS_214.23); default: read from the disc
if defined ASKED set /p "_=[pack] Press Enter to close this window. " <con
exit /b 1
