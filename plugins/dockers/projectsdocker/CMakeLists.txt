remove_definitions(-DTRANSLATION_DOMAIN=\"krita\")
add_definitions(-DTRANSLATION_DOMAIN=\"geppetto\")

add_subdirectory( tests )

set(kritaprojectsdocker_SOURCES
    ProjectsDockerPlugin.cpp
    ProjectsDock.cpp
    ProjectsModel.cpp
    ProjectsDocumentHost.cpp
    ProjectsSyncRunner.cpp
)

kis_add_library(kritaprojectsdocker MODULE ${kritaprojectsdocker_SOURCES})
target_link_libraries(kritaprojectsdocker kritaui kritaversioning)
install(TARGETS kritaprojectsdocker DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})

# No text shown to the artist may use version control words.
set(VOCABULARY_SCRIPT ${CMAKE_SOURCE_DIR}/cmake/modules/CheckVersioningVocabulary.cmake)
add_test(NAME versioning-vocabulary
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${VOCABULARY_SCRIPT})

# Proof that the check catches a forbidden word: it must report the planted one, by file and line.
set(VOCABULARY_SAMPLE ${CMAKE_CURRENT_BINARY_DIR}/vocabulary-sample)
file(WRITE ${VOCABULARY_SAMPLE}/libs/versioning/Planted.cpp
    "QString text = i18n(\"Send\",\n    \"Pull the latest work\");\n")
add_test(NAME versioning-vocabulary-detects
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${VOCABULARY_SAMPLE} -P ${VOCABULARY_SCRIPT})
set_tests_properties(versioning-vocabulary-detects PROPERTIES
    PASS_REGULAR_EXPRESSION "libs/versioning/Planted.cpp:2: .*pull")
