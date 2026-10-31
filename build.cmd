@echo off
rem Builds Network.dll for x64 and x86 into dist\<arch>.
rem Usage: build.cmd [test]   "test" also builds the fault-injecting x64 test DLL into build\testdll.
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -all -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvarsall.bat`) do if not defined VCVARS set "VCVARS=%%i"
if not defined VCVARS (
	echo Visual C++ build tools not found.
	exit /b 1
)

call :build x64 dist\x64 || exit /b 1
call :build x86 dist\x86 || exit /b 1
if /i "%~1"=="test" (
	call :build x64 build\testdll /DNETWORK_TEST_HOOKS || exit /b 1
)
exit /b 0

:build
setlocal
set "ARCH=%~1"
set "OUT=%~dp0%~2"
set "EXTRA=%~3"
set "OBJ=%~dp0build\obj\%~2"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJ%" mkdir "%OBJ%"
call "%VCVARS%" %ARCH% >nul || exit /b 1
pushd "%~dp0src"
cl /nologo /c /std:c++17 /utf-8 /EHsc /O2 /MT /W3 /GS /Gy /Zi /permissive- /DWIN32 /D_WINDOWS /DUNICODE /D_UNICODE /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_WARNINGS /D_CRT_NON_CONFORMING_SWPRINTFS /D_WIN32_WINNT=0x0601 %EXTRA% /I..\sdk /I. /Fo"%OBJ%\\" /Fd"%OBJ%\vc.pdb" Network.cpp NetCfg.cpp NetClass.cpp NetCommon.cpp NetFavorites.cpp NetScan.cpp NetDiscovery.cpp || (popd & exit /b 1)
rc /nologo /I..\sdk /I. /dUNICODE /fo"%OBJ%\Network.res" Network.rc || (popd & exit /b 1)
link /nologo /DLL /OPT:REF /OPT:ICF /DEBUG /DEF:Network.vc.def /MAP:"%OUT%\Network.map" /PDB:"%OUT%\Network.pdb" /IMPLIB:"%OBJ%\Network.lib" /OUT:"%OUT%\Network.dll" "%OBJ%\*.obj" "%OBJ%\Network.res" mpr.lib netapi32.lib user32.lib advapi32.lib ws2_32.lib winhttp.lib rpcrt4.lib || (popd & exit /b 1)
popd
echo Built %OUT%\Network.dll
endlocal
exit /b 0
