cmake_minimum_required(VERSION 3.21)
project(KRKWakeup VERSION 0.1.1 LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "KRK Wakeup requires Windows")
endif()

add_executable(krk-wakeup WIN32
    src/app.cpp
    src/audio.cpp
    src/audio.hpp
    app.rc
)
target_compile_features(krk-wakeup PRIVATE cxx_std_17)
target_compile_definitions(krk-wakeup PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(krk-wakeup PRIVATE ole32 uuid shell32 advapi32 dwmapi gdi32 comctl32)
if(MSVC)
    set_property(TARGET krk-wakeup PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(krk-wakeup PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_options(krk-wakeup PRIVATE "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/app.manifest")
endif()
