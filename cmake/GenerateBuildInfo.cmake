# Writes generated/lizard/version.h and generated/BUILDINFO. Run in script mode, at configure time
# and again on every build (target lizard_buildinfo), so the commit and the dirty state are never
# stale. configure_file leaves a file alone when its content is unchanged, so nothing is rebuilt
# unless the build information really changed.
#
#   cmake -DSRC_DIR=... -DBIN_DIR=... -DTEMPLATE=... -DPROJECT_VERSION=... -P GenerateBuildInfo.cmake
#
# Inputs: SRC_DIR BIN_DIR TEMPLATE GIT_EXECUTABLE PROJECT_VERSION PROJECT_VERSION_MAJOR/MINOR/PATCH
#         BUILD_DIR (excluded from the dirty check) CHANNEL (local|official) SIGNING_KEY CONFIG TARGET COMPILER JIT LTO SANITIZERS STATIC
#
# Build kind:
#   release   clean tree, HEAD is exactly the tag vX.Y.Z
#   snapshot  clean tree, HEAD is past the last tag (or there is no tag)
#   dirty     uncommitted changes (the version carries a hash of them, so two dirty builds of
#             different edits do not look alike)
#   source    no git: a source tarball. build.sh puts a .buildinfo next to the sources, which
#             this script then reads; without it the commit is "unknown".
cmake_minimum_required(VERSION 3.16)

foreach(required SRC_DIR BIN_DIR TEMPLATE PROJECT_VERSION)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "GenerateBuildInfo: ${required} is not set")
    endif()
endforeach()
if(NOT DEFINED CHANNEL OR CHANNEL STREQUAL "")
    set(CHANNEL local)
endif()

