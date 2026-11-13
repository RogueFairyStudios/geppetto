# The test names get their "plugins-versioning-lore-" prefix from this directory.

# Only the header is needed: nothing of the library is called.
kis_add_test(
    LoreStringsTest.cpp
    TEST_NAME LoreStringsTest
    LINK_LIBRARIES kritaglobal Qt${QT_MAJOR_VERSION}::Widgets kritatestsdk
)
target_include_directories(LoreStringsTest PRIVATE ${Lore_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/..)

# The conformance header is listed among the sources so that moc runs on it for this target.
set(CONFORMANCE_DIR ${CMAKE_SOURCE_DIR}/libs/versioning/tests)
set(LORE_DRIVER_SOURCES ${kritaversioninglore_DRIVER_SOURCES})
list(TRANSFORM LORE_DRIVER_SOURCES PREPEND ../)

kis_add_test(
    LoreDriverTest.cpp
    ${LORE_DRIVER_SOURCES}
    ${CONFORMANCE_DIR}/KisVersioningDriverConformance.h
    TEST_NAME LoreDriverTest
    LINK_LIBRARIES kritaversioning Lore::Lore KF${KF_MAJOR}::CoreAddons Qt${QT_MAJOR_VERSION}::Gui kritatestsdk
)
target_include_directories(LoreDriverTest PRIVATE ${CONFORMANCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
copy_lore_runtime(LoreDriverTest)

# The discovery test loads the plugin built by the plugin target.
add_dependencies(LoreDriverTest kritaversioninglore)
target_compile_definitions(LoreDriverTest PRIVATE
    LORE_PLUGIN_FILE="$<TARGET_FILE:kritaversioninglore>")

# Without a test server every test skips itself with the reason; ctest then reports a skip. On
# Windows Qt sends the test log to the debugger when no console is attached, as under ctest, so
# the log is forced to the output ctest reads.
set_tests_properties(plugins-versioning-lore-LoreDriverTest PROPERTIES
    SKIP_REGULAR_EXPRESSION "KRITA_LORE_TEST_SERVER is not set"
    ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1")
