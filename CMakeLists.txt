cmake_minimum_required(VERSION 3.15)
project(DiscordMuter LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT linkage — no vcruntime DLLs needed on target machines
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(DiscordMuter WIN32 main.cpp)

target_compile_definitions(DiscordMuter PRIVATE UNICODE _UNICODE)

target_link_libraries(DiscordMuter PRIVATE
    comctl32
    shell32
    ole32
    user32
    gdi32
    advapi32
)
