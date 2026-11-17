# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-espressif)

idf_build_set_property(CXX_COMPILE_OPTIONS "-Werror" APPEND)
# Keep ESP-IDF's flash filename and provide a versioned copy for OTA uploads.
idf_build_get_property(firmware_version PROJECT_VER)
set(ota_image "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-${firmware_version}.bin")
add_custom_command(OUTPUT "${ota_image}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.bin" "${ota_image}"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/.bin_timestamp"
    VERBATIM)
add_custom_target(ota_image ALL DEPENDS "${ota_image}")
add_dependencies(ota_image gen_project_binary)
