@echo off
rem Decompiles functions from an already analyzed Ghidra project (no re-analysis).
rem Usage: decompile.cmd <project dir> <out dir> <timeout s> <address file|address>
rem Needs GHIDRA_HOME. Paths are passed in 8.3 form because analyzeHeadless.bat
rem cannot handle spaces or parentheses.
setlocal
if "%GHIDRA_HOME%"=="" (echo Set GHIDRA_HOME to the Ghidra folder & exit /b 1)
if not exist "%~2" mkdir "%~2"
for %%I in ("%GHIDRA_HOME%") do set GH=%%~sI
for %%I in ("%~1") do set PROJECT=%%~sI
for %%I in ("%~2") do set OUT=%%~sI
for %%I in ("%~dp0.") do set SCRIPTS=%%~sI
set TARGET=%~4
if exist "%~4" for %%I in ("%~4") do set TARGET=%%~sI
if "%GHIDRA_MAXMEM%"=="" set GHIDRA_MAXMEM=6G
call %GH%\support\analyzeHeadless.bat %PROJECT% NovaDrift -process NovaDrift.exe -noanalysis -readOnly ^
    -scriptPath %SCRIPTS% -postScript DecompileToFiles.java %OUT% %3 %TARGET%
