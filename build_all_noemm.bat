@echo off
setlocal EnableExtensions EnableDelayedExpansion
set "ROOT=%~dp0"
cd /d "%ROOT%"

set "CPU_TARGET=286"
if not "%~1"=="" if not "%~1:~0,1%"=="-" (
    set "CPU_TARGET=%~1"
    shift
)
if not "!CPU_TARGET!"=="286" (
    echo CPU target '!CPU_TARGET!' is not enabled in build_all: the 386 branch is currently untested. 1>&2
    exit /b 2
)

set "EXTRA_ARGS="
:collect_args
if "%~1"=="" goto args_done
if /I "%~1"=="--emm" (
    echo build_all controls EMM itself and builds both OFF and ON variants; do not pass --emm. 1>&2
    exit /b 2
)
set "EXTRA_ARGS=!EXTRA_ARGS! %~1"
shift
goto collect_args
:args_done
set /a COUNT=0
set "TOTAL=32"

for %%B in (M1 M2 PC Z2 C2) do (
    for %%V in (MCGA EGA128 VGA128 VGA256) do (
        if "%%B"=="PC" (
            call :build_one %%B %%V PWM OFF
            if errorlevel 1 exit /b !errorlevel!
        ) else if "%%B"=="C2" (
            call :build_one %%B %%V I2S OFF
            if errorlevel 1 exit /b !errorlevel!
        ) else (
            call :build_one %%B %%V I2S OFF
            if errorlevel 1 exit /b !errorlevel!
            call :build_one %%B %%V PWM OFF
            if errorlevel 1 exit /b !errorlevel!
        )
    )
)

echo.
echo All %TOTAL% supported 286 variants ^(without EMM^) built. UF2 files are under bin/^<build-type^>/.
exit /b 0

:build_one
set /a COUNT+=1
set "B=%~1"
set "V=%~2"
set "A=%~3"
set "E=%~4"
set "TAG=!B!-286-!V!-!A!"
set "EMM_ARG="
if /I "!E!"=="ON" (
    set "TAG=!TAG!-emm"
    set "EMM_ARG=--emm"
)
echo.
echo [!COUNT!/%TOTAL%] !TAG!
call "%ROOT%build.bat" --board !B! --video !V! --audio !A! --build-dir "%ROOT%build\all\!TAG!" !EMM_ARG! %EXTRA_ARGS%
exit /b %errorlevel%
