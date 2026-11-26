cmake_minimum_required(VERSION 3.28)
project(TulliusNovelist VERSION 0.0.9 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(TN_BUILD_ROOT "G:/skyrim-build/TulliusNovelist" CACHE PATH "External build artifact root")
set(TN_PACKAGE_DIR "${TN_BUILD_ROOT}/package" CACHE PATH "Package output directory")

# CommonLibSSE-NG (alandtse) v9 submodule: one DLL for SE 1.5.97 and AE 1.6.x.
if(NOT EXISTS "${PROJECT_SOURCE_DIR}/external/CommonLibSSE-NG/CMakeLists.txt")
    message(FATAL_ERROR "CommonLibSSE-NG is missing. Run: git submodule update --init")
endif()
set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(REX_OPTION_INI OFF CACHE BOOL "" FORCE)
set(REX_OPTION_JSON OFF CACHE BOOL "" FORCE)
set(REX_OPTION_TOML OFF CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_XBYAK OFF CACHE BOOL "" FORCE)
add_subdirectory(external/CommonLibSSE-NG EXCLUDE_FROM_ALL)
include(external/CommonLibSSE-NG/cmake/CommonLibSSE.cmake)

# Stage 0 instrumentation plugin (docs/design-draft.md 12장 0단계). Not the mod itself.
add_commonlibsse_plugin(TulliusNovelistProbe
    NAME "TulliusNovelistProbe"
    AUTHOR "serval"
    VERSION "${PROJECT_VERSION}"
    USE_ADDRESS_LIBRARY
    SOURCES
        probe/src/main.cpp
)
target_sources(TulliusNovelistProbe PRIVATE
    probe/src/Out.cpp
    probe/src/Forms.cpp
    probe/src/Events.cpp
    probe/src/DamageHook.cpp
    probe/src/Poller.cpp
    probe/src/World.cpp
    probe/src/CoSave.cpp
    probe/src/Session.cpp
    probe/src/Text.cpp
)
target_precompile_headers(TulliusNovelistProbe PRIVATE probe/src/PCH.h)
# CommonLib v9 pulls in <Windows.h>; keep its min/max macros away from std::min/std::max.
# The probe version comes only from project(VERSION ...) above.
target_compile_definitions(TulliusNovelistProbe PRIVATE NOMINMAX TN_PROBE_VERSION="${PROJECT_VERSION}")
target_include_directories(TulliusNovelistProbe PRIVATE probe/src)

# Game-independent text handling, tested on its own. package_probe runs it first and stops
# if a check fails.
add_executable(probe_text_test EXCLUDE_FROM_ALL probe/tests/TextTest.cpp probe/src/Text.cpp)
target_include_directories(probe_text_test PRIVATE probe/src)
target_compile_definitions(probe_text_test PRIVATE NOMINMAX)
target_compile_options(probe_text_test PRIVATE /utf-8 /W4 /permissive-)

set(TN_PROBE_ZIP "${TN_BUILD_ROOT}/TulliusNovelistProbe-${PROJECT_VERSION}.zip")
file(MAKE_DIRECTORY "${TN_PACKAGE_DIR}/probe")
add_custom_target(package_probe
    COMMAND ${CMAKE_COMMAND} -E rm -f "${TN_PROBE_ZIP}"
    COMMAND "$<TARGET_FILE:probe_text_test>"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${TN_PACKAGE_DIR}/probe/SKSE"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${TN_PACKAGE_DIR}/probe/SKSE/Plugins"
    COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:TulliusNovelistProbe>"
        "${TN_PACKAGE_DIR}/probe/SKSE/Plugins/TulliusNovelistProbe.dll"
    COMMAND ${CMAKE_COMMAND} -E tar cf "${TN_PROBE_ZIP}" --format=zip SKSE
    COMMAND ${CMAKE_COMMAND} -E echo "MO2 zip: ${TN_PROBE_ZIP}"
    WORKING_DIRECTORY "${TN_PACKAGE_DIR}/probe"
    DEPENDS TulliusNovelistProbe probe_text_test
)
