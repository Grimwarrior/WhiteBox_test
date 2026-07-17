#
# Copyright (c) Contributors to the Open 3D Engine Project.
# For complete copyright and license terms please see the LICENSE at the root of this distribution.
#
# SPDX-License-Identifier: Apache-2.0 OR MIT
#
#

include(FetchContent)

if (TARGET 3rdParty::Mcut)
    return()
endif()

function(GetMcut)
    set(MCUT_GIT_REPO "https://github.com/cutdigital/mcut.git")
    set(MCUT_GIT_TAG "d424aec52454c22cd9d436907f28db3595e706e1")

    message(STATUS "WhiteBox Gem uses MCUT (LGPL-3.0-or-later, dual-licensed) ${MCUT_GIT_REPO}")
    message(STATUS "    - MCUT provides the CSG boolean operations (Api::MeshBoolean).")
    message(STATUS "    - NOTE: MCUT is LGPL. For commercial/closed-source distribution review its license terms.")

    set(OLD_LOG_LEVEL ${CMAKE_MESSAGE_LOG_LEVEL})
    set(CMAKE_MESSAGE_LOG_LEVEL ${O3DE_FETCHCONTENT_MESSAGE_LEVEL})
    set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)
    set(ORIGINAL_CMAKE_SUPPRESS_DEVELOPER_WARNINGS ${CMAKE_SUPPRESS_DEVELOPER_WARNINGS})
    set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS ON CACHE BOOL "" FORCE)

    # mcut build options - static lib, no extras
    set(MCUT_BUILD_AS_SHARED_LIB OFF CACHE BOOL "" FORCE)
    set(MCUT_BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
    set(MCUT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MCUT_BUILD_TUTORIALS OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        mcut
        GIT_REPOSITORY ${MCUT_GIT_REPO}
        GIT_TAG        ${MCUT_GIT_TAG}
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(mcut)

    # MSVC fix: mcut's frontend.h uses std::chrono::system_clock but is missing the
    # <chrono> include (GCC/Clang get it transitively via <future>, MSVC's STL does not).
    # Patch the fetched source in place; idempotent, so safe to run on every configure.
    set(mcut_frontend_header "${mcut_SOURCE_DIR}/include/mcut/internal/frontend.h")
    if (EXISTS "${mcut_frontend_header}")
        file(READ "${mcut_frontend_header}" mcut_frontend_contents)
        string(FIND "${mcut_frontend_contents}" "#include <chrono>" mcut_chrono_include_found)
        if (mcut_chrono_include_found EQUAL -1)
            string(REPLACE
                "#include <future>"
                "#include <chrono> // missing include patched by WhiteBox gem (see FindMcut.cmake)\n#include <future>"
                mcut_frontend_contents "${mcut_frontend_contents}")
            file(WRITE "${mcut_frontend_header}" "${mcut_frontend_contents}")
            message(STATUS "WhiteBox Gem: patched mcut frontend.h to add missing <chrono> include (MSVC fix)")
        endif()
    endif()

    set(CMAKE_MESSAGE_LOG_LEVEL ${OLD_LOG_LEVEL})
    set(CMAKE_WARN_DEPRECATED ON CACHE BOOL "" FORCE)
    set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS ${ORIGINAL_CMAKE_SUPPRESS_DEVELOPER_WARNINGS} CACHE BOOL "" FORCE)

    if (NOT TARGET mcut)
        message(FATAL_ERROR "WhiteBox Gem: failed to fetch/configure the mcut library")
    endif()

    target_compile_options(mcut PRIVATE
        ${O3DE_COMPILE_OPTION_DISABLE_WARNINGS}
        ${O3DE_COMPILE_OPTION_ENABLE_EXCEPTIONS})

    if (COMMAND ly_get_engine_relative_source_dir)
        get_property(this_gem_root GLOBAL PROPERTY "@GEMROOT:${gem_name}@")
        ly_get_engine_relative_source_dir(${this_gem_root} relative_this_gem_root)
        set_property(TARGET mcut PROPERTY FOLDER "${relative_this_gem_root}/External")
    endif()

    # mcut declares its include directories PRIVATE, so consumers need an interface
    # wrapper that carries the headers alongside the static library
    add_library(McutInterface INTERFACE IMPORTED GLOBAL)
    target_include_directories(McutInterface INTERFACE "${mcut_SOURCE_DIR}/include")
    target_link_libraries(McutInterface INTERFACE mcut)
endfunction()

GetMcut()

add_library(3rdParty::Mcut ALIAS McutInterface)

set(Mcut_FOUND TRUE)

if (COMMAND ly_install)
    ly_install(FILES ${CMAKE_CURRENT_LIST_DIR}/Installer/FindMcut.cmake DESTINATION cmake/3rdParty)
endif()
