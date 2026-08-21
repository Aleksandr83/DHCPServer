@echo off
rem PlatformIO calls "$MKFSTOOL" for the FS image builder. This batch file is a
rem single-token stand-in that runs mkfs_wrapper.py (which calls the ESP-IDF
rem spiffsgen.py instead of the broken mkspiffs).
python "%~dp0mkfs_wrapper.py" %*
exit /b %errorlevel%
