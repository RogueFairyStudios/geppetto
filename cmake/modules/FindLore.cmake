# SPDX-License-Identifier: BSD-3-Clause

#[=======================================================================[.rst:
FindLore
--------

Find the client library of the Lore version control system (its C API,
``lore.h``).

Two layouts are searched: an installed prefix (``include/lore.h``, ``lib/``,
``bin/``) and a built source checkout of Lore, when ``Lore_ROOT`` points at it
(``lore-capi/lore.h``, ``target/release/``).

Imported Targets
^^^^^^^^^^^^^^^^

``Lore::Lore``
  The Lore library, if found.

Result Variables
^^^^^^^^^^^^^^^^

``Lore_FOUND``
  true if the library and its header were found.
``Lore_VERSION``
  the interface version, read from ``LORE_INTERFACE_VERSION`` in ``lore.h``.
``Lore_INCLUDE_DIRS``
  where to find ``lore.h``.
``Lore_LIBRARIES``
  the library to link against.
``Lore_RUNTIME_LIBRARY``
  on Windows, the path of ``lore.dll``.

#]=======================================================================]

set(_lore_hints ${Lore_ROOT} $ENV{Lore_ROOT})

find_path(Lore_INCLUDE_DIR
    NAMES lore.h
    HINTS ${_lore_hints}
    PATH_SUFFIXES include lore-capi
)

if (WIN32)
    # The import library of lore.dll. Plain "lore.lib" next to it is the static library.
    find_library(Lore_LIBRARY
        NAMES lore.dll.lib lore.dll liblore.dll.a
        HINTS ${_lore_hints}
        PATH_SUFFIXES lib target/release
    )
    find_file(Lore_RUNTIME_LIBRARY
        NAMES lore.dll
        HINTS ${_lore_hints}
        PATH_SUFFIXES bin target/release
    )
    set(_lore_required Lore_INCLUDE_DIR Lore_LIBRARY Lore_RUNTIME_LIBRARY)
else()
    find_library(Lore_LIBRARY
        NAMES lore
        HINTS ${_lore_hints}
        PATH_SUFFIXES lib target/release
    )
    set(_lore_required Lore_INCLUDE_DIR Lore_LIBRARY)
endif()

if (Lore_INCLUDE_DIR AND EXISTS "${Lore_INCLUDE_DIR}/lore.h")
    file(STRINGS "${Lore_INCLUDE_DIR}/lore.h" _lore_version_line
         REGEX "^#define[ \t]+LORE_INTERFACE_VERSION[ \t]+\"[^\"]+\"")
    if (_lore_version_line MATCHES "\"([^\"]+)\"")
        set(Lore_VERSION "${CMAKE_MATCH_1}")
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Lore
    REQUIRED_VARS ${_lore_required}
    VERSION_VAR Lore_VERSION
)

if (Lore_FOUND)
    set(Lore_INCLUDE_DIRS ${Lore_INCLUDE_DIR})
    set(Lore_LIBRARIES Lore::Lore)
    if (NOT TARGET Lore::Lore)
        if (WIN32)
            add_library(Lore::Lore SHARED IMPORTED)
            set_target_properties(Lore::Lore PROPERTIES
                IMPORTED_IMPLIB "${Lore_LIBRARY}"
                IMPORTED_LOCATION "${Lore_RUNTIME_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${Lore_INCLUDE_DIR}"
            )
        else()
            add_library(Lore::Lore UNKNOWN IMPORTED)
            set_target_properties(Lore::Lore PROPERTIES
                IMPORTED_LOCATION "${Lore_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${Lore_INCLUDE_DIR}"
            )
        endif()
    endif()
endif()

mark_as_advanced(Lore_INCLUDE_DIR Lore_LIBRARY Lore_RUNTIME_LIBRARY)
unset(_lore_hints)
unset(_lore_required)
unset(_lore_version_line)
