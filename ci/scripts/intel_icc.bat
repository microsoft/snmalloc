REM SPDX-FileCopyrightText: 2020 Intel Corporation
REM
REM SPDX-License-Identifier: MIT

set URL=%1
set COMPONENTS=%2

curl.exe --output webimage.exe --url %URL% --retry 5 --retry-delay 5
start /b /wait webimage.exe -s -x -f webimage_extracted --log extract.log
del webimage.exe
if "%COMPONENTS%"=="" (
  webimage_extracted\bootstrapper.exe -s --action install --eula=accept --continue-with-optional-error=yes -p=NEED_VS2017_INTEGRATION=0 -p=NEED_VS2019_INTEGRATION=0 --log-dir=.
) else (
  webimage_extracted\bootstrapper.exe -s --action install --components=%COMPONENTS% --eula=accept --continue-with-optional-error=yes -p=NEED_VS2017_INTEGRATION=0 -p=NEED_VS2019_INTEGRATION=0 --log-dir=.
)