cmake_minimum_required(VERSION 3.20)
project(BigscreenDesktopBridge LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "This prototype currently targets Windows only.")
endif()

# Check Windows Environment variable first, fallback to your hardcoded path
if(NOT OPENVR_SDK_ROOT)
  if(DEFINED ENV{OPENVR_SDK_ROOT})
    set(OPENVR_SDK_ROOT "$ENV{OPENVR_SDK_ROOT}")
  else()
    # Replace this path with your actual OpenVR folder (use forward slashes '/')
    set(OPENVR_SDK_ROOT "D:/VS-Resources/openvr" CACHE PATH "Path to a Valve OpenVR SDK checkout" FORCE)
  endif()
endif()

if(NOT EXISTS "${OPENVR_SDK_ROOT}/headers/openvr_driver.h")
  message(FATAL_ERROR "Set OPENVR_SDK_ROOT to an OpenVR SDK checkout containing headers/openvr_driver.h - Currently looking in: ${OPENVR_SDK_ROOT}")
endif()

if(NOT FRIDA_CORE_DEVKIT_ROOT)
  if(DEFINED ENV{FRIDA_CORE_DEVKIT_ROOT})
    set(FRIDA_CORE_DEVKIT_ROOT "$ENV{FRIDA_CORE_DEVKIT_ROOT}")
  else()
    # Replace with your actual path to the folder containing frida-core.h and frida-core.lib
    set(FRIDA_CORE_DEVKIT_ROOT "D:/VS-Resources/frida-devkit" CACHE PATH "Path to an official Frida Core devkit" FORCE)
  endif()
endif()

if(NOT EXISTS "${FRIDA_CORE_DEVKIT_ROOT}/frida-core.h" OR NOT EXISTS "${FRIDA_CORE_DEVKIT_ROOT}/frida-core.lib")
  message(FATAL_ERROR "Set FRIDA_CORE_DEVKIT_ROOT to an official Frida Core devkit containing frida-core.h and frida-core.lib - Currently looking in: ${FRIDA_CORE_DEVKIT_ROOT}")
endif()

