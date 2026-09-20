# HOW TO BUILD USING CMAKE as a standalone project:
# mkdir build
# cd build
# cmake .. -DCMAKE_TOOLCHAIN_FILE=path/to/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
# make

set(MAC_TCP_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.deps/mac-sdk" CACHE PATH
    "Directory containing locally supplied, verified MacTCP SDK headers")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools/prepare_dependencies.py"
    --component mac-sdk --check --destination "${MAC_TCP_SDK_DIR}"
    RESULT_VARIABLE SDK_CHECK_RESULT OUTPUT_VARIABLE SDK_CHECK_OUTPUT
    ERROR_VARIABLE SDK_CHECK_ERROR)
if(NOT SDK_CHECK_RESULT EQUAL 0)
    message(FATAL_ERROR "${SDK_CHECK_ERROR}\nPrepare SDK inputs as described in THIRD_PARTY.md.")
endif()
message(STATUS "${SDK_CHECK_OUTPUT}")

add_application(llm_agent
    llm_agent.c
    llm_agent.r
   )

target_include_directories(llm_agent PRIVATE vendor "${MAC_TCP_SDK_DIR}")

set_target_properties(llm_agent PROPERTIES COMPILE_OPTIONS -ffunction-sections)
if(CMAKE_SYSTEM_NAME MATCHES Retro68)
    set_target_properties(llm_agent PROPERTIES LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-strip-macsbug")
else()
    set_target_properties(llm_agent PROPERTIES LINK_FLAGS "-Wl,-gc-sections")
endif()

add_application(llm_updater
    llm_updater.c
    llm_updater.r
   )

set_target_properties(llm_updater PROPERTIES COMPILE_OPTIONS -ffunction-sections)
if(CMAKE_SYSTEM_NAME MATCHES Retro68)
    set_target_properties(llm_updater PROPERTIES LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-strip-macsbug")
else()
    set_target_properties(llm_updater PROPERTIES LINK_FLAGS "-Wl,-gc-sections")
endif()
