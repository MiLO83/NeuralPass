@echo off
setlocal EnableExtensions

rem NeuralPass Windows x64/x86 builder.
rem Usage:
rem   build_windows.cmd                       Build x64 DirectML with Candy
rem   build_windows.cmd directml all x64      Build x64 DirectML with all models
rem   build_windows.cmd directml x86           Build 32-bit add-on plus x64 worker
rem   build_windows.cmd preview x86            Build 32-bit preview backend

cd /d "%~dp0"
set "NP_ROOT=%CD%"
set "NP_MODE=%~1"
set "NP_MODELS=%~2"
set "NP_ARCH=%~3"
if not defined NP_MODE set "NP_MODE=directml"
if not defined NP_DIRECTML_TEST set "NP_DIRECTML_TEST=ON"
if /I "%NP_MODELS%"=="x86" (
    set "NP_ARCH=x86"
    set "NP_MODELS="
)
if /I "%NP_MODELS%"=="x64" (
    set "NP_ARCH=x64"
    set "NP_MODELS="
)
if not defined NP_ARCH set "NP_ARCH=x64"

if /I not "%NP_MODE%"=="directml" if /I not "%NP_MODE%"=="preview" (
    echo ERROR: First argument must be directml or preview.
    echo Usage: build_windows.cmd [directml^|preview] [all] [x64^|x86]
    exit /b 2
)
if /I not "%NP_ARCH%"=="x64" if /I not "%NP_ARCH%"=="x86" (
    echo ERROR: Architecture must be x64 or x86.
    exit /b 2
)
if /I "%NP_ARCH%"=="x86" (
    set "NP_CMAKE_ARCH=Win32"
    set "NP_ADDON=NeuralPass.addon32"
    set "NP_PACKAGE_ARCH=windows-x86"
) else (
    set "NP_CMAKE_ARCH=x64"
    set "NP_ADDON=NeuralPass.addon64"
    set "NP_PACKAGE_ARCH=windows-x64"
)

echo.
echo ============================================================
echo  NeuralPass Windows builder - %NP_MODE% %NP_ARCH%
echo ============================================================
echo.

call :find_tools || exit /b 1
call :prepare_sdk || exit /b 1

if /I "%NP_MODE%"=="directml" (
    call :prepare_directml || exit /b 1
    call :prepare_models || exit /b 1
    set "NP_BUILD=%NP_ROOT%\build-windows-directml-%NP_ARCH%"
    set "NP_WORKER_BUILD=%NP_ROOT%\build-windows-directml-x64"
) else (
    set "NP_BUILD=%NP_ROOT%\build-windows-preview-%NP_ARCH%"
)

if /I "%NP_MODE%"=="directml" if /I "%NP_ARCH%"=="x86" if not exist "%NP_WORKER_BUILD%\Release\NeuralPassWorker.exe" (
    echo       Building the required x64 inference worker...
    cmake -S "%NP_ROOT%" -B "%NP_WORKER_BUILD%" -A x64 ^
      -DNEURALPASS_BUILD_ADDON=ON ^
      -DNEURALPASS_BUILD_TESTS=ON ^
      -DCMAKE_SUPPRESS_REGENERATION=ON ^
      -DNEURALPASS_TEST_DIRECTML=OFF ^
      -DRESHADE_SDK_DIR="%NP_ROOT%\external\reshade" ^
      -DVULKAN_HEADERS_DIR="%NP_ROOT%\external\vulkan-headers" ^
      -DONNXRUNTIME_ROOT="%NP_ROOT%\external\onnxruntime"
    if errorlevel 1 goto :failed
    cmake --build "%NP_WORKER_BUILD%" --config Release --target NeuralPassWorker neuralpass_onnx_smoke_tests --parallel
    if errorlevel 1 goto :failed
)

