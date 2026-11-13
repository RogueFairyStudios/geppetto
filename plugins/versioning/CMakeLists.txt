# On Android the Projects panel and the versioning drivers are left out of the build.
if(NOT ANDROID)
    add_subdirectory( localfolder )
endif()

if(HAVE_LORE AND NOT ANDROID)
    add_subdirectory( lore )
endif()
