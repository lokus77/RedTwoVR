cmake_minimum_required(VERSION 3.25)
project(red_two LANGUAGES C CXX)
if(WIN32)
    enable_language(RC)
endif()
# macOS: Apple Silicon only (memory.cpp's fault handler reads AArch64's exception state)
if(APPLE AND NOT IOS)
    if(CMAKE_OSX_ARCHITECTURES)
        set(_r2_mac_arch "${CMAKE_OSX_ARCHITECTURES}")
    else()
        set(_r2_mac_arch "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    if(NOT _r2_mac_arch STREQUAL "arm64")
        message(FATAL_ERROR "macOS builds are for Apple Silicon (arm64) only, not ${_r2_mac_arch}")
    endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(R2_DEFS _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
# CMake's RelWithDebInfo for MSVC-style compilers inlines only functions marked inline (/Ob1):
# the bridge's small helpers (vertexSize, isPalette, cmdLength, ...) and Aurora's stayed calls
# on paths taken for every draw. Full inlining, as in Release.
string(REPLACE "/Ob1" "/Ob2" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
string(REPLACE "/Ob1" "/Ob2" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")

# --- Aurora (renderer, window, input): extern/aurora, MIT ---------------------------------
# The game's own SDK code runs recompiled, so only Aurora's renderer and platform layer are
# used: no DVD (we read the disc), no THP, no memory card (EXI is ours).
set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_THP OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_RMLUI OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)

# The game's own main.dol, or the user's disc image (ISO/GCM/RVZ/WIA) it is read from; never
# committed. Defaults to the first that exists of the usual places.
set(_r2_dol_default "${CMAKE_SOURCE_DIR}/orig/GSWE64/sys/main.dol")
foreach(_c orig/GSWE64/sys/main.dol orig/GSWE64.iso orig/GSWE64.rvz)
    if(EXISTS "${CMAKE_SOURCE_DIR}/${_c}")
        set(_r2_dol_default "${CMAKE_SOURCE_DIR}/${_c}")
        break()
    endif()
endforeach()
set(R2_DOL "${_r2_dol_default}" CACHE FILEPATH "main.dol or disc image of Star Wars: Rogue Leader (GSWE64)")
set(R2_GEN_FILES 96 CACHE STRING "number of generated code files")
# Generated files are large: cap how many compile at once (memory).
set(R2_GEN_JOBS 12 CACHE STRING "parallel compiles of generated code")
set_property(GLOBAL APPEND PROPERTY JOB_POOLS r2gen=${R2_GEN_JOBS})

# Android: the program is a shared library (libmain.so) SDL's Java activity loads; everything
# in it position-independent. The recompiler has to run on the build machine: R2_HOST_RECOMP
# names one built there (tools/build_android.ps1 passes build/windows/r2recomp.exe).
if(ANDROID)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()
set(R2_HOST_RECOMP "" CACHE FILEPATH "a recompiler built for the build machine (cross builds: Android)")

# --- recompiler -------------------------------------------------------------------------
add_executable(r2recomp
    src/recompiler/main.cpp
    src/recompiler/analysis.cpp
    src/recompiler/emit.cpp
    src/recompiler/ppc.cpp
    src/common/disc_image.cpp)
target_compile_definitions(r2recomp PRIVATE ${R2_DEFS})
target_link_libraries(r2recomp PRIVATE zstd::libzstd)

# disc_compare <a> <b>: check the disc image reader (not built by default)
add_executable(disc_compare EXCLUDE_FROM_ALL src/common/disc_compare.cpp src/common/disc_image.cpp)
target_link_libraries(disc_compare PRIVATE zstd::libzstd)

# --- generated code ---------------------------------------------------------------------
set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")
set(GEN_SOURCES "${GEN_DIR}/table.cpp")
math(EXPR _last "${R2_GEN_FILES} - 1")
foreach(i RANGE ${_last})
    string(LENGTH "${i}" _len)
    if(_len EQUAL 1)
        set(_n "00${i}")
    elseif(_len EQUAL 2)
        set(_n "0${i}")
    else()
        set(_n "${i}")
    endif()
    list(APPEND GEN_SOURCES "${GEN_DIR}/code_${_n}.cpp")
endforeach()
file(GLOB R2_CONFIG "${CMAKE_SOURCE_DIR}/config/GSWE64/*.txt")
if(R2_HOST_RECOMP)
    set(_r2_recomp "${R2_HOST_RECOMP}")
    set(_r2_recomp_dep "${R2_HOST_RECOMP}")
elseif(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "cross build: set R2_HOST_RECOMP to an r2recomp built for this machine")
else()
    set(_r2_recomp r2recomp)
    set(_r2_recomp_dep r2recomp)
endif()
add_custom_command(
    OUTPUT ${GEN_SOURCES} "${GEN_DIR}/funcs.h"
    COMMAND ${_r2_recomp} "${R2_DOL}" "${CMAKE_SOURCE_DIR}/config/GSWE64" "${GEN_DIR}" --files ${R2_GEN_FILES}
    DEPENDS ${_r2_recomp_dep} "${R2_DOL}" ${R2_CONFIG}
    COMMENT "Recompiling main.dol"
    VERBATIM)

add_library(r2game STATIC ${GEN_SOURCES})
set_target_properties(r2game PROPERTIES JOB_POOL_COMPILE r2gen)
target_compile_definitions(r2game PRIVATE ${R2_DEFS})
target_include_directories(r2game PUBLIC src/runtime "${GEN_DIR}")
if(MSVC)
    # clang-cl: generated code is huge and full of labels
    target_compile_options(r2game PRIVATE /O2 -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable
        -fno-strict-aliasing /GS-)
elseif(ANDROID OR APPLE)
    # Android and macOS (AArch64, position-independent): no stack protector either (as below)
    target_compile_options(r2game PRIVATE -O2 -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable
        -fno-strict-aliasing -fno-stack-protector)
else()
    # Linux (clang): as for Windows - no stack protector (save states keep host stacks, whose
    # canaries differ per process), no CET (rt_longjmp jumps without the shadow stack), not
    # position-independent (the exe sits at a fixed address)
    target_compile_options(r2game PRIVATE -O2 -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable
        -fno-strict-aliasing -fno-stack-protector -fcf-protection=none -fno-pie)
endif()
# R2_ARCH: the x86-64 level the game and runtime code are compiled for. x86-64-v3 (Haswell /
# Zen and later: AVX2, FMA, MOVBE, BMI2, LZCNT) makes the PowerPC's fused multiply-adds one
# instruction instead of a call into the C library and big-endian loads/stores one MOVBE.
# Floating-point contraction stays off: the game's separate multiplies and adds round as on
# the GameCube. Empty: the compiler's baseline (any x86-64; ARM builds: always the baseline).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(R2_ARCH "x86-64-v3" CACHE STRING "x86-64 level for the game and runtime code (empty: baseline)")
else()
    set(R2_ARCH "" CACHE STRING "x86-64 level for the game and runtime code (empty: baseline)")
endif()
if(R2_ARCH)
    if(MSVC)
        set(_r2_arch "/clang:-march=${R2_ARCH}")
    else()
        set(_r2_arch "-march=${R2_ARCH}")
    endif()
endif()
# (clang-cl takes clang's own options behind /clang:; without it the option was ignored)
if(MSVC)
    set(_r2_fp "/clang:-ffp-contract=off")
else()
    set(_r2_fp "-ffp-contract=off")
endif()
target_compile_options(r2game PRIVATE ${_r2_arch} ${_r2_fp})

# --- rcheevos (RetroAchievements client): extern/rcheevos, MIT ------------------------------
file(GLOB R2_RCHEEVOS_SOURCES
    extern/rcheevos/src/rapi/*.c
    extern/rcheevos/src/rcheevos/*.c
    extern/rcheevos/src/rhash/*.c)
add_library(rcheevos STATIC ${R2_RCHEEVOS_SOURCES}
    extern/rcheevos/src/rc_client.c
    extern/rcheevos/src/rc_compat.c
    extern/rcheevos/src/rc_util.c
    extern/rcheevos/src/rc_version.c)
target_include_directories(rcheevos PUBLIC extern/rcheevos/include)
target_compile_definitions(rcheevos PUBLIC RC_CLIENT_SUPPORTS_HASH PRIVATE _CRT_SECURE_NO_WARNINGS)

# --- runtime ----------------------------------------------------------------------------
set(R2_RUNTIME_SOURCES
    src/runtime/main.cpp
    src/runtime/disc_reader.cpp
    src/runtime/disc_language.cpp
    src/runtime/memory.cpp
    src/runtime/cpu.cpp
    src/runtime/events.cpp
    src/runtime/threads.cpp
    src/runtime/sjlj.cpp
    src/runtime/vm.cpp
    src/runtime/interp.cpp
    src/runtime/profiler.cpp
    src/runtime/audio.cpp
    src/runtime/hle_musyx.cpp
    src/common/disc_image.cpp
    src/runtime/hw/musyx_dsp.cpp
    src/runtime/hle_os.cpp
    src/runtime/hw/hw.cpp
    src/runtime/hw/gx_fifo.cpp
    src/runtime/gfx/gx_bridge.cpp
    src/runtime/gfx/tex_dump.cpp
    src/runtime/gfx/button_icons.cpp
    src/runtime/input.cpp
    src/runtime/gyro.cpp
    src/runtime/settings.cpp
    src/runtime/hle_game.cpp
    src/runtime/mods.cpp
    src/runtime/codes.cpp
    src/runtime/card_image.cpp
    src/runtime/dolphin_import.cpp
    src/runtime/gfx/menu.cpp
    src/runtime/gfx/touch.cpp
    src/runtime/gfx/xr.cpp
    src/runtime/savestate.cpp
    src/runtime/hotkeys.cpp
    src/runtime/achievements.cpp
    src/runtime/http.cpp
    src/runtime/update.cpp
    src/runtime/discord.cpp)
if(ANDROID)
    add_library(red_two SHARED ${R2_RUNTIME_SOURCES})
    set_target_properties(red_two PROPERTIES OUTPUT_NAME main)  # libmain.so, what SDLActivity loads
else()
    add_executable(red_two ${R2_RUNTIME_SOURCES})
endif()
target_include_directories(red_two PRIVATE src/runtime "${GEN_DIR}" extern/aurora/lib)
# The version (VERSION: bumped for each release) and whether this is a release build (the
# downloads: -DR2_RELEASE=ON): only those replace themselves with a newer release (update.cpp).
file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" R2_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/VERSION")
option(R2_RELEASE "a release build: installs newer releases itself" OFF)
set(_r2_update_defs "R2_VERSION=\"${R2_VERSION}\"")
if(R2_RELEASE)
    list(APPEND _r2_update_defs R2_RELEASE_BUILD=1)
endif()
set_source_files_properties(src/runtime/update.cpp PROPERTIES COMPILE_DEFINITIONS "${_r2_update_defs}")
# Discord (discord.cpp): Red Two's application on Discord's developer portal, for Rich Presence
# (empty: none), and its server's invite (the menu's Join button and a Rich Presence button)
set(R2_DISCORD_APP_ID "1574605819603984604" CACHE STRING "Discord application ID for Rich Presence (empty: no Rich Presence)")
set(R2_DISCORD_INVITE "https://discord.gg/cc65jQwbqG" CACHE STRING "Red Two's Discord invite (empty: no Join button)")
set_source_files_properties(src/runtime/discord.cpp PROPERTIES COMPILE_DEFINITIONS
                            "R2_DISCORD_APP_ID=\"${R2_DISCORD_APP_ID}\";R2_DISCORD_INVITE=\"${R2_DISCORD_INVITE}\"")
# Program icon: your own res/icon.ico (not in the repository) when it exists, else Red Two's
# own res/red_two.ico (no game artwork; what public builds use: -DR2_ICON=res/red_two.ico).
if(EXISTS "${CMAKE_SOURCE_DIR}/res/icon.ico")
    set(_r2_icon_default "${CMAKE_SOURCE_DIR}/res/icon.ico")
else()
    set(_r2_icon_default "${CMAKE_SOURCE_DIR}/res/red_two.ico")
endif()
set(R2_ICON "${_r2_icon_default}" CACHE FILEPATH "program icon (.ico, Windows)")
if(WIN32 AND R2_ICON)
    get_filename_component(_r2_icon "${R2_ICON}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    if(EXISTS "${_r2_icon}")
        configure_file(res/red_two.rc.in "${CMAKE_BINARY_DIR}/red_two.rc" @ONLY)
        target_sources(red_two PRIVATE "${CMAKE_BINARY_DIR}/red_two.rc")
        set_source_files_properties("${CMAKE_BINARY_DIR}/red_two.rc" PROPERTIES OBJECT_DEPENDS "${_r2_icon}")
        target_compile_definitions(red_two PRIVATE R2_HAS_ICON=1)
    endif()
endif()
target_compile_definitions(red_two PRIVATE ${R2_DEFS})
target_compile_options(red_two PRIVATE ${_r2_arch} ${_r2_fp})
target_link_libraries(red_two PRIVATE r2game aurora::core aurora::gx aurora::vi aurora::pad xxHash::xxhash zstd::libzstd
    rcheevos PNG::PNG nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(red_two PRIVATE winmm dbghelp winhttp ZLIB::ZLIB)
endif()
# --- OpenXR (VR on Windows: src/runtime/gfx/xr.cpp): the Khronos loader, linked in, Apache-2.0 --
option(R2_OPENXR "OpenXR support (the game on a screen in a VR headset; --xr)" ${WIN32})
if(R2_OPENXR AND WIN32)
    include(FetchContent)
    set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
    set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(openxr
        URL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.65.tar.gz
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(openxr)
    target_link_libraries(red_two PRIVATE openxr_loader d3d12 dxgi)
    target_compile_definitions(red_two PRIVATE R2_OPENXR=1)
endif()
if(WIN32)
elseif(ANDROID)
    # Android: position-independent (save states then load only within a session: the system
    # puts the library anywhere), no stack protector (as the generated code), segments aligned
    # for devices with 16 KiB pages. The C++ runtime is linked in (ANDROID_STL=c++_static).
    target_compile_options(red_two PRIVATE -fno-strict-aliasing -fno-stack-protector)
    target_link_options(red_two PRIVATE -Wl,-z,max-page-size=16384 -Wl,--no-undefined)
    target_link_libraries(red_two PRIVATE android log dl)
elseif(APPLE)
    # macOS: position-independent (the arm64 loader requires it; save states then load only
    # within a session, as on Android), no stack protector (as the generated code), a 16 MiB
    # main-thread stack as on Windows. Built as Red Two.app (tools/build_macos.sh).
    target_compile_options(red_two PRIVATE -fno-strict-aliasing -fno-stack-protector)
    target_link_options(red_two PRIVATE -Wl,-stack_size,0x1000000)
    set_target_properties(red_two PROPERTIES
        MACOSX_BUNDLE TRUE
        OUTPUT_NAME "Red Two"
        MACOSX_BUNDLE_BUNDLE_NAME "Red Two"
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.gminustwo.redtwo"
        MACOSX_BUNDLE_BUNDLE_VERSION "${R2_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${R2_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "red_two.icns"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/res/Info.plist.in")
    if(EXISTS "${CMAKE_SOURCE_DIR}/res/red_two.icns")
        target_sources(red_two PRIVATE res/red_two.icns)
        set_source_files_properties(res/red_two.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    endif()
else()
    # Not a position-independent executable (save states hold return addresses into it, as
    # /DYNAMICBASE:NO on Windows); names in the dynamic symbol table for crash call stacks; the
    # C++ runtime linked in (the Steam Deck's own libstdc++ may be older than the build's);
    # libraries found next to it (an AppImage's usr/lib, or the build tree's).
    target_compile_options(red_two PRIVATE -fno-strict-aliasing -fno-stack-protector -fcf-protection=none -fno-pie)
    target_link_options(red_two PRIVATE -no-pie -rdynamic -static-libstdc++ -static-libgcc)
    set_target_properties(red_two PROPERTIES INSTALL_RPATH "$ORIGIN/../lib" BUILD_RPATH_USE_ORIGIN ON)
    target_link_libraries(red_two PRIVATE dl pthread)
endif()
if(MSVC)
    # /GS-: save states keep the guest threads' host stacks, also into a later session, whose
    # stack cookies differ (the generated code is built without them too)
    target_compile_options(red_two PRIVATE -fno-strict-aliasing /GS-)
    # GUI subsystem (no console window), still entering through main(). No ASLR: those stacks
    # hold return addresses into the exe (savestate.h).
    target_link_options(red_two PRIVATE /STACK:16777216 /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /DYNAMICBASE:NO)
    # UTF-8 as the process code page (Windows 10 1903+): fopen and the A functions take the
    # UTF-8 paths SDL gives (the program's folder, which may be anywhere: portable user folder)
    target_sources(red_two PRIVATE res/red_two.manifest)
    # A CPU without R2_ARCH's instructions gets a message box instead of a silent crash: the check
    # is compiled for the baseline and runs before anything else (cpu_check.cpp)
    if(R2_ARCH AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        target_sources(red_two PRIVATE src/runtime/cpu_check.cpp)
        set_source_files_properties(src/runtime/cpu_check.cpp PROPERTIES
            COMPILE_OPTIONS "/clang:-march=x86-64" COMPILE_DEFINITIONS R2_CPU_CHECK=1)
        target_link_options(red_two PRIVATE /INCLUDE:r2CpuCheck)
    endif()
endif()

# Dawn and SDL3 are DLLs on Windows: copy them next to the executable.
if(WIN32)
    include(extern/aurora/cmake/AuroraCopyRuntimeDLLs.cmake)
    aurora_copy_runtime_dlls(red_two)
endif()
