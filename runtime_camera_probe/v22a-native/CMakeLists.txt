cmake_minimum_required(VERSION 3.20)
project(BigscreenV22A LANGUAGES CXX)

add_library(BigscreenThirdPersonGpuExport SHARED ../third_person_gpu_export.cpp)
target_include_directories(BigscreenThirdPersonGpuExport PRIVATE ../../src)
target_compile_features(BigscreenThirdPersonGpuExport PRIVATE cxx_std_17)
target_compile_definitions(BigscreenThirdPersonGpuExport PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _AMD64_ _WIN64)
target_link_libraries(BigscreenThirdPersonGpuExport PRIVATE d3d11 dxgi)