echo [4/6] Configuring CMake...
if /I "%NP_MODE%"=="directml" (
    if /I "%NP_ARCH%"=="x86" (
        cmake -S "%NP_ROOT%" -B "%NP_BUILD%" -A %NP_CMAKE_ARCH% ^
          -DNEURALPASS_BUILD_ADDON=ON ^
          -DNEURALPASS_BUILD_TESTS=ON ^
          -DCMAKE_SUPPRESS_REGENERATION=ON ^
          -DNEURALPASS_TEST_DIRECTML=OFF ^
          -DNEURALPASS_EXTERNAL_WORKER="%NP_WORKER_BUILD%\Release\NeuralPassWorker.exe" ^
          -DRESHADE_SDK_DIR="%NP_ROOT%\external\reshade" ^
          -DVULKAN_HEADERS_DIR="%NP_ROOT%\external\vulkan-headers" ^
          -DONNXRUNTIME_ROOT=
    ) else (
        cmake -S "%NP_ROOT%" -B "%NP_BUILD%" -A %NP_CMAKE_ARCH% ^
          -DNEURALPASS_BUILD_ADDON=ON ^
          -DNEURALPASS_BUILD_TESTS=ON ^
          -DCMAKE_SUPPRESS_REGENERATION=ON ^
          -DNEURALPASS_TEST_DIRECTML=%NP_DIRECTML_TEST% ^
          -DRESHADE_SDK_DIR="%NP_ROOT%\external\reshade" ^
          -DVULKAN_HEADERS_DIR="%NP_ROOT%\external\vulkan-headers" ^
          -DONNXRUNTIME_ROOT="%NP_ROOT%\external\onnxruntime"
    )
) else (
    cmake -S "%NP_ROOT%" -B "%NP_BUILD%" -A %NP_CMAKE_ARCH% ^
      -DNEURALPASS_BUILD_ADDON=ON ^
      -DNEURALPASS_BUILD_TESTS=ON ^
      -DCMAKE_SUPPRESS_REGENERATION=ON ^
      -DNEURALPASS_TEST_DIRECTML=OFF ^
      -DRESHADE_SDK_DIR="%NP_ROOT%\external\reshade" ^
      -DVULKAN_HEADERS_DIR="%NP_ROOT%\external\vulkan-headers" ^
      -DONNXRUNTIME_ROOT=
)
if errorlevel 1 goto :failed

echo [5/6] Building Release %NP_ARCH% and running tests...
cmake --build "%NP_BUILD%" --config Release --parallel
if errorlevel 1 goto :failed
ctest --test-dir "%NP_BUILD%" -C Release --output-on-failure
if errorlevel 1 goto :failed

echo [6/6] Assembling deployment folder...
if /I "%NP_ARCH%"=="x64" (
    set "NP_DIST=%NP_ROOT%\dist\NeuralPass"
) else (
    set "NP_DIST=%NP_ROOT%\dist\NeuralPass-x86"
)
if exist "%NP_DIST%" rmdir /S /Q "%NP_DIST%"
if errorlevel 1 (
    echo ERROR: Could not clean the deployment folder. Close programs using it and retry.
    goto :failed
)
if not exist "%NP_DIST%" mkdir "%NP_DIST%"
if not exist "%NP_DIST%\reshade-shaders\Shaders" mkdir "%NP_DIST%\reshade-shaders\Shaders"
if not exist "%NP_DIST%\third-party" mkdir "%NP_DIST%\third-party"

