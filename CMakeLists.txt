cmake_minimum_required(VERSION 3.14)
project(Tau VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Compiler flags ───────────────────────────────────────────────────────────
if(UNIX AND NOT APPLE)
    add_compile_options(-fstack-protector-strong)
endif()

# ─── Locate xic ───────────────────────────────────────────────────────────────
# xic is a sibling repository if present; otherwise fetch via FetchContent.
set(XI_EXCLUDE_TASK ON  CACHE BOOL "" FORCE)   # tau provides no Task usage; skip JIT
set(XI_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
set(XI_BUILD_HARDWARE OFF CACHE BOOL "" FORCE)

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

# ─── Tau library sources ──────────────────────────────────────────────────────
file(GLOB TAU_SOURCES "src/Tau/*.cpp")

add_library(TauLib OBJECT ${TAU_SOURCES})
target_compile_options(TauLib PRIVATE -O2 -Wall -Wextra)
target_include_directories(TauLib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(TauLib PUBLIC Xi::Xi)

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
    target_compile_options(${name} PRIVATE -O2 -Wall -Wextra)
    target_link_libraries(${name} PRIVATE TauLib Xi::Xi)
endfunction()

# ─── Binaries ─────────────────────────────────────────────────────────────────
tau_executable(tau       src/main.cpp)
tau_executable(tau-store src/main_store.cpp)
tau_executable(tau-instance src/main_instance.cpp)

add_library(nosetgroups SHARED src/nosetgroups.c)
set_target_properties(nosetgroups PROPERTIES OUTPUT_NAME "nosetgroups")

# tgh is just tau with argv[0] = tgh; create a symlink via install
install(TARGETS tau tau-store tau-instance nosetgroups DESTINATION bin)

install(CODE "execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink tau
              \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/tgh)")

# Inform the installer about sudoers configuration
message(STATUS "")
message(STATUS "NOTE: Use install.sh to automatically configure /etc/sudoers.d/tau for 'tau-store'.")
message(STATUS "")
