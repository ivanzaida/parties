add_library(parties_plugin_sdk INTERFACE)

target_include_directories(parties_plugin_sdk
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
