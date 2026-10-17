@echo off
rem Builds PointDownMe.dll with the MSVC x64 tools (Visual Studio 2019 or newer with "Desktop development with C++").
rem No Windows SDK and no C runtime are needed: the few Win32 functions are declared in the source and their import
rem libraries are generated from kernel32.def / user32.def.
rem Run it from any command prompt; it finds Visual Studio with vswhere when cl.exe is not on PATH.
setlocal
cd /d %~dp0
where cl >nul 2>nul
if errorlevel 1 (
  for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul
  )
)
where cl >nul 2>nul || (echo MSVC x64 tools not found & exit /b 1)
lib /nologo /def:kernel32.def /machine:x64 /out:kernel32.lib || exit /b 1
lib /nologo /def:user32.def /machine:x64 /out:user32.lib || exit /b 1
cl /nologo /c /O2 /GS- /Gs999999 /Zl /std:c++17 PointDownMe.cpp || exit /b 1
link /nologo /DLL /NODEFAULTLIB /ENTRY:DllMain /MACHINE:X64 /OUT:PointDownMe.dll PointDownMe.obj kernel32.lib user32.lib || exit /b 1
echo built PointDownMe.dll
