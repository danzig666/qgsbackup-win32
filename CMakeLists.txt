cmake_minimum_required(VERSION 3.16)
project(qgsbackup VERSION 1.0.2 LANGUAGES C CXX RC)

if(NOT WIN32)
  message(FATAL_ERROR "qgsbackup is a native Windows application")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Build qgsbackup for x64")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

add_library(qgsbackup_engine STATIC
  src/backup_engine.cpp src/filesystem_utils.cpp src/qgis_project.cpp src/wildcard.cpp src/zip_utils.cpp
  third_party/tinyxml2/tinyxml2.cpp
  third_party/miniz/miniz.c third_party/miniz/miniz_tdef.c third_party/miniz/miniz_tinfl.c third_party/miniz/miniz_zip.c)
target_include_directories(qgsbackup_engine PUBLIC src third_party/tinyxml2 third_party/miniz)
target_compile_definitions(qgsbackup_engine PRIVATE UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(qgsbackup_engine PUBLIC bcrypt ole32)

add_executable(qgsbackup src/main.cpp src/cli.cpp src/gui.cpp src/qgsbackup.rc)
set_target_properties(qgsbackup PROPERTIES LINK_FLAGS "/MANIFEST:NO")
target_compile_definitions(qgsbackup PRIVATE UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(qgsbackup PRIVATE qgsbackup_engine comctl32 shell32 ole32 uuid)

if(MSVC)
  set_property(TARGET qgsbackup qgsbackup_engine PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_compile_options(qgsbackup_engine PRIVATE /W4 /permissive- /utf-8)
  target_compile_options(qgsbackup PRIVATE /W4 /permissive- /utf-8)
endif()

set_target_properties(qgsbackup PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist")
foreach(config Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${config}" config_upper)
  set_target_properties(qgsbackup PROPERTIES "RUNTIME_OUTPUT_DIRECTORY_${config_upper}" "${CMAKE_SOURCE_DIR}/dist")
endforeach()

add_custom_command(TARGET qgsbackup POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "${CMAKE_SOURCE_DIR}/qgsbackup_skip.txt" "$<TARGET_FILE_DIR:qgsbackup>/qgsbackup_skip.txt")

enable_testing()
add_executable(qgsbackup_tests tests/tests.cpp src/wildcard.cpp src/qgis_project.cpp src/filesystem_utils.cpp third_party/tinyxml2/tinyxml2.cpp)
target_include_directories(qgsbackup_tests PRIVATE src third_party/tinyxml2)
target_compile_definitions(qgsbackup_tests PRIVATE UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(qgsbackup_tests PRIVATE bcrypt ole32)
if(MSVC)
  set_property(TARGET qgsbackup_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
add_test(NAME unit COMMAND qgsbackup_tests)
add_test(NAME integration COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
  "${CMAKE_SOURCE_DIR}/tests/run_integration.ps1" -Executable "$<TARGET_FILE:qgsbackup>")
