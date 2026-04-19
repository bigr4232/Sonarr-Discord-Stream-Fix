cmake_minimum_required(VERSION 3.20)
project(MuteDiscordDevice_Config LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "MuteDiscordDevice_Config is Windows-only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(MuteDiscordDevice_Config WIN32
    main.cpp
    MuteDiscordDevice_Config.rc
)

target_compile_definitions(MuteDiscordDevice_Config PRIVATE
    UNICODE
    _UNICODE
    $<$<CONFIG:Debug>:DEBUG>
)

if(MSVC)
    target_compile_options(MuteDiscordDevice_Config PRIVATE /W3 /permissive-)
endif()
