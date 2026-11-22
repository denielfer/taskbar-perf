cmake_minimum_required(VERSION 3.24)
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
endif()
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug>:Embedded>")
project(TaskbarPerf LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(TaskbarPerf WIN32
    src/config.cpp
    src/metrics_collector.cpp
    src/d2d_layered.cpp
    src/taskbar_tracker.cpp
    src/taskbar_bar.cpp
    src/flyout_window.cpp
    src/main.cpp
    resources/app.manifest
)

target_compile_definitions(TaskbarPerf PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)

if(MSVC)
    target_compile_options(TaskbarPerf PRIVATE
        /W4
        /sdl
        /guard:cf
        /utf-8
        /permissive-
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/GL>
    )
    # Mesmo hardening do build.bat: CFG, shadow stack (CET) e DLLs importadas so de System32.
    target_link_options(TaskbarPerf PRIVATE
        /GUARD:CF
        /CETCOMPAT
        /DEPENDENTLOADFLAG:0x800
        $<$<CONFIG:Release>:/LTCG>
    )
endif()

target_link_libraries(TaskbarPerf PRIVATE
    user32
    gdi32
    dwmapi
    d2d1
    dwrite
    dxgi
    pdh
    iphlpapi
    shell32
    shcore
    advapi32
    ole32
    powrprof
    wtsapi32
)

set_target_properties(TaskbarPerf PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
)