# Runs git in SRC_DIR. Sets ${out} to its output and ${out}_RC to its exit status.
function(lz_git out)
    if(NOT GIT_EXECUTABLE)
        set(${out} "" PARENT_SCOPE)
        set(${out}_RC 127 PARENT_SCOPE)
        return()
    endif()
    execute_process(
        COMMAND ${GIT_EXECUTABLE} -C ${SRC_DIR} ${ARGN}
        OUTPUT_VARIABLE text ERROR_QUIET RESULT_VARIABLE rc
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(${out} "${text}" PARENT_SCOPE)
    set(${out}_RC ${rc} PARENT_SCOPE)
endfunction()

# Build directories are not part of the source. A build directory inside SRC_DIR that is not
# ignored (cmake -B out) would otherwise turn the tree dirty, and so would a second one made for
# another configuration. Prefixes of such directories, relative to SRC_DIR, with a trailing slash.
set(build_prefixes "")
if(DEFINED BUILD_DIR AND NOT BUILD_DIR STREQUAL "")
    file(RELATIVE_PATH build_rel "${SRC_DIR}" "${BUILD_DIR}")
    if(NOT build_rel MATCHES "^\\.\\." AND NOT build_rel STREQUAL "")
        list(APPEND build_prefixes "${build_rel}/")
    endif()
endif()

set(kind source)
set(commit unknown)
set(commit_short unknown)
set(commit_date unknown)
set(dirty 0)
set(dirty_hash "")
set(commits_since 0)
set(tag "")

# Only a repository rooted at SRC_DIR counts: sources unpacked inside some other checkout must not
# report that checkout's commit.
lz_git(prefix rev-parse --show-prefix)
lz_git(inside rev-parse --is-inside-work-tree)
if(inside_RC EQUAL 0 AND inside STREQUAL "true" AND prefix_RC EQUAL 0 AND prefix STREQUAL "")
    lz_git(commit rev-parse HEAD)
    if(NOT commit_RC EQUAL 0)
        # a repository with no commit yet
        set(kind source)
        set(commit unknown)
    else()
        lz_git(commit_short rev-parse --short=7 HEAD)
        lz_git(commit_date show -s --format=%cI HEAD)
        lz_git(tag describe --tags --abbrev=0 --match "v[0-9]*")
        if(tag_RC EQUAL 0 AND NOT tag STREQUAL "")
            lz_git(commits_since rev-list --count ${tag}..HEAD)
        else()
            set(tag "")
            lz_git(commits_since rev-list --count HEAD)
        endif()

        lz_git(status status --porcelain --untracked-files=normal)
        string(REPLACE "\n" ";" status_lines "${status}")
        set(changes "")
        foreach(line IN LISTS status_lines)
            if(line MATCHES "^\\?\\? (.*)/$")
                set(untracked_dir "${CMAKE_MATCH_1}")
                if(EXISTS "${SRC_DIR}/${untracked_dir}/CMakeCache.txt"
                   OR EXISTS "${SRC_DIR}/${untracked_dir}/CMakeFiles")
                    list(APPEND build_prefixes "${untracked_dir}/")
                    continue()
                endif()
            endif()
            list(APPEND changes "${line}")
        endforeach()
        if(NOT changes STREQUAL "")
            set(dirty 1)
            lz_git(diff diff HEAD)
            lz_git(untracked ls-files --others --exclude-standard)
            string(REPLACE "\n" ";" untracked "${untracked}")
            set(material "${diff}")
            foreach(file IN LISTS untracked)
                set(in_build FALSE)
                foreach(prefix IN LISTS build_prefixes)
                    string(FIND "${file}" "${prefix}" at)
                    if(at EQUAL 0)
                        set(in_build TRUE)
                    endif()
                endforeach()
                if(NOT in_build AND EXISTS "${SRC_DIR}/${file}" AND NOT IS_DIRECTORY "${SRC_DIR}/${file}")
                    file(SHA256 "${SRC_DIR}/${file}" file_hash)
                    string(APPEND material "\n${file} ${file_hash}")
                endif()
            endforeach()
            string(SHA256 all "${material}")
            string(SUBSTRING "${all}" 0 8 dirty_hash)
            set(kind dirty)
        elseif(NOT tag STREQUAL "" AND commits_since EQUAL 0)
            set(kind release)
        else()
            set(kind snapshot)
        endif()
    endif()
elseif(EXISTS "${SRC_DIR}/.buildinfo")
    # A source tarball made by build.sh: the state of the tree it was cut from.
    file(STRINGS "${SRC_DIR}/.buildinfo" lines)
    foreach(line IN LISTS lines)
        if(line MATCHES "^([a-z_]+)=(.*)$")
            set(info_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
        endif()
    endforeach()
    foreach(key kind commit commit_short commit_date dirty_hash commits_since)
        if(DEFINED info_${key} AND NOT info_${key} STREQUAL "")
            set(${key} "${info_${key}}")
        endif()
    endforeach()
    if(kind STREQUAL "dirty")
        set(dirty 1)
    endif()
endif()

# SOURCE_DATE_EPOCH wins over the commit date, for reproducible packages.
if(DEFINED ENV{SOURCE_DATE_EPOCH} AND NOT "$ENV{SOURCE_DATE_EPOCH}" STREQUAL "")
    string(TIMESTAMP commit_date "%Y-%m-%dT%H:%M:%SZ" UTC)
endif()

# ---- the version string -------------------------------------------------------------------
set(core "${PROJECT_VERSION}")
if(kind STREQUAL "release")
    set(full "${core}")
    if(NOT tag STREQUAL "v${core}")
        message(WARNING "HEAD is tagged ${tag} but the project version is ${core}")
    endif()
elseif(kind STREQUAL "snapshot")
    set(full "${core}-dev.${commits_since}+g${commit_short}")
elseif(kind STREQUAL "dirty")
    if(commits_since GREATER 0)
        set(full "${core}-dev.${commits_since}+g${commit_short}.dirty.${dirty_hash}")
    else()
        set(full "${core}+g${commit_short}.dirty.${dirty_hash}")
    endif()
else()
    if(commit STREQUAL "unknown")
        set(full "${core}+source")
    else()
        set(full "${core}+g${commit_short}")
    endif()
endif()

# ---- channel and signature ----------------------------------------------------------------
# "official" is the claim that this build is meant to be signed by the maintainers: it needs a
# clean tree (a dirty one is, by definition, not what anybody signed) and a signing key. The
# signature itself is a detached file next to the package (build.sh package --official); the
# binary only says who is expected to have signed it.
if(CHANNEL STREQUAL "official")
    if(kind STREQUAL "dirty" OR (kind STREQUAL "source" AND commit STREQUAL "unknown"))
        message(FATAL_ERROR
            "An official build needs a clean tree with a known commit (this one is '${kind}'). "
            "Commit your changes, or build with -DLZ_BUILD_CHANNEL=local.")
    endif()
    if(NOT DEFINED SIGNING_KEY OR SIGNING_KEY STREQUAL "")
        message(FATAL_ERROR "An official build needs a signing key: -DLZ_SIGNING_KEY=<fingerprint> "
                            "(build.sh --official finds it for you).")
    endif()
    set(signed 1)
    set(signer "${SIGNING_KEY}")
elseif(CHANNEL STREQUAL "local")
    set(signed 0)
    set(signer "")
else()
    message(FATAL_ERROR "LZ_BUILD_CHANNEL must be 'local' or 'official', not '${CHANNEL}'")
endif()

# ---- the rest ------------------------------------------------------------------------------
function(lz_c_string out value)
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    string(REPLACE "\n" " " value "${value}")
    set(${out} "${value}" PARENT_SCOPE)
endfunction()

lz_c_string(LZ_VERSION_FULL "${full}")
lz_c_string(LZ_BUILD_KIND "${kind}")
lz_c_string(LZ_BUILD_COMMIT "${commit}")
lz_c_string(LZ_BUILD_COMMIT_SHORT "${commit_short}")
lz_c_string(LZ_BUILD_DATE "${commit_date}")
lz_c_string(LZ_BUILD_DIRTY_HASH "${dirty_hash}")
lz_c_string(LZ_BUILD_CHANNEL "${CHANNEL}")
lz_c_string(LZ_BUILD_SIGNER "${signer}")
if(signed)
    lz_c_string(LZ_BUILD_CHANNEL_NOTE " (signed, key ${signer}; verify the .asc of the package)")
else()
    set(LZ_BUILD_CHANNEL_NOTE " (unsigned)")
endif()
lz_c_string(LZ_BUILD_CONFIG "${CONFIG}")
lz_c_string(LZ_BUILD_TARGET "${TARGET}")
lz_c_string(LZ_BUILD_COMPILER "${COMPILER}")
set(LZ_BUILD_DIRTY ${dirty})
set(LZ_BUILD_SIGNED ${signed})
foreach(feature JIT LTO SANITIZERS STATIC)
    if(${feature})
        set(LZ_BUILD_${feature} 1)
        set(LZ_BUILD_${feature}_TEXT on)
    else()
        set(LZ_BUILD_${feature} 0)
        set(LZ_BUILD_${feature}_TEXT off)
    endif()
endforeach()

configure_file("${TEMPLATE}" "${BIN_DIR}/lizard/version.h" @ONLY)

# The same facts as plain text: installed as share/doc/lizard/BUILDINFO and read by tools.
file(WRITE "${BIN_DIR}/BUILDINFO.tmp"
"version=${full}
core_version=${core}
kind=${kind}
commit=${commit}
commit_short=${commit_short}
commit_date=${commit_date}
commits_since=${commits_since}
dirty=${dirty}
dirty_hash=${dirty_hash}
channel=${CHANNEL}
signed=${signed}
signer=${signer}
config=${CONFIG}
target=${TARGET}
compiler=${COMPILER}
jit=${LZ_BUILD_JIT_TEXT}
lto=${LZ_BUILD_LTO_TEXT}
sanitizers=${LZ_BUILD_SANITIZERS_TEXT}
static=${LZ_BUILD_STATIC_TEXT}
")
configure_file("${BIN_DIR}/BUILDINFO.tmp" "${BIN_DIR}/BUILDINFO" COPYONLY)
file(REMOVE "${BIN_DIR}/BUILDINFO.tmp")