copy /Y "%NP_BUILD%\Release\%NP_ADDON%" "%NP_DIST%\%NP_ADDON%" >nul
if errorlevel 1 (
    echo ERROR: The expected add-on was not produced.
    goto :failed
)
copy /Y "%NP_BUILD%\Release\neuralpass_vulkan_runtime_tests.exe" "%NP_DIST%\NeuralPassVulkanTest.exe" >nul
if errorlevel 1 (
    echo ERROR: The native Vulkan evidence runner was not produced.
    goto :failed
)
copy /Y "%NP_ROOT%\shaders\NeuralPass.fx" "%NP_DIST%\reshade-shaders\Shaders\NeuralPass.fx" >nul
copy /Y "%NP_ROOT%\README.md" "%NP_DIST%\README.md" >nul
copy /Y "%NP_ROOT%\LICENSE" "%NP_DIST%\LICENSE.txt" >nul
copy /Y "%NP_ROOT%\docs\architecture.md" "%NP_DIST%\ARCHITECTURE.md" >nul
copy /Y "%NP_ROOT%\docs\compatibility.md" "%NP_DIST%\COMPATIBILITY.md" >nul
copy /Y "%NP_ROOT%\THIRD_PARTY_NOTICES.md" "%NP_DIST%\THIRD_PARTY_NOTICES.md" >nul
copy /Y "%NP_ROOT%\packaging\Install NeuralPass.cmd" "%NP_DIST%\Install NeuralPass.cmd" >nul
copy /Y "%NP_ROOT%\packaging\Uninstall NeuralPass.cmd" "%NP_DIST%\Uninstall NeuralPass.cmd" >nul
copy /Y "%NP_ROOT%\packaging\Diagnose NeuralPass.cmd" "%NP_DIST%\Diagnose NeuralPass.cmd" >nul
copy /Y "%NP_ROOT%\packaging\Install-NeuralPass.ps1" "%NP_DIST%\Install-NeuralPass.ps1" >nul
copy /Y "%NP_ROOT%\packaging\Uninstall-NeuralPass.ps1" "%NP_DIST%\Uninstall-NeuralPass.ps1" >nul
copy /Y "%NP_ROOT%\packaging\Diagnose-NeuralPass.ps1" "%NP_DIST%\Diagnose-NeuralPass.ps1" >nul
copy /Y "%NP_ROOT%\packaging\Validate NeuralPass Vulkan.cmd" "%NP_DIST%\Validate NeuralPass Vulkan.cmd" >nul
copy /Y "%NP_ROOT%\packaging\Validate-NeuralPassVulkan.ps1" "%NP_DIST%\Validate-NeuralPassVulkan.ps1" >nul
copy /Y "%NP_ROOT%\external\reshade\LICENSE.md" "%NP_DIST%\third-party\ReShade-LICENSE.txt" >nul
copy /Y "%NP_ROOT%\external\vulkan-headers\LICENSE.md" "%NP_DIST%\third-party\Vulkan-Headers-LICENSE.txt" >nul

if /I "%NP_MODE%"=="directml" (
    copy /Y "%NP_ROOT%\external\onnxruntime\lib\onnxruntime.dll" "%NP_DIST%\onnxruntime.dll" >nul
    copy /Y "%NP_ROOT%\external\onnxruntime\lib\onnxruntime_providers_shared.dll" "%NP_DIST%\onnxruntime_providers_shared.dll" >nul
    copy /Y "%NP_ROOT%\external\onnxruntime\lib\DirectML.dll" "%NP_DIST%\DirectML.dll" >nul
    if /I "%NP_ARCH%"=="x86" (
        copy /Y "%NP_WORKER_BUILD%\Release\NeuralPassWorker.exe" "%NP_DIST%\NeuralPassWorker.exe" >nul
        copy /Y "%NP_WORKER_BUILD%\Release\neuralpass_onnx_smoke_tests.exe" "%NP_DIST%\NeuralPassHardwareTest.exe" >nul
    ) else (
        copy /Y "%NP_BUILD%\Release\NeuralPassWorker.exe" "%NP_DIST%\NeuralPassWorker.exe" >nul
        copy /Y "%NP_BUILD%\Release\neuralpass_onnx_smoke_tests.exe" "%NP_DIST%\NeuralPassHardwareTest.exe" >nul
    )
    copy /Y "%NP_ROOT%\packaging\Validate NeuralPass Hardware.cmd" "%NP_DIST%\Validate NeuralPass Hardware.cmd" >nul
    copy /Y "%NP_ROOT%\packaging\Validate-NeuralPassHardware.ps1" "%NP_DIST%\Validate-NeuralPassHardware.ps1" >nul
    if not exist "%NP_DIST%\models\downloads" mkdir "%NP_DIST%\models\downloads"
    copy /Y "%NP_ROOT%\models\downloads\*.onnx" "%NP_DIST%\models\downloads\" >nul
    copy /Y "%NP_ROOT%\models\manifest.json" "%NP_DIST%\models\manifest.json" >nul
    copy /Y "%NP_ORT_PACKAGE%\LICENSE" "%NP_DIST%\third-party\ONNXRuntime-LICENSE.txt" >nul
    copy /Y "%NP_ORT_PACKAGE%\ThirdPartyNotices.txt" "%NP_DIST%\third-party\ONNXRuntime-NOTICES.txt" >nul
    copy /Y "%NP_DML_PACKAGE%\LICENSE.txt" "%NP_DIST%\third-party\DirectML-LICENSE.txt" >nul
    copy /Y "%NP_DML_PACKAGE%\ThirdPartyNotices.txt" "%NP_DIST%\third-party\DirectML-NOTICES.txt" >nul
)

