remove_definitions(-DTRANSLATION_DOMAIN=\"krita\")
add_definitions(-DTRANSLATION_DOMAIN=\"geppetto\")

add_subdirectory( tests )

set(kritaversioning_LIB_SRCS
    KisVersioningJob.cpp
    KisVersioningDriver.cpp
    KisVersioningDriverRegistry.cpp
    KisVersioningFileRules.cpp
    KisVersioningConflictMarkers.cpp
)

kis_add_library(kritaversioning SHARED ${kritaversioning_LIB_SRCS})
generate_export_header(kritaversioning BASE_NAME kritaversioning)

target_link_libraries(kritaversioning
    PUBLIC
        kritaplugin
        kritaglobal
        KF${KF_MAJOR}::I18n
        KF${KF_MAJOR}::ConfigCore
        Qt${QT_MAJOR_VERSION}::Core
)

set_target_properties(kritaversioning PROPERTIES
    VERSION ${GENERIC_KRITA_LIB_VERSION} SOVERSION ${GENERIC_KRITA_LIB_SOVERSION}
)
install(TARGETS kritaversioning ${INSTALL_TARGETS_DEFAULT_ARGS})
