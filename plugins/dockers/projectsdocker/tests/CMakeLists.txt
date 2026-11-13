# The test names get their "plugins-dockers-projectsdocker-" prefix from this directory.
# The fake driver is test-support code from the library's tests, compiled into this test.
set(VERSIONING_TESTS_DIR ${CMAKE_SOURCE_DIR}/libs/versioning/tests)

kis_add_test(
    ProjectsModelTest.cpp
    ../ProjectsModel.cpp
    ../ProjectsSyncRunner.cpp
    ${VERSIONING_TESTS_DIR}/KisVersioningFakeDriver.cpp
    TEST_NAME ProjectsModelTest
    LINK_LIBRARIES kritaversioning kritaui kritatestsdk
)
target_include_directories(ProjectsModelTest PRIVATE ${VERSIONING_TESTS_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)

# The translation test reads the catalogue that the pofiles target builds from po/; the
# registration test loads the plugin built by the plugin target.
add_dependencies(ProjectsModelTest pofiles kritaprojectsdocker)
target_compile_definitions(ProjectsModelTest PRIVATE
    GEPPETTO_LOCALE_DIR="${CMAKE_BINARY_DIR}/locale"
    PROJECTSDOCKER_PLUGIN_FILE="$<TARGET_FILE:kritaprojectsdocker>")

# Works with real documents of the application, so it starts the application's resources.
kis_add_test(
    ProjectsDocumentHostTest.cpp
    ../ProjectsDocumentHost.cpp
    TEST_NAME ProjectsDocumentHostTest
    LINK_LIBRARIES kritaversioning kritaui kritatestsdk
)
target_include_directories(ProjectsDocumentHostTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

# The logic runs without a window, with the fake driver.
kis_add_test(
    ProjectsPresenceTest.cpp
    ../ProjectsPresence.cpp
    ../ProjectsDocumentHost.cpp
    ${VERSIONING_TESTS_DIR}/KisVersioningFakeDriver.cpp
    TEST_NAME ProjectsPresenceTest
    LINK_LIBRARIES kritaversioning kritaui kritatestsdk
)
target_include_directories(ProjectsPresenceTest PRIVATE ${VERSIONING_TESTS_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
