if(NOT EXISTS ${CMAKE_SOURCE_DIR}/src/version.h)
    file(TOUCH ${CMAKE_SOURCE_DIR}/src/version.h)
endif()

list(APPEND CMAKE_MODULE_PATH
    ${CMAKE_SOURCE_DIR}/CMakeModules)
include(GetGitRevisionDescription)

git_describe(GIT_VERSION --tags --always --match "[0-9A-Z]*.[0-9A-Z]*")

# FP3D: in a git worktree .git is a file with an absolute gitdir (and the refs live in the common dir), which
# GetGitRevisionDescription can't follow: GIT_VERSION came back HEAD-HASH-NOTFOUND, version.h stayed empty and
# version.cpp didn't compile. Ask git itself then, and never leave version.h without a VERSION.
if(NOT GIT_VERSION OR "${GIT_VERSION}" MATCHES "NOTFOUND")
    set(_fp3d_git "${GIT_BINARY}")
    if(NOT _fp3d_git)
        find_program(_fp3d_git NAMES git)
    endif()
    if(_fp3d_git)
        execute_process(COMMAND "${_fp3d_git}" -c core.safecrlf=false describe --tags --always
                --match "[0-9A-Z]*.[0-9A-Z]*"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            RESULT_VARIABLE _fp3d_res
            OUTPUT_VARIABLE _fp3d_out
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(_fp3d_res EQUAL 0 AND _fp3d_out)
            set(GIT_VERSION "${_fp3d_out}")
            set(GIT_EXECUTABLE "${_fp3d_git}")
        endif()
    endif()
endif()
if(NOT GIT_VERSION OR "${GIT_VERSION}" MATCHES "NOTFOUND")
    file(READ ${CMAKE_SOURCE_DIR}/src/version.h _fp3d_version_h)
    if(NOT _fp3d_version_h MATCHES "#define VERSION")
        file(WRITE ${CMAKE_SOURCE_DIR}/src/version.h "// NOLINT(cata-header-guard)\n\#define VERSION \"0.I-fp3d\"\n")
    endif()
endif()

if(EXISTS ${GIT_EXECUTABLE})
    execute_process(COMMAND ${GIT_EXECUTABLE} -c core.safecrlf=false diff --quiet
        RESULT_VARIABLE DIRTY_FLAG
    )

    if(DIRTY_FLAG)
        string(APPEND GIT_VERSION "-dirty")
    endif()
endif()

message(NOTICE ${GIT_VERSION})

if("${GIT_VERSION}" MATCHES "GIT-NOTFOUND")
    return()
endif()

if(GIT_VERSION)
    string(REPLACE "-NOTFOUND" "" GIT_VERSION ${GIT_VERSION})
    file(READ  ${CMAKE_SOURCE_DIR}/src/version.h VERSION_H)
    string(REGEX MATCH "#define VERSION \"(.+)\"" VERSION_H "${VERSION_H}")

    if(NOT GIT_VERSION STREQUAL VERSION_H)
        file(WRITE ${CMAKE_SOURCE_DIR}/src/version.h
                "// NOLINT(cata-header-guard)\n\#define VERSION \"${GIT_VERSION}\"\n")
    endif()
endif()

# get_git_head_revision() does not work with worktrees in Windows
execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE _sha1
    OUTPUT_STRIP_TRAILING_WHITESPACE)
string(TIMESTAMP _timestamp %Y-%m-%d-%H%M)
file(WRITE ${CMAKE_SOURCE_DIR}/VERSION.txt "\
build type: Release\n\
build number: ${_timestamp}\n\
commit sha: ${_sha1}\n\
commit url: https://github.com/CleverRaven/Cataclysm-DDA/commit/${_sha1}"
)

