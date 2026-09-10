# HOW TO BUILD USING CMAKE as a standalone project:
# mkdir build
# cd build
# cmake .. -DCMAKE_TOOLCHAIN_FILE=path/to/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
# make

add_application(llm_agent
    llm_agent.c
    llm_agent.r
   )

target_include_directories(llm_agent PRIVATE vendor)

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
