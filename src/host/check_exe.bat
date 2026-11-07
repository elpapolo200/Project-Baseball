@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
dumpbin /headers ..\mlb15_recomp.exe | findstr -i "version\|description\|copyright"