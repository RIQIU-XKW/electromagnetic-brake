@echo off
rem ---------------------------------------------------------------------------
rem  Double-click this after ANY hardware change (new XSA / regenerated platform).
rem  It refreshes the FSBL's ps7_init.c/.h from the current hardware handoff,
rem  rebuilds the FSBL, regenerates BOOT.bin and verifies what is inside them.
rem
rem  Then re-flash BOOT.bin the usual way (Vitis -> Program Flash).
rem  Add " -Force" if you want a rebuild even when nothing changed.
rem ---------------------------------------------------------------------------
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0refresh_fsbl_psinit.ps1" %*
echo.
pause
