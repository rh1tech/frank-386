@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem Build one supported murm386/FRANK RP2350 firmware variant.
rem Production scripts intentionally build the 286 core only.

set "ROOT=%~dp0"
cd /d "%ROOT%"

set "BOARD=M1"
set "VIDEO_MODE=VGA256"
set "AUDIO=PWM"
set "CPU_SPEED=504"
set "PSRAM_SPEED=66"
set "BUILD_TYPE=Release"
set "BUILD_DIR=%ROOT%build"
set "JOBS="
set "CLEAN=0"
set "FORCE_HDMI=OFF"
set "FORCE_VGA=OFF"
set "DEBUG=OFF"
set "DIAG=OFF"
set "EMM=OFF"

:parse
if "%~1"=="" goto validate
if /I "%~1"=="-h" goto help
if /I "%~1"=="--help" goto help
if /I "%~1"=="--clean" set "CLEAN=1"& shift& goto parse
if /I "%~1"=="-clean" set "CLEAN=1"& shift& goto parse
if /I "%~1"=="--hdmi" set "FORCE_HDMI=ON"& set "FORCE_VGA=OFF"& shift& goto parse
if /I "%~1"=="--vga" set "FORCE_VGA=ON"& set "FORCE_HDMI=OFF"& shift& goto parse
if /I "%~1"=="--debug" set "DEBUG=ON"& shift& goto parse
if /I "%~1"=="--diag" set "DIAG=ON"& shift& goto parse
if /I "%~1"=="--emm" set "EMM=ON"& shift& goto parse

if /I "%~1"=="-M1" set "BOARD=M1"& shift& goto parse
if /I "%~1"=="-M2" set "BOARD=M2"& shift& goto parse
if /I "%~1"=="-PC" set "BOARD=PC"& shift& goto parse
if /I "%~1"=="-Z2" set "BOARD=Z2"& shift& goto parse
if /I "%~1"=="-C2" set "BOARD=C2"& shift& goto parse
if /I "%~1"=="-MCGA" set "VIDEO_MODE=MCGA"& shift& goto parse
if /I "%~1"=="-EGA128" set "VIDEO_MODE=EGA128"& shift& goto parse
if /I "%~1"=="-VGA128" set "VIDEO_MODE=VGA128"& shift& goto parse
if /I "%~1"=="-VGA256" set "VIDEO_MODE=VGA256"& shift& goto parse
if /I "%~1"=="-i2s" set "AUDIO=I2S"& shift& goto parse
if /I "%~1"=="-pwm" set "AUDIO=PWM"& shift& goto parse
if "%~1"=="-252" set "CPU_SPEED=252"& shift& goto parse
if "%~1"=="-378" set "CPU_SPEED=378"& shift& goto parse
if "%~1"=="-504" set "CPU_SPEED=504"& shift& goto parse

if /I "%~1"=="-b" goto board_arg
if /I "%~1"=="--board" goto board_arg
if /I "%~1"=="-v" goto video_arg
if /I "%~1"=="--video" goto video_arg
if /I "%~1"=="-a" goto audio_arg
if /I "%~1"=="--audio" goto audio_arg
if /I "%~1"=="-c" goto clock_arg
if /I "%~1"=="--clock" goto clock_arg
if /I "%~1"=="-p" goto psram_arg
if /I "%~1"=="--psram" goto psram_arg
if /I "%~1"=="--build-type" goto type_arg
if /I "%~1"=="--build-dir" goto dir_arg
if /I "%~1"=="-j" goto jobs_arg
if /I "%~1"=="--jobs" goto jobs_arg

echo Unknown option: %~1 1>&2
goto usage_error

