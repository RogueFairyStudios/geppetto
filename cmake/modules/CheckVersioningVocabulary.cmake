# SPDX-License-Identifier: GPL-2.0-or-later
#
# Fails when text shown to the artist by the versioning code uses version
# control words. Run it as a script:
#
#   cmake -DSOURCE_DIR=<source tree> -P CheckVersioningVocabulary.cmake
#
# It reads the .cpp, .h and .ui files of libs/versioning, plugins/versioning
# and plugins/dockers/projectsdocker, and the translations (msgstr) in
# po/pt_BR/geppetto.po. A word is reported, with its file and line, when it
# stands as a whole word, in any case, inside the string literals of an i18n(),
# i18nc(), i18np() or i18ncp() call, inside a <string> element of a .ui file,
# or inside a translation.

cmake_minimum_required(VERSION 3.16)

if(NOT SOURCE_DIR)
    get_filename_component(SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif()

set(FORBIDDEN_WORDS commit branch push pull checkout staging repository repositório)

# Characters that CMake lists treat specially are swapped for control characters before a file is
# split into lines, and swapped back for the report. Control characters also count as word breaks.
string(ASCII 1 BACKSLASH)
string(ASCII 2 SEMICOLON)
string(ASCII 3 OPEN_BRACKET)
string(ASCII 4 CLOSE_BRACKET)

# The tokens of a line of C++: the start of a translation call, a string literal, or a parenthesis.
set(TOKEN_REGEX "i18n(c|p|cp)?\\(|\"([^\"${BACKSLASH}]|${BACKSLASH}.)*\"|\\(|\\)")
set(CHAR_LITERAL_REGEX "'([^'${BACKSLASH}]|${BACKSLASH}.)'")
set(CALL_REGEX "i18n(c|p|cp)?\\(")

set(PROBLEMS "")

function(restore_text text out)
    string(REPLACE "${BACKSLASH}" "\\" text "${text}")
    string(REPLACE "${SEMICOLON}" ";" text "${text}")
    string(REPLACE "${OPEN_BRACKET}" "[" text "${text}")
    string(REPLACE "${CLOSE_BRACKET}" "]" text "${text}")
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

function(check_text text file line)
    # CMake lowers ASCII letters only: the accented capital of "repositório" is lowered here.
    string(TOLOWER "${text}" lower)
    string(REPLACE "Ó" "ó" lower "${lower}")
    set(found "${PROBLEMS}")
    foreach(word IN LISTS FORBIDDEN_WORDS)
        if(lower MATCHES "(^|[^a-z0-9_])${word}([^a-z0-9_]|$)")
            file(RELATIVE_PATH relative "${SOURCE_DIR}" "${file}")
            restore_text("${text}" shown)
            string(APPEND found "${relative}:${line}: \"${word}\" in ${shown}\n")
        endif()
    endforeach()
    set(PROBLEMS "${found}" PARENT_SCOPE)
endfunction()

function(read_lines file out)
    file(READ "${file}" content)
    string(REPLACE "\r" "" content "${content}")
    string(REPLACE "\\" "${BACKSLASH}" content "${content}")
    string(REPLACE ";" "${SEMICOLON}" content "${content}")
    string(REPLACE "[" "${OPEN_BRACKET}" content "${content}")
    string(REPLACE "]" "${CLOSE_BRACKET}" content "${content}")
    string(REPLACE "\n" ";" content "${content}")
    set(${out} "${content}" PARENT_SCOPE)
endfunction()

function(check_cpp_file file)
    read_lines("${file}" lines)
    set(number 0)
    set(depth 0)   # parentheses open inside a translation call; it may span several lines
    foreach(line IN LISTS lines)
        math(EXPR number "${number} + 1")
        if(depth EQUAL 0 AND NOT line MATCHES "${CALL_REGEX}")
            continue()
        endif()
        string(REGEX REPLACE "${CHAR_LITERAL_REGEX}" "''" line "${line}")
        string(REGEX MATCHALL "${TOKEN_REGEX}" tokens "${line}")
        foreach(token IN LISTS tokens)
            if(token MATCHES "^i18n")
                math(EXPR depth "${depth} + 1")
            elseif(token STREQUAL "(")
                if(depth GREATER 0)
                    math(EXPR depth "${depth} + 1")
                endif()
            elseif(token STREQUAL ")")
                if(depth GREATER 0)
                    math(EXPR depth "${depth} - 1")
                endif()
            elseif(depth GREATER 0)
                check_text("${token}" "${file}" ${number})
            endif()
        endforeach()
    endforeach()
    set(PROBLEMS "${PROBLEMS}" PARENT_SCOPE)
endfunction()

function(check_ui_file file)
    read_lines("${file}" lines)
    set(number 0)
    foreach(line IN LISTS lines)
        math(EXPR number "${number} + 1")
        string(REGEX MATCHALL "<string[^>]*>[^<]*</string>" elements "${line}")
        foreach(element IN LISTS elements)
            string(REGEX REPLACE "^<string[^>]*>([^<]*)</string>$" "\\1" text "${element}")
            check_text("${text}" "${file}" ${number})
        endforeach()
    endforeach()
    set(PROBLEMS "${PROBLEMS}" PARENT_SCOPE)
endfunction()

function(check_po_file file)
    read_lines("${file}" lines)
    set(number 0)
    set(inTranslation FALSE)   # a translation goes on in the quoted lines that follow msgstr
    foreach(line IN LISTS lines)
        math(EXPR number "${number} + 1")
        if(line MATCHES "^msgstr")
            set(inTranslation TRUE)
        elseif(NOT (inTranslation AND line MATCHES "^\""))
            set(inTranslation FALSE)
        endif()
        if(inTranslation)
            check_text("${line}" "${file}" ${number})
        endif()
    endforeach()
    set(PROBLEMS "${PROBLEMS}" PARENT_SCOPE)
endfunction()

set(sources "")
foreach(folder libs/versioning plugins/versioning plugins/dockers/projectsdocker)
    file(GLOB_RECURSE found
        "${SOURCE_DIR}/${folder}/*.cpp" "${SOURCE_DIR}/${folder}/*.h" "${SOURCE_DIR}/${folder}/*.ui")
    list(APPEND sources ${found})
endforeach()
list(SORT sources)

foreach(file IN LISTS sources)
    if(file MATCHES "\\.ui$")
        check_ui_file("${file}")
    else()
        check_cpp_file("${file}")
    endif()
endforeach()

set(translations "${SOURCE_DIR}/po/pt_BR/geppetto.po")
if(EXISTS "${translations}")
    check_po_file("${translations}")
endif()

if(NOT PROBLEMS STREQUAL "")
    # Plain messages are printed as they are, one per line, with no wrapping.
    message("${PROBLEMS}")
    message(FATAL_ERROR "Text shown to the artist uses version control words; see the lines above.")
endif()

list(LENGTH sources count)
message(STATUS "No version control word in the text of ${count} source files and the translations.")