%NP_PY% "%NP_ROOT%\tools\write_package_metadata.py" ^
  --package "%NP_DIST%" --mode "%NP_MODE%" --architecture "%NP_PACKAGE_ARCH%" --version "0.1.0-preview"
if errorlevel 1 goto :failed
%NP_PY% "%NP_ROOT%\tools\validate_package.py" "%NP_DIST%"
if errorlevel 1 goto :failed
powershell -NoProfile -ExecutionPolicy Bypass -File "%NP_ROOT%\tests\package_lifecycle_tests.ps1" -Package "%NP_DIST%"
if errorlevel 1 goto :failed

echo.
echo ============================================================
echo  BUILD SUCCEEDED
echo ============================================================
echo Ready-to-copy package:
echo   %NP_DIST%
echo.
echo Install ReShade with full add-on support into an offline game,
echo then copy the CONTENTS of that folder beside the game executable.
echo Keep "NeuralPass (keep last)" last in the technique order.
echo.
exit /b 0

:find_tools
echo [1/6] Checking build tools...
where git >nul 2>nul || (
    echo ERROR: Git was not found in PATH.
    exit /b 1
)
where cmake >nul 2>nul || (
    echo ERROR: CMake was not found in PATH. Install the Visual Studio CMake component.
    exit /b 1
)

where cl >nul 2>nul
if not errorlevel 1 goto :compiler_ready

