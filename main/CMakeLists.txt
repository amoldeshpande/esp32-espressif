idf_component_register(SRCS "main.cpp" "SomfyReceiver.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES CC1101Lib)

option(SOMFY_LEGACY_RF "Use the original RF settings for a receiver comparison" OFF)
if(SOMFY_LEGACY_RF)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SOMFY_LEGACY_RF)
endif()
