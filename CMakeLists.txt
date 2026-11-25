cmake_minimum_required(VERSION 3.19)
# the C runtime is linked in: the executables need no Visual C++ redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
project(cdi_recomp C)

# version (VERSION file): compiled in, and in the executables' file properties
file(STRINGS ${CMAKE_SOURCE_DIR}/VERSION APP_VERSION LIMIT_COUNT 1)
string(REPLACE "." "," APP_VERSION_COMMAS "${APP_VERSION}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/VERSION)

set(CMAKE_C_STANDARD 11)

# executable name comes from game.json ("exe_name")
file(READ ${CMAKE_SOURCE_DIR}/game.json GAME_JSON)
string(JSON GAME_EXE ERROR_VARIABLE json_err GET ${GAME_JSON} exe_name)
if(json_err)
  set(GAME_EXE cdigame)
endif()

# If gen/ has not been derived yet, generate zero-derived-code stub tables so
# the project always configures and compiles without any game code in the repo.
find_package(Python3 COMPONENTS Interpreter QUIET)
set(PY_CMD python3)
if(Python3_Interpreter_FOUND)
  set(PY_CMD ${Python3_EXECUTABLE})
endif()

if(NOT EXISTS ${CMAKE_SOURCE_DIR}/gen/recomp_table.c)
  execute_process(
    COMMAND ${PY_CMD} ${CMAKE_SOURCE_DIR}/tools/gen.py --stub --out ${CMAKE_SOURCE_DIR}/gen
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  )
endif()

# Embed recompiler & CHD tools directly into the binary so the final .exe or
# .AppImage is 100% self-contained and never needs external tool scripts.
execute_process(
  COMMAND ${PY_CMD} ${CMAKE_SOURCE_DIR}/tools/embed_tools.py ${CMAKE_BINARY_DIR}/embedded_tools.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

file(GLOB GEN_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/gen/*.c)
file(GLOB RT_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/runtime/*.c)

list(FILTER RT_SOURCES EXCLUDE REGEX "edition_[a-z]+\\.c$")
if(WIN32)
  list(FILTER RT_SOURCES EXCLUDE REGEX "(host_linux|render_gl)\\.c$")
else()
  list(FILTER RT_SOURCES EXCLUDE REGEX "(host_win32|render_d3d11)\\.c$")
endif()

# Single self-contained executable (supports Cartoon HD, Cartoon HD+, and
# original CD-i pixels dynamically in one binary)
add_library(core OBJECT ${RT_SOURCES} ${GEN_SOURCES})
target_compile_definitions(core PRIVATE APP_VERSION="${APP_VERSION}")
target_include_directories(core PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/gen ${CMAKE_BINARY_DIR})

if(WIN32)
  set(VERSION_EDITION "")
  set(VERSION_FILE ${GAME_EXE}.exe)
  configure_file(runtime/version.rc.in ${CMAKE_BINARY_DIR}/version.rc)
  add_executable(game $<TARGET_OBJECTS:core> ${CMAKE_BINARY_DIR}/version.rc)
else()
  add_executable(game $<TARGET_OBJECTS:core>)
endif()
set_target_properties(game PROPERTIES OUTPUT_NAME ${GAME_EXE} ENABLE_EXPORTS ON)

foreach(t core game)
  if(MSVC)
    target_compile_options(${t} PRIVATE /O2 /W3 /wd4146 /wd4244 /wd4267 /wd4996 /wd4102 /bigobj)
    target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${t} PRIVATE -O2 -Wno-unused-label -Wno-format-truncation)
  endif()
endforeach()

if(MSVC)
  target_link_libraries(game PRIVATE winmm gdi32 user32 winhttp comdlg32 shell32)
  set_target_properties(game PROPERTIES WIN32_EXECUTABLE OFF)
else()
  target_link_libraries(game PRIVATE X11 GL m pthread dl)
  target_link_options(game PRIVATE -rdynamic)
endif()

# Differential instruction tester (recompiled semantics vs interpreter).
# Generate test/insn_tests.c with: python tools/gen_tests.py
if(EXISTS ${CMAKE_SOURCE_DIR}/test/insn_tests.c)
  add_executable(insn_test test/harness.c test/insn_tests.c runtime/interp.c runtime/alu.c runtime/disc.c)
  target_include_directories(insn_test PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/gen)
  target_compile_definitions(insn_test PRIVATE RECOMP_TEST _CRT_SECURE_NO_WARNINGS)
  if(MSVC)
    target_compile_options(insn_test PRIVATE /O1 /bigobj /wd4146 /wd4244 /wd4267 /wd4996 /wd4102)
  endif()
endif()

# ALU helper check against an independent reference model (python test/gen_alu_vectors.py)
if(EXISTS ${CMAKE_SOURCE_DIR}/test/alu_vectors.h)
  add_executable(alu_test test/alu_test.c runtime/alu.c)
  target_include_directories(alu_test PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/gen ${CMAKE_SOURCE_DIR}/test)
endif()
