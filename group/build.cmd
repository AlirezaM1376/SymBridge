@echo off
rem set PATH=C:\Program Files (x86)\CSL Arm Toolchain\bin;%PATH%
call abld reallyclean gcce || goto :err
call bldmake bldfiles || goto :err
call abld build gcce urel || goto :err
makesis symbridge.pkg symbridge.sis || goto :err
echo DONE: symbridge.sis (unsigned)
goto :eof
:err
echo BUILD FAILED