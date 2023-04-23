setlocal

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"

set build=build\win32-x86
set pwd=%~dp0

if /I [%1] == [rebuild] (
	rd /q /s %build%
	set option="-t:Rebuild"
)

if not exist %build% (
	mkdir %build% && cd %build%
	cmake %pwd% -A Win32 -DBELL_EXTERNAL_MBEDTLS=%pwd%\..\common\libmbedtls
) else (
	cd %build%
)

msbuild "spotraop.sln" -p:Configuration=Release -p:Platform=Win32 %option%

robocopy Release %pwd%\bin *.exe /NDL /NJH /NJS /nc /ns /np

endlocal

