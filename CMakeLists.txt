cmake_minimum_required(VERSION 3.14)
project(Tau VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Compiler flags ───────────────────────────────────────────────────────────
add_compile_options(-O2 -Wall -Wextra -Wpedantic)
if(UNIX AND NOT APPLE)
    # Linux-specific: allow SUID helpers to be built without PIE
    add_compile_options(-fstack-protector-strong)
endif()

# ─── Locate xic ───────────────────────────────────────────────────────────────
# xic is a sibling repository; build it as a sub-project so we get all
# its include paths and compiled objects automatically.
set(XI_EXCLUDE_TASK ON  CACHE BOOL "" FORCE)   # tau provides no Task usage; skip JIT
set(XI_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
set(XI_BUILD_HARDWARE OFF CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../xic
                 ${CMAKE_CURRENT_BINARY_DIR}/xic
                 EXCLUDE_FROM_ALL)

# ─── Tau library sources ──────────────────────────────────────────────────────
file(GLOB TAU_SOURCES "src/Tau/*.cpp")

add_library(TauLib OBJECT ${TAU_SOURCES})
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
    target_link_libraries(${name} PRIVATE TauLib Xi::Xi)
endfunction()

# ─── Binaries ─────────────────────────────────────────────────────────────────
tau_executable(tau       src/main.cpp)
tau_executable(tau-store src/main_store.cpp)
tau_executable(tau-instance src/main_instance.cpp)

# tgh is just tau with argv[0] = tgh; create a symlink via install
install(TARGETS tau tau-store tau-instance DESTINATION bin)

install(CODE "execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink tau
              \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/tgh)")

# tau-store needs SUID root in a real deployment; remind the installer
message(STATUS "")
message(STATUS "NOTE: 'tau-store' requires SUID root in production:")
message(STATUS "      sudo chown root:root tau-store && sudo chmod u+s tau-store")
message(STATUS "")
