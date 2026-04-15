# FindGMP.cmake
#
# Locates the GNU Multiple Precision Arithmetic Library.
#
# Provides:
#   GMP_FOUND
#   GMP_INCLUDE_DIRS
#   GMP_LIBRARIES
#   GMP_VERSION
#   imported target GMP::GMP

find_path(GMP_INCLUDE_DIR gmp.h
    HINTS
        /opt/homebrew/opt/gmp/include
        /opt/homebrew/include
        /usr/local/include
)

find_library(GMP_LIBRARY
    NAMES gmp
    HINTS
        /opt/homebrew/opt/gmp/lib
        /opt/homebrew/lib
        /usr/local/lib
)

if(GMP_INCLUDE_DIR AND EXISTS "${GMP_INCLUDE_DIR}/gmp.h")
    file(STRINGS "${GMP_INCLUDE_DIR}/gmp.h" _gmp_version_lines
         REGEX "^#define[ \t]+__GNU_MP_VERSION(_MINOR|_PATCHLEVEL)?[ \t]+[0-9]+")
    set(_gmp_major 0)
    set(_gmp_minor 0)
    set(_gmp_patch 0)
    foreach(_line IN LISTS _gmp_version_lines)
        if(_line MATCHES "__GNU_MP_VERSION[ \t]+([0-9]+)")
            set(_gmp_major ${CMAKE_MATCH_1})
        elseif(_line MATCHES "__GNU_MP_VERSION_MINOR[ \t]+([0-9]+)")
            set(_gmp_minor ${CMAKE_MATCH_1})
        elseif(_line MATCHES "__GNU_MP_VERSION_PATCHLEVEL[ \t]+([0-9]+)")
            set(_gmp_patch ${CMAKE_MATCH_1})
        endif()
    endforeach()
    set(GMP_VERSION "${_gmp_major}.${_gmp_minor}.${_gmp_patch}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(GMP
    REQUIRED_VARS GMP_LIBRARY GMP_INCLUDE_DIR
    VERSION_VAR   GMP_VERSION
)

if(GMP_FOUND)
    set(GMP_LIBRARIES    "${GMP_LIBRARY}")
    set(GMP_INCLUDE_DIRS "${GMP_INCLUDE_DIR}")
    if(NOT TARGET GMP::GMP)
        add_library(GMP::GMP UNKNOWN IMPORTED)
        set_target_properties(GMP::GMP PROPERTIES
            IMPORTED_LOCATION             "${GMP_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${GMP_INCLUDE_DIR}"
        )
    endif()
endif()

mark_as_advanced(GMP_INCLUDE_DIR GMP_LIBRARY)
