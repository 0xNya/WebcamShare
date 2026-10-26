cmake_minimum_required(VERSION 3.20)
project(WebcamShare VERSION 1.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the filter DLL lives inside client processes, the app ships as one exe.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/W4> $<$<COMPILE_LANGUAGE:CXX>:/permissive->
                    $<$<COMPILE_LANGUAGE:CXX>:/utf-8> $<$<COMPILE_LANGUAGE:CXX>:/Zi>)
add_link_options(/DEBUG /OPT:REF /OPT:ICF)

add_library(wcs_common STATIC
    src/common/shared_frame.cpp
    src/common/convert.cpp
    src/common/config.cpp
    src/common/strings.cpp)
target_include_directories(wcs_common PUBLIC src)
target_link_libraries(wcs_common PUBLIC gdi32 user32 advapi32)

# Virtual camera DirectShow filter (built for x64 and Win32).
add_library(WebcamShareCam SHARED
    src/vcam/dll.cpp
    src/vcam/filter.cpp
    src/vcam/vcam.def)
target_link_libraries(WebcamShareCam PRIVATE wcs_common strmiids ole32 oleaut32 uuid advapi32)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    return()
endif()

# --- x64 only: the app (with both filter DLLs embedded) and test tools -------

set(WCS_X86_DLL "${CMAKE_SOURCE_DIR}/build/Win32/Release/WebcamShareCam.dll" CACHE FILEPATH
    "32-bit filter DLL to embed (build the Win32 configuration first)")
set(WCS_EMBED_DIR "${CMAKE_BINARY_DIR}/embed")
set(WCS_APP_DIR "${CMAKE_SOURCE_DIR}/src/app")
string(REPLACE "." "," WCS_VERSION_COMMAS "${PROJECT_VERSION}.0")
configure_file(src/app/app.rc.in "${CMAKE_BINARY_DIR}/app.rc" @ONLY)

add_custom_command(TARGET WebcamShareCam POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${WCS_EMBED_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WebcamShareCam>"
            "${WCS_EMBED_DIR}/WebcamShareCam_x64.dll")
add_custom_command(OUTPUT "${WCS_EMBED_DIR}/WebcamShareCam_x86.dll"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${WCS_EMBED_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WCS_X86_DLL}"
            "${WCS_EMBED_DIR}/WebcamShareCam_x86.dll"
    DEPENDS "${WCS_X86_DLL}")
add_custom_target(embed_x86 DEPENDS "${WCS_EMBED_DIR}/WebcamShareCam_x86.dll")

add_executable(WebcamShare WIN32
    src/app/main.cpp
    src/app/capture.cpp
    src/app/log.cpp
    src/app/settings.cpp
    src/app/system.cpp
    src/app/tray.cpp
    src/app/app.manifest
    "${CMAKE_BINARY_DIR}/app.rc")
set_source_files_properties("${CMAKE_BINARY_DIR}/app.rc" PROPERTIES OBJECT_DEPENDS
    "${WCS_EMBED_DIR}/WebcamShareCam_x64.dll;${WCS_EMBED_DIR}/WebcamShareCam_x86.dll")
add_dependencies(WebcamShare WebcamShareCam embed_x86)
target_link_libraries(WebcamShare PRIVATE wcs_common mfplat mf mfreadwrite mfuuid ole32 shell32
                      advapi32 user32 gdi32 cfgmgr32)

add_executable(patternfeed tools/patternfeed/patternfeed.cpp)
target_link_libraries(patternfeed PRIVATE wcs_common)

add_executable(camsnap tools/camsnap/camsnap.cpp)

add_executable(vcamprobe tools/vcamprobe/vcamprobe.cpp)
target_link_libraries(vcamprobe PRIVATE strmiids ole32 oleaut32)

add_executable(wcs_tests tests/tests.cpp src/app/settings.cpp src/app/log.cpp src/app/capture.cpp)
target_link_libraries(wcs_tests PRIVATE wcs_common mfplat mf mfreadwrite mfuuid ole32 shell32 cfgmgr32)
