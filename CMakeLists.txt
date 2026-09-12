cmake_minimum_required(VERSION 3.14)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

option(TAU_STATIC "Build tau as a fully static binary" ON)
option(TAU_MUSL   "Fetch and use local musl toolchain in build/ for static build" ON)

if(TAU_MUSL)
    set(TAU_STATIC ON CACHE BOOL "" FORCE)
    include(FetchContent)
    message(STATUS "Fetching musl i686 toolchain into build/ ...")
    FetchContent_Declare(
        musl_toolchain
        URL https://musl.cc/i686-linux-musl-cross.tgz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(musl_toolchain)
    set(MUSL_BIN_DIR "${musl_toolchain_SOURCE_DIR}/bin")
    if(EXISTS "${MUSL_BIN_DIR}/i686-linux-musl-gcc")
        message(STATUS "Using musl-gcc: ${MUSL_BIN_DIR}/i686-linux-musl-gcc")
        set(CMAKE_C_COMPILER   "${MUSL_BIN_DIR}/i686-linux-musl-gcc" CACHE FILEPATH "" FORCE)
        set(CMAKE_CXX_COMPILER "${MUSL_BIN_DIR}/i686-linux-musl-g++" CACHE FILEPATH "" FORCE)
    endif()
endif()

project(Tau VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Compiler flags ───────────────────────────────────────────────────────────
if(UNIX AND NOT APPLE)
    add_compile_options(-fstack-protector-strong -m32 -march=i686 -mno-avx -mno-avx2 -mno-fma)
endif()

# ─── Locate xic ───────────────────────────────────────────────────────────────
# xic is a sibling repository if present; otherwise fetch via FetchContent.
set(XI_EXCLUDE_TASK ON  CACHE BOOL "" FORCE)   # tau provides no Task usage; skip JIT
set(XI_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
set(XI_BUILD_HARDWARE OFF CACHE BOOL "" FORCE)
if(TAU_STATIC OR TAU_MUSL)
    set(XI_BUILD_TLS     OFF CACHE BOOL "" FORCE)
    set(XI_BUILD_DEFLATE OFF CACHE BOOL "" FORCE)
    set(XI_BUILD_LZ4     OFF CACHE BOOL "" FORCE)
    set(XI_BUILD_ZSTD    OFF CACHE BOOL "" FORCE)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../xic/CMakeLists.txt")
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../xic
                     ${CMAKE_CURRENT_BINARY_DIR}/xic
                     EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(
        xic
        GIT_REPOSITORY https://github.com/ksee6/xic.git
        GIT_TAG        main
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(xic)
endif()

# ─── Locate meca ──────────────────────────────────────────────────────────────
# meca is a sibling repository if present; otherwise fetch via FetchContent.
get_filename_component(MECA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../meca" ABSOLUTE)

if(EXISTS "${MECA_DIR}/CMakeLists.txt")
    message(STATUS "Found local meca at ${MECA_DIR}")
    add_subdirectory(${MECA_DIR} ${CMAKE_CURRENT_BINARY_DIR}/meca EXCLUDE_FROM_ALL)
    set(MECA_INCLUDE_DIR "${MECA_DIR}/include")
else()
    include(FetchContent)
    FetchContent_Declare(
        meca
        GIT_REPOSITORY https://github.com/ksee6/meca.git
        GIT_TAG        master
    )
    FetchContent_MakeAvailable(meca)
    set(MECA_INCLUDE_DIR "${meca_SOURCE_DIR}/include")
endif()

# ─── Tau library sources ──────────────────────────────────────────────────────
file(GLOB TAU_SOURCES "src/Tau/*.cpp")

add_library(TauLib OBJECT ${TAU_SOURCES})
target_compile_options(TauLib PRIVATE -O2 -Wall -Wextra -fno-jump-tables)
target_include_directories(TauLib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${MECA_INCLUDE_DIR}>
)
target_link_libraries(TauLib PUBLIC Xi::Xi Meca)

# Linux-only: namespaces, cgroups, mmap
if(UNIX AND NOT APPLE)
    find_library(PTHREAD_LIB pthread)
    if(PTHREAD_LIB)
        target_link_libraries(TauLib PUBLIC ${PTHREAD_LIB})
    endif()
endif()



# ─── Helper macro: build a tau binary ────────────────────────────────────────
function(tau_executable name source)
    add_executable(${name} ${source})
    target_compile_options(${name} PRIVATE -O2 -Wall -Wextra -fno-jump-tables)
    target_link_libraries(${name} PRIVATE TauLib Xi::Xi Meca)
    if(TAU_STATIC)
        target_link_options(${name} PRIVATE -static)
        set_target_properties(${name} PROPERTIES LINK_SEARCH_START_STATIC ON)
        set_target_properties(${name} PROPERTIES LINK_SEARCH_END_STATIC ON)
    endif()
endfunction()

# ─── Binaries ─────────────────────────────────────────────────────────────────
tau_executable(tau src/main.cpp)

# tgh is just tau with argv[0] = tgh; create a symlink via install
install(TARGETS tau DESTINATION bin)

install(CODE "execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink tau
              \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/tgh)")

# Inform the installer about sudoers configuration
message(STATUS "")
message(STATUS "NOTE: Use install.sh to automatically configure /etc/sudoers.d/tau for 'tau do-store'.")
message(STATUS "")