set "NP_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%NP_VSWHERE%" (
    echo ERROR: MSVC was not found. Install Visual Studio 2022 Desktop development with C++.
    exit /b 1
)
set "NP_VSINSTALL="
for /f "usebackq tokens=*" %%I in (`"%NP_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "NP_VSINSTALL=%%I"
if not defined NP_VSINSTALL (
    echo ERROR: Visual Studio C++ tools were not found.
    exit /b 1
)
call "%NP_VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=%NP_ARCH% -host_arch=x64 >nul
if errorlevel 1 (
    echo ERROR: Visual Studio developer environment initialization failed.
    exit /b 1
)

:compiler_ready
where py >nul 2>nul
if not errorlevel 1 (
    set "NP_PY=py -3"
) else (
    where python >nul 2>nul || (
        echo ERROR: Python 3 was not found in PATH.
        exit /b 1
    )
    set "NP_PY=python"
)
echo       Tools ready.
exit /b 0

:prepare_sdk
echo [2/6] Preparing ReShade 6.8.0 SDK...
if not exist "%NP_ROOT%\external" mkdir "%NP_ROOT%\external"
if not exist "%NP_ROOT%\external\reshade\include\reshade.hpp" (
    git clone --branch v6.8.0 --depth 1 ^
      https://github.com/crosire/reshade.git "%NP_ROOT%\external\reshade"
    if errorlevel 1 exit /b 1
)
if not exist "%NP_ROOT%\external\reshade\deps\imgui\imgui.h" (
    git -C "%NP_ROOT%\external\reshade" submodule update --init deps/imgui
    if errorlevel 1 exit /b 1
)
if not exist "%NP_ROOT%\external\reshade\deps\imgui\imgui.h" (
    echo ERROR: ReShade's ImGui submodule is missing.
    exit /b 1
)
if not exist "%NP_ROOT%\external\vulkan-headers\include\vulkan\vulkan.h" (
    echo       Downloading Vulkan-Headers vulkan-sdk-1.4.350.0...
    git clone --branch vulkan-sdk-1.4.350.0 --depth 1 ^
      https://github.com/KhronosGroup/Vulkan-Headers.git "%NP_ROOT%\external\vulkan-headers"
    if errorlevel 1 exit /b 1
)
exit /b 0

:prepare_directml
echo [3/6] Preparing ONNX Runtime DirectML...
set "NP_ORT_VERSION=1.24.4"
set "NP_DML_VERSION=1.15.4"
set "NP_ORT_ZIP=%NP_ROOT%\external\onnxruntime-directml-%NP_ORT_VERSION%.zip"
set "NP_DML_ZIP=%NP_ROOT%\external\directml-%NP_DML_VERSION%.zip"
set "NP_ORT_PACKAGE=%NP_ROOT%\external\ort-package-%NP_ORT_VERSION%"
set "NP_DML_PACKAGE=%NP_ROOT%\external\dml-package-%NP_DML_VERSION%"
set "NP_ORT_ROOT=%NP_ROOT%\external\onnxruntime"

if not exist "%NP_ORT_PACKAGE%\build\native\include\onnxruntime_cxx_api.h" (
    echo       Downloading ONNX Runtime %NP_ORT_VERSION%...
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
      "$ProgressPreference='SilentlyContinue'; Invoke-WebRequest 'https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/%NP_ORT_VERSION%' -OutFile '%NP_ORT_ZIP%'; Expand-Archive -LiteralPath '%NP_ORT_ZIP%' -DestinationPath '%NP_ORT_PACKAGE%' -Force"
    if errorlevel 1 exit /b 1
)

if not exist "%NP_DML_PACKAGE%\bin\x64-win\DirectML.dll" (
    echo       Downloading DirectML %NP_DML_VERSION%...
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
      "$ProgressPreference='SilentlyContinue'; Invoke-WebRequest 'https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/%NP_DML_VERSION%' -OutFile '%NP_DML_ZIP%'; Expand-Archive -LiteralPath '%NP_DML_ZIP%' -DestinationPath '%NP_DML_PACKAGE%' -Force"
    if errorlevel 1 exit /b 1
)

if not exist "%NP_ORT_ROOT%\include" mkdir "%NP_ORT_ROOT%\include"
if not exist "%NP_ORT_ROOT%\lib" mkdir "%NP_ORT_ROOT%\lib"
xcopy /E /I /Y "%NP_ORT_PACKAGE%\build\native\include\*" "%NP_ORT_ROOT%\include\" >nul
copy /Y "%NP_ORT_PACKAGE%\runtimes\win-x64\native\onnxruntime.lib" "%NP_ORT_ROOT%\lib\" >nul
copy /Y "%NP_ORT_PACKAGE%\runtimes\win-x64\native\onnxruntime.dll" "%NP_ORT_ROOT%\lib\" >nul
copy /Y "%NP_ORT_PACKAGE%\runtimes\win-x64\native\onnxruntime_providers_shared.dll" "%NP_ORT_ROOT%\lib\" >nul
copy /Y "%NP_DML_PACKAGE%\bin\x64-win\DirectML.dll" "%NP_ORT_ROOT%\lib\" >nul

if not exist "%NP_ORT_ROOT%\include\dml_provider_factory.h" (
    echo ERROR: DirectML provider headers were not prepared correctly.
    exit /b 1
)
if not exist "%NP_ORT_ROOT%\lib\onnxruntime.lib" (
    echo ERROR: onnxruntime.lib was not prepared correctly.
    exit /b 1
)
exit /b 0

:prepare_models
if /I "%NP_MODELS%"=="all" (
    %NP_PY% "%NP_ROOT%\tools\fetch_models.py" candy mosaic rain-princess udnie
) else (
    %NP_PY% "%NP_ROOT%\tools\fetch_models.py" candy
)
if errorlevel 1 exit /b 1
exit /b 0

:failed
echo.
echo ============================================================
echo  BUILD FAILED
echo ============================================================
echo Review the first error above. The build directory was preserved.
exit /b 1
