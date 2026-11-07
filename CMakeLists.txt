cmake_minimum_required(VERSION 3.24)
set(CCRIDE_RELEASE_VERSION "" CACHE STRING "Optional release version override")
if(CCRIDE_RELEASE_VERSION)
    project(ccrIDE VERSION ${CCRIDE_RELEASE_VERSION} LANGUAGES CXX)
else()
    project(ccrIDE VERSION 0.3.3 LANGUAGES CXX)
endif()
if(NOT WIN32 OR NOT MINGW)
    message(FATAL_ERROR "ccrIDE currently supports Windows with MinGW-w64. Select the MinGW Makefiles generator and a MinGW-w64 toolchain.")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(cmake/Dependencies.cmake)

set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

file(GLOB APP_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
set(CCRIDE_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_target(ccride_build_metadata
    COMMAND ${CMAKE_COMMAND} -DOUTPUT_DIR=${CCRIDE_GENERATED} -DCCRIDE_VERSION=${PROJECT_VERSION}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/packaging/write-build-info.cmake)

set(BUILTIN_PLUGIN_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/plugins/clock/clock.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/plugins/hello/hello.cpp)
add_executable(MiniCPP WIN32 ${APP_SOURCES} ${IMGUI_SOURCES} ${BUILTIN_PLUGIN_SOURCES})
set_target_properties(MiniCPP PROPERTIES OUTPUT_NAME "ccride-link" SUFFIX ".bin")
add_dependencies(MiniCPP ccride_build_metadata)
target_include_directories(MiniCPP PRIVATE ${CCRIDE_GENERATED})
add_custom_command(TARGET MiniCPP POST_BUILD COMMAND powershell -NoProfile -ExecutionPolicy Bypass
    -File ${CMAKE_CURRENT_SOURCE_DIR}/packaging/publish-build.ps1 -Source $<TARGET_FILE:MiniCPP>
    -OutputDir ${CMAKE_CURRENT_SOURCE_DIR}/build/releases -Version ${PROJECT_VERSION} -CompilerPath ${CMAKE_CXX_COMPILER} VERBATIM)
if(MINGW)
    target_link_options(MiniCPP PRIVATE -static-libgcc -static-libstdc++)
endif()
target_include_directories(MiniCPP PRIVATE
    ${IMGUI_DIR} ${IMGUI_DIR}/backends
    ${GLFW_DIR}/include ${NLOHMANN_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(MiniCPP PRIVATE
    ${CCRIDE_GLFW_TARGET} opengl32 gdi32 user32 shell32 wininet comdlg32 imm32 ole32 advapi32 ZLIB::ZLIB)

# Built-in plugins use unique Clock_/Hello_ entry points and share host ImGui.
set(PLUGIN_ROOT "${CMAKE_SOURCE_DIR}/build/releases/plugins" CACHE PATH "Plugin manifest directory")
foreach(p clock hello)
    add_custom_command(TARGET MiniCPP POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PLUGIN_ROOT}/${p}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/plugins/${p}/plugin.json"
            "${PLUGIN_ROOT}/${p}/plugin.json")
endforeach()

set(PLUGIN_IMGUI_SOURCES ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp)
add_library(plugin_imgui STATIC ${PLUGIN_IMGUI_SOURCES})
target_include_directories(plugin_imgui PUBLIC ${IMGUI_DIR})
add_library(completion_docs_plugin SHARED plugins/completion-docs/plugin.cpp)
add_library(ai_assistant_plugin SHARED plugins/ai-assistant/ai_assist.cpp src/utils.cpp src/execution.cpp src/service_client.cpp)
foreach(pair completion_docs_plugin ai_assistant_plugin)
    target_include_directories(${pair} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${NLOHMANN_DIR})
    target_link_libraries(${pair} PRIVATE plugin_imgui user32 gdi32 shell32 wininet comdlg32 advapi32)
    target_link_options(${pair} PRIVATE -static-libgcc -static-libstdc++)
    set_target_properties(${pair} PROPERTIES PREFIX "")
endforeach()
set_target_properties(completion_docs_plugin PROPERTIES OUTPUT_NAME "completion-docs" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/plugins/completion-docs")
set_target_properties(ai_assistant_plugin PROPERTIES OUTPUT_NAME "ai-assistant" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/plugins/ai-assistant")
foreach(id ai-assistant completion-docs)
    file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/build/plugins/${id}")
    configure_file("plugins/${id}/plugin.json" "${CMAKE_SOURCE_DIR}/build/plugins/${id}/plugin.json" COPYONLY)
endforeach()

add_executable(ccride_installer WIN32 packaging/installer.cpp)
add_dependencies(ccride_installer ccride_build_metadata)
target_include_directories(ccride_installer PRIVATE ${CCRIDE_GENERATED} ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ccride_installer PRIVATE ZLIB::ZLIB user32 gdi32 comctl32 shell32 ole32 uuid)
target_link_options(ccride_installer PRIVATE -municode -static -static-libgcc -static-libstdc++)

add_executable(ccride_ai_service plugins/ai-assistant/service_main.cpp src/utils.cpp src/execution.cpp)
target_include_directories(ccride_ai_service PRIVATE src
    ${NLOHMANN_DIR})
target_link_libraries(ccride_ai_service PRIVATE user32 shell32 wininet comdlg32 advapi32)
target_link_options(ccride_ai_service PRIVATE -static -static-libgcc -static-libstdc++)
set_target_properties(ccride_ai_service PROPERTIES OUTPUT_NAME "ai-service"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/plugins/ai-assistant")
add_dependencies(ai_assistant_plugin ccride_ai_service)

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(service_fixture tests/service_fixture.cpp)
    target_include_directories(service_fixture PRIVATE ${NLOHMANN_DIR})
    target_link_options(service_fixture PRIVATE -static-libgcc -static-libstdc++)
    add_executable(service_tests tests/service_tests.cpp src/service_client.cpp src/execution.cpp src/utils.cpp)
    target_include_directories(service_tests PRIVATE src ${NLOHMANN_DIR})
    target_link_libraries(service_tests PRIVATE user32 shell32 wininet comdlg32 advapi32)
    target_link_options(service_tests PRIVATE -municode -static-libgcc -static-libstdc++)
    add_dependencies(service_tests service_fixture)
    add_test(NAME ccrIDE_service_faults COMMAND service_tests $<TARGET_FILE:service_fixture>)
    set_tests_properties(ccrIDE_service_faults PROPERTIES TIMEOUT 20)

    add_executable(ai_plugin_tests tests/ai_plugin_tests.cpp src/utils.cpp src/execution.cpp src/service_client.cpp)
    add_custom_target(ai_service_test_assets
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ccride_ai_service> "${CMAKE_CURRENT_BINARY_DIR}/ai-service.exe"
        DEPENDS ccride_ai_service)
    add_dependencies(ai_plugin_tests ai_service_test_assets)
    target_include_directories(ai_plugin_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${NLOHMANN_DIR})
    target_link_libraries(ai_plugin_tests PRIVATE plugin_imgui user32 gdi32 shell32 wininet comdlg32 advapi32)
    target_link_options(ai_plugin_tests PRIVATE -municode -static-libgcc -static-libstdc++)
    add_test(NAME ccrIDE_ai_plugin COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ai_plugin_tests.py" $<TARGET_FILE:ai_plugin_tests>)
    set_tests_properties(ccrIDE_ai_plugin PROPERTIES TIMEOUT 35)

    add_executable(zip_extract_test tests/zip_extract_test.cpp)
    target_include_directories(zip_extract_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(zip_extract_test PRIVATE ZLIB::ZLIB)
    target_link_options(zip_extract_test PRIVATE -municode -static-libgcc -static-libstdc++)
    add_test(NAME ccrIDE_zip COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/zip_archive_tests.py" $<TARGET_FILE:zip_extract_test>)
    add_executable(debugger_tests tests/debugger_tests.cpp src/debugger.cpp src/execution.cpp src/utils.cpp)
    target_include_directories(debugger_tests PRIVATE ${NLOHMANN_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(debugger_tests PRIVATE user32 shell32 wininet comdlg32 advapi32)
    target_link_options(debugger_tests PRIVATE -static-libgcc -static-libstdc++)
    add_test(NAME ccrIDE_debugger COMMAND debugger_tests)
    set_tests_properties(ccrIDE_debugger PROPERTIES TIMEOUT 65 SKIP_RETURN_CODE 77)
    add_executable(execution_fixture tests/execution_fixture.cpp)
    add_dependencies(debugger_tests execution_fixture)
    target_link_libraries(execution_fixture PRIVATE shell32)
    target_link_options(execution_fixture PRIVATE -static-libgcc -static-libstdc++)
    add_executable(execution_tests tests/execution_tests.cpp src/execution.cpp src/utils.cpp)
    target_compile_definitions(execution_tests PRIVATE MINICPP_EXECUTION_TESTING)
    target_include_directories(execution_tests PRIVATE ${NLOHMANN_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(execution_tests PRIVATE user32 shell32 wininet comdlg32 advapi32)
    target_link_options(execution_tests PRIVATE -static-libgcc -static-libstdc++)
    add_dependencies(execution_tests execution_fixture)
    add_test(NAME MiniCPP_execution COMMAND execution_tests)
    set_tests_properties(MiniCPP_execution PROPERTIES TIMEOUT 45)
    set(LOGIC_SOURCES ${APP_SOURCES})
    list(REMOVE_ITEM LOGIC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
    add_executable(run_tests tests/run_tests.cpp ${LOGIC_SOURCES} ${BUILTIN_PLUGIN_SOURCES}
        ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp)
    add_dependencies(run_tests execution_fixture completion_docs_plugin ai_assistant_plugin)
    target_compile_definitions(run_tests PRIVATE CCRIDE_TEST_PLUGIN_ROOT="${CMAKE_SOURCE_DIR}/build/plugins")
    target_include_directories(run_tests PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends
        ${NLOHMANN_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(run_tests PRIVATE user32 gdi32 shell32 wininet comdlg32 imm32 ole32 advapi32 shlwapi ZLIB::ZLIB)
    if(MINGW)
        target_link_options(run_tests PRIVATE -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME MiniCPP_regression COMMAND run_tests)
    add_test(NAME ccrIDE_plugin_market COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/plugin_market_tests.py" $<TARGET_FILE:run_tests> $<TARGET_FILE:completion_docs_plugin>)
    set_tests_properties(ccrIDE_plugin_market PROPERTIES TIMEOUT 35)
    set_tests_properties(MiniCPP_regression PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

    # Optional real OpenGL workspace preview; not part of routine/headless tests.
    add_executable(ui_preview EXCLUDE_FROM_ALL tests/ui_preview.cpp ${LOGIC_SOURCES}
        ${BUILTIN_PLUGIN_SOURCES} ${IMGUI_SOURCES})
    add_dependencies(ui_preview ccride_build_metadata completion_docs_plugin ai_assistant_plugin)
    target_compile_definitions(ui_preview PRIVATE CCRIDE_TEST_PLUGIN_ROOT="${CMAKE_SOURCE_DIR}/build/plugins")
    target_include_directories(ui_preview PRIVATE ${CCRIDE_GENERATED})
    target_include_directories(ui_preview PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends
        ${GLFW_DIR}/include ${NLOHMANN_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src)

    target_link_libraries(ui_preview PRIVATE ${CCRIDE_GLFW_TARGET} opengl32 gdi32 user32 shell32 wininet comdlg32 imm32 ole32 advapi32 ZLIB::ZLIB)
    if(MINGW)
        target_link_options(ui_preview PRIVATE -static-libgcc -static-libstdc++)
    endif()
endif()
