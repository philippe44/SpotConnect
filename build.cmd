setlocal

set pwd=%~dp0

cd spotupnp
call build %1 %2
cd ..

cd spotraop
call build %1 %2
cd ..


endlocal

