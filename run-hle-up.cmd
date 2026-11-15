@echo off
setlocal
set "XB_HLE_D3D8_BUFFERS=off"
set "XB_HLE_D3D8_UP=1"
set "XB_HLE_D3D8_INDEXED=off"
set "XB_HLE_D3D8_RESOURCES=off"
pushd "%~dp0build"
if errorlevel 1 exit /b 1
ironphoenix.exe --game "%~dp0extracted" --renderer opengl --hz 60 --scale 3 %*
set "game_result=%errorlevel%"
popd
exit /b %game_result%
