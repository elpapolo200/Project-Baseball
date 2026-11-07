@echo off
rem Compila el traductor de shaders de Vita3K (GPLv2) como v3k.lib.
rem spv=v3kspv: el SpvBuilder de glslang fc9889c (el que usa Vita3K) va en otro
rem espacio de nombres para no chocar con el glslang 16 que usa shc.c.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set V3K=%~dp0
set DEPS=%V3K%..\..\..\deps
set GL=%DEPS%\glslang-v3k
if not exist "%V3K%obj" mkdir "%V3K%obj"
cd /d "%V3K%obj"
del /q *.obj 2>nul
cl /nologo /c /O2 /MD /MP /EHsc /std:c++20 /Zc:preprocessor /Zc:__cplusplus /bigobj /W0 /utf-8 ^
  /Dspv=v3kspv /DFMT_HEADER_ONLY /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
  /I"%V3K%include" /I"%GL%" /I"%GL%\SPIRV" /I"%DEPS%\fmt\include" ^
  "%V3K%src\v3k_api.cpp" "%V3K%src\shader\*.cpp" "%V3K%src\shader\translator\*.cpp" "%V3K%src\gxm\*.cpp" ^
  "%GL%\SPIRV\SpvBuilder.cpp" "%GL%\SPIRV\SpvPostProcess.cpp" "%GL%\SPIRV\InReadableOrder.cpp" ^
  "%GL%\SPIRV\disassemble.cpp" "%GL%\SPIRV\doc.cpp" "%GL%\SPIRV\Logger.cpp"
if errorlevel 1 exit /b 1
lib /nologo /OUT:"%V3K%v3k.lib" *.obj
if errorlevel 1 exit /b 1
echo v3k.lib listo
