# The test names get their "libs-versioning-" prefix from this directory.
kis_add_tests(
    KisVersioningJobTest.cpp
    KisVersioningFileRulesTest.cpp
    KisVersioningSettingsTest.cpp

    LINK_LIBRARIES kritaversioning kritatestsdk
)

# The fake driver is test-support code compiled into the test, not into the
# library; other tests compile the same file.
kis_add_test(
    KisVersioningSyncSessionTest.cpp
    KisVersioningFakeDriver.cpp
    TEST_NAME KisVersioningSyncSessionTest
    LINK_LIBRARIES kritaversioning kritatestsdk
)
