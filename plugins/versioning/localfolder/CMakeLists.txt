remove_definitions(-DTRANSLATION_DOMAIN=\"krita\")
add_definitions(-DTRANSLATION_DOMAIN=\"geppetto\")

add_subdirectory( tests )

set(kritaversioninglocalfolder_SOURCES
    LocalFolderDriver.cpp
    LocalFolderDriverPlugin.cpp
)

kis_add_library(kritaversioninglocalfolder MODULE ${kritaversioninglocalfolder_SOURCES})
target_link_libraries(kritaversioninglocalfolder
    kritaversioning
    KF${KF_MAJOR}::CoreAddons
    Qt${QT_MAJOR_VERSION}::Concurrent
)
install(TARGETS kritaversioninglocalfolder DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})
