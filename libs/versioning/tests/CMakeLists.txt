# The test names get their "libs-versioning-" prefix from this directory.
kis_add_tests(
    KisVersioningJobTest.cpp

    LINK_LIBRARIES kritaversioning kritatestsdk
)