:board_arg
if "%~2"=="" goto missing_arg
set "BOARD=%~2"& shift& shift& goto parse
:video_arg
if "%~2"=="" goto missing_arg
set "VIDEO_MODE=%~2"& shift& shift& goto parse
:audio_arg
if "%~2"=="" goto missing_arg
set "AUDIO=%~2"& shift& shift& goto parse
:clock_arg
if "%~2"=="" goto missing_arg
set "CPU_SPEED=%~2"& shift& shift& goto parse
:psram_arg
if "%~2"=="" goto missing_arg
set "PSRAM_SPEED=%~2"& shift& shift& goto parse
:type_arg
if "%~2"=="" goto missing_arg
set "BUILD_TYPE=%~2"& shift& shift& goto parse
:dir_arg
if "%~2"=="" goto missing_arg
set "BUILD_DIR=%~2"& shift& shift& goto parse
:jobs_arg
if "%~2"=="" goto missing_arg
set "JOBS=%~2"& shift& shift& goto parse

:missing_arg
echo Missing argument for %~1 1>&2
goto usage_error

:validate
for %%B in (M1 M2 PC Z2 C2) do if /I "!BOARD!"=="%%B" set "BOARD=%%B"& set "BOARD_OK=1"
if not defined BOARD_OK echo Invalid board: !BOARD! 1>&2& exit /b 2
for %%V in (MCGA EGA128 VGA128 VGA256) do if /I "!VIDEO_MODE!"=="%%V" set "VIDEO_MODE=%%V"& set "VIDEO_OK=1"
if not defined VIDEO_OK echo Invalid video mode: !VIDEO_MODE! 1>&2& exit /b 2
for %%A in (I2S PWM) do if /I "!AUDIO!"=="%%A" set "AUDIO=%%A"& set "AUDIO_OK=1"
if not defined AUDIO_OK echo Invalid audio type: !AUDIO! 1>&2& exit /b 2

if "!BOARD!"=="PC" set "AUDIO=PWM"
if "!BOARD!"=="C2" set "AUDIO=I2S"

if "!CLEAN!"=="1" if exist "!BUILD_DIR!" rmdir /s /q "!BUILD_DIR!"
if not exist "!BUILD_DIR!" mkdir "!BUILD_DIR!"

echo murm386 build
echo   CPU target : 286
echo   Board      : !BOARD!
echo   Video mode : !VIDEO_MODE!
echo   Audio      : !AUDIO!
echo   RP2350     : !CPU_SPEED! MHz
echo   PSRAM max  : !PSRAM_SPEED! MHz
echo   Build type : !BUILD_TYPE!
echo   Build dir  : !BUILD_DIR!
echo.

cmake -S "%ROOT%." -B "!BUILD_DIR!" -DCMAKE_BUILD_TYPE=!BUILD_TYPE! -DCPU_TARGET=286 -DBOARD=!BOARD! -DVIDEO_MODE=!VIDEO_MODE! -DAUDIO_TYPE=!AUDIO! -DCPU_SPEED=!CPU_SPEED! -DPSRAM_SPEED=!PSRAM_SPEED! -DFORCE_HDMI=!FORCE_HDMI! -DFORCE_VGA=!FORCE_VGA! -DDEBUG_ENABLED=!DEBUG! -DDIAG_ENABLED=!DIAG! -DEMM=!EMM!
if errorlevel 1 exit /b %errorlevel%
if defined JOBS (
    cmake --build "!BUILD_DIR!" --config "!BUILD_TYPE!" --parallel !JOBS!
) else (
    cmake --build "!BUILD_DIR!" --config "!BUILD_TYPE!" --parallel
)
exit /b %errorlevel%

:help
call :usage
exit /b 0

:usage_error
call :usage 1>&2
exit /b 2

:usage
echo Usage: build.bat [options]
echo.
echo Supported production CPU target: 286
echo.
echo   -b, --board M1^|M2^|PC^|Z2^|C2
echo   -v, --video MCGA^|EGA128^|VGA128^|VGA256
echo   -a, --audio I2S^|PWM
echo   -c, --clock MHz
echo   -p, --psram MHz
echo       --hdmi / --vga
echo       --debug / --diag / --emm
echo       --build-type TYPE
echo       --build-dir DIR
echo   -j, --jobs N
echo       --clean
echo.
echo Short forms: -M1 -M2 -PC -Z2 -C2, -MCGA -EGA128 -VGA128 -VGA256,
echo              -i2s -pwm, -252 -378 -504
exit /b 0
