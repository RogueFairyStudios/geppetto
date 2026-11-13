remove_definitions(-DTRANSLATION_DOMAIN=\"krita\")
add_definitions(-DTRANSLATION_DOMAIN=\"geppetto\")

# The driver is written against one version of the library's C API, whose structures and events
# change between versions.
set(LORE_REQUIRED_VERSION "0.14.1-nightly")
if(NOT Lore_VERSION STREQUAL LORE_REQUIRED_VERSION)
    message(FATAL_ERROR
        "The Lore versioning driver needs the Lore library ${LORE_REQUIRED_VERSION}, "
        "but ${Lore_INCLUDE_DIR}/lore.h declares \"${Lore_VERSION}\". "
        "Point Lore_ROOT at a build of that version.")
endif()

set(kritaversioninglore_DRIVER_SOURCES
    LoreCall.cpp
    LoreOperation.cpp
    LoreFolderListing.cpp
    LoreSyncOperation.cpp
    LoreDriver.cpp
)

# Copies lore.dll next to the executables of the build tree, so that the plugin and the tests
# run from it without changing PATH.
function(copy_lore_runtime target)
    if(WIN32 AND Lore_RUNTIME_LIBRARY)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${Lore_RUNTIME_LIBRARY}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()

add_subdirectory( tests )

kis_add_library(kritaversioninglore MODULE ${kritaversioninglore_DRIVER_SOURCES} LoreDriverPlugin.cpp)
target_link_libraries(kritaversioninglore
    kritaversioning
    Lore::Lore
    KF${KF_MAJOR}::CoreAddons
    Qt${QT_MAJOR_VERSION}::Gui
)
copy_lore_runtime(kritaversioninglore)
install(TARGETS kritaversioninglore DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})

# The Windows build ships the library next to the executables. The licence (MIT) asks for its
# notice to travel with the binaries, so it is installed on every platform.
if(WIN32 AND Lore_RUNTIME_LIBRARY)
    install(FILES ${Lore_RUNTIME_LIBRARY} DESTINATION ${KDE_INSTALL_BINDIR})
endif()

if(Lore_LICENSE_FILE AND EXISTS "${Lore_LICENSE_FILE}")
    install(FILES "${Lore_LICENSE_FILE}" DESTINATION ${KDE_INSTALL_DATADIR}/krita RENAME lore-LICENSE.txt)
else()
    message(STATUS "Lore_LICENSE_FILE is not set or does not exist: the Lore licence will not be installed. "
                   "Set it to the LICENSE file of the Lore sources.")
endif()
