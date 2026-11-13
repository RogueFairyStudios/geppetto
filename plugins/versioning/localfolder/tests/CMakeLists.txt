# The test names get their "plugins-versioning-localfolder-" prefix from this directory.
# The conformance header is listed among the sources so that moc runs on it for this target.
set(CONFORMANCE_DIR ${CMAKE_SOURCE_DIR}/libs/versioning/tests)

kis_add_test(
    LocalFolderDriverTest.cpp
    ../LocalFolderDriver.cpp
    ${CONFORMANCE_DIR}/KisVersioningDriverConformance.h
    TEST_NAME LocalFolderDriverTest
    LINK_LIBRARIES kritaversioning KF${KF_MAJOR}::CoreAddons Qt${QT_MAJOR_VERSION}::Concurrent kritatestsdk
)
target_include_directories(LocalFolderDriverTest PRIVATE ${CONFORMANCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)

# The discovery test loads the plugin built by the plugin target.
add_dependencies(LocalFolderDriverTest kritaversioninglocalfolder)
target_compile_definitions(LocalFolderDriverTest PRIVATE
    LOCALFOLDER_PLUGIN_FILE="$<TARGET_FILE:kritaversioninglocalfolder>")
