@echo off
setlocal
pushd "%~dp0build"
if errorlevel 1 exit /b 1
ironphoenix.exe --game "%~dp0extracted" --renderer vulkan --hz 60 --scale 3 %*
set "game_result=%errorlevel%"
popd
exit /b %game_result%