add_library(driver_bigscreen_desktop SHARED
  src/driver.cpp
  src/direct_mode.cpp
)
target_include_directories(driver_bigscreen_desktop PRIVATE "${OPENVR_SDK_ROOT}/headers")
target_compile_features(driver_bigscreen_desktop PRIVATE cxx_std_17)
target_compile_definitions(driver_bigscreen_desktop PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _AMD64_ _WIN64)
target_link_libraries(driver_bigscreen_desktop PRIVATE d3d11 dxgi)
set_target_properties(driver_bigscreen_desktop PROPERTIES
  OUTPUT_NAME "driver_bigscreen_desktop"
  PREFIX ""
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

set(DRIVER_ROOT "${CMAKE_CURRENT_BINARY_DIR}/BigscreenDesktopDriver")
add_custom_command(TARGET driver_bigscreen_desktop POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${DRIVER_ROOT}/bin/win64"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${DRIVER_ROOT}/resources/settings"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:driver_bigscreen_desktop>" "${DRIVER_ROOT}/bin/win64/driver_bigscreen_desktop.dll"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/driver.vrdrivermanifest" "${DRIVER_ROOT}/driver.vrdrivermanifest"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/resources/settings/default.vrsettings" "${DRIVER_ROOT}/resources/settings/default.vrsettings"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/runtime/driver/bin/win64"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/runtime/driver/resources/settings"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:driver_bigscreen_desktop>" "${CMAKE_BINARY_DIR}/runtime/driver/bin/win64/driver_bigscreen_desktop.dll"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/driver.vrdrivermanifest" "${CMAKE_BINARY_DIR}/runtime/driver/driver.vrdrivermanifest"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/resources/settings/default.vrsettings" "${CMAKE_BINARY_DIR}/runtime/driver/resources/settings/default.vrsettings"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:driver_bigscreen_desktop>" "${CMAKE_BINARY_DIR}/runtime/driver_bigscreen_desktop.dll"
)

add_executable(BigscreenDesktopBridge WIN32
  src/bridge_main.cpp
)
target_include_directories(BigscreenDesktopBridge PRIVATE src)
target_compile_features(BigscreenDesktopBridge PRIVATE cxx_std_17)
target_compile_definitions(BigscreenDesktopBridge PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _AMD64_ _WIN64 _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS)

if(DEFINED ENV{WindowsSdkDir} AND DEFINED ENV{WindowsSDKVersion})
  target_include_directories(BigscreenDesktopBridge PRIVATE "$ENV{WindowsSdkDir}Include/$ENV{WindowsSDKVersion}cppwinrt")
elseif(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
  target_include_directories(BigscreenDesktopBridge PRIVATE "C:/Program Files (x86)/Windows Kits/10/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt")
else()
  file(GLOB WINSDK_DIRS "C:/Program Files (x86)/Windows Kits/10/Include/10.*/cppwinrt")
  if(WINSDK_DIRS)
    list(GET WINSDK_DIRS -1 LATEST_CPPWINRT)
    target_include_directories(BigscreenDesktopBridge PRIVATE "${LATEST_CPPWINRT}")
  endif()
endif()

target_link_libraries(BigscreenDesktopBridge PRIVATE setupapi hid runtimeobject)
set_target_properties(BigscreenDesktopBridge PROPERTIES
  OUTPUT_NAME "BigscreenDesktopBridge"
  WIN32_EXECUTABLE TRUE
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

add_executable(BigscreenDesktopViewer WIN32
  src/viewer_main.cpp
)
target_include_directories(BigscreenDesktopViewer PRIVATE "${OPENVR_SDK_ROOT}/headers")
target_compile_features(BigscreenDesktopViewer PRIVATE cxx_std_17)
target_compile_definitions(BigscreenDesktopViewer PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _AMD64_ _WIN64)
target_include_directories(BigscreenDesktopViewer PRIVATE src)
target_link_libraries(BigscreenDesktopViewer PRIVATE
  "${OPENVR_SDK_ROOT}/lib/win64/openvr_api.lib"
  d3d11 dxgi d3dcompiler d2d1 dwrite
)
set_target_properties(BigscreenDesktopViewer PROPERTIES
  OUTPUT_NAME "BigscreenDesktopViewer"
  WIN32_EXECUTABLE TRUE
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
add_custom_command(TARGET BigscreenDesktopViewer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${OPENVR_SDK_ROOT}/bin/win64/openvr_api.dll"
    "$<TARGET_FILE_DIR:BigscreenDesktopViewer>/openvr_api.dll"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${OPENVR_SDK_ROOT}/bin/win64/openvr_api.dll"
    "${CMAKE_BINARY_DIR}/runtime/openvr_api.dll"
)

add_library(BigscreenThirdPersonGpuExport SHARED
  runtime_camera_probe/third_person_gpu_export.cpp
)
target_include_directories(BigscreenThirdPersonGpuExport PRIVATE src)
target_compile_features(BigscreenThirdPersonGpuExport PRIVATE cxx_std_17)
target_compile_definitions(BigscreenThirdPersonGpuExport PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _AMD64_ _WIN64)
target_link_libraries(BigscreenThirdPersonGpuExport PRIVATE d3d11 dxgi)
set_target_properties(BigscreenThirdPersonGpuExport PROPERTIES
  OUTPUT_NAME "BigscreenThirdPersonGpuExport"
  PREFIX ""
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

add_executable(BigscreenThirdPersonHost WIN32
  src/third_person_host.cpp
)
target_include_directories(BigscreenThirdPersonHost PRIVATE "${FRIDA_CORE_DEVKIT_ROOT}")
target_compile_features(BigscreenThirdPersonHost PRIVATE cxx_std_17)
target_compile_definitions(BigscreenThirdPersonHost PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _AMD64_ _WIN64 UNICODE _UNICODE)
target_link_libraries(BigscreenThirdPersonHost PRIVATE "${FRIDA_CORE_DEVKIT_ROOT}/frida-core.lib" setupapi)
set_target_properties(BigscreenThirdPersonHost PROPERTIES
  OUTPUT_NAME "BigscreenThirdPersonHost"
  WIN32_EXECUTABLE TRUE
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

foreach(runtime_target BigscreenDesktopBridge BigscreenDesktopViewer BigscreenThirdPersonGpuExport BigscreenThirdPersonHost)
  set_target_properties(${runtime_target} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/runtime"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/runtime"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/runtime"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/runtime"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime"
    ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/runtime"
    ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/runtime"
  )
endforeach()

add_custom_target(stage_bundle_assets ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/runtime"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/licenses/FRIDA-CORE-COPYING.txt" "${CMAKE_BINARY_DIR}/runtime/FRIDA-CORE-COPYING.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/licenses/FRIDA-GUM-COPYING.txt" "${CMAKE_BINARY_DIR}/runtime/FRIDA-GUM-COPYING.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/licenses/FRIDA-IL2CPP-BRIDGE-LICENSE.txt" "${CMAKE_BINARY_DIR}/runtime/FRIDA-IL2CPP-BRIDGE-LICENSE.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/licenses/FRIDA-THIRD-PARTY-NOTICES.txt" "${CMAKE_BINARY_DIR}/runtime/FRIDA-THIRD-PARTY-NOTICES.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/runtime_camera_probe/v24_gpu_only.js" "${CMAKE_BINARY_DIR}/runtime/third_person_agent.js"
)

add_executable(BigscreenFrameDump
  src/frame_dump.cpp
)
target_compile_features(BigscreenFrameDump PRIVATE cxx_std_17)
target_compile_definitions(BigscreenFrameDump PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _AMD64_ _WIN64)
target_link_libraries(BigscreenFrameDump PRIVATE windowscodecs)
set_target_properties(BigscreenFrameDump PROPERTIES
  OUTPUT_NAME "BigscreenFrameDump"
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

add_executable(BigscreenAppDiag
  phase8-state/app_diag.cpp
)
target_include_directories(BigscreenAppDiag PRIVATE "${OPENVR_SDK_ROOT}/headers")
target_compile_features(BigscreenAppDiag PRIVATE cxx_std_17)
target_link_libraries(BigscreenAppDiag PRIVATE "${OPENVR_SDK_ROOT}/lib/win64/openvr_api.lib")
set_target_properties(BigscreenAppDiag PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
