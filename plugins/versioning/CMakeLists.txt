add_subdirectory( localfolder )

if(HAVE_LORE AND NOT ANDROID)
    add_subdirectory( lore )
endif()
