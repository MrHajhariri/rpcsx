project(rx)

find_package(Git)

add_library(${PROJECT_NAME} OBJECT
    src/debug.cpp
    src/die.cpp
    src/FileLock.cpp
    src/hexdump.cpp
    src/mem.cpp
    src/SharedAtomic.cpp
    src/SharedCV.cpp
    src/SharedMutex.cpp
    src/Version.cpp
)

target_include_directories(${PROJECT_NAME}
PUBLIC
    include

PRIVATE
    include/${PROJECT_NAME}
)

target_link_libraries(${PROJECT_NAME} PUBLIC fmt::fmt)

if (Git_FOUND)
    # Note: execute_process() does not run a shell, so --pretty=format:'%cd'
    # keeps the single quotes and they end up in the value. They are dropped
    # here instead of being sliced away with substring offsets below.
    execute_process(COMMAND ${GIT_EXECUTABLE} log --date=format:%Y%m%d --pretty=format:%cd -n 1 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" OUTPUT_VARIABLE GIT_DATE)

    execute_process(COMMAND ${GIT_EXECUTABLE} log --pretty=format:%h -n 1
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE GIT_REV)

    execute_process(
        COMMAND ${GIT_EXECUTABLE} diff --quiet --exit-code
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE GIT_DIRTY ERROR_QUIET)

    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE GIT_BRANCH)

    string(STRIP "${GIT_DATE}" GIT_DATE)
    string(STRIP "${GIT_REV}" GIT_REV)
    string(STRIP "${GIT_DIRTY}" GIT_DIRTY)
    string(STRIP "${GIT_BRANCH}" GIT_BRANCH)

    # git prints nothing when the repository has no commits yet, or when the
    # sources were extracted from an archive that has no .git directory.
    # Guard it, otherwise string(SUBSTRING) below aborts the whole configure.
    if (GIT_DATE STREQUAL "" OR GIT_REV STREQUAL "")
        message(STATUS "No git metadata found, using fallback version information")
        set(GIT_DATE 0)
        set(GIT_REV 0)
        set(GIT_DIRTY 1)
        set(GIT_BRANCH Unknown)
    else()
        # RX_RAW_VERSION / RX_GIT_REV are injected as C++ integer literals.
        # Take the full yyyymmdd value: starting at offset 1 would leave a
        # leading zero, i.e. an octal constant, and the digits 8/9 (months
        # 08/09, days 08/09/18/19/28/29, all of 2028/2029) would then fail to
        # compile. Keep the short hash at 7 characters as before.
        string(SUBSTRING "${GIT_DATE}" 0 8 GIT_DATE)
        string(SUBSTRING "${GIT_REV}" 0 7 GIT_REV)
    endif()
else()
    set(GIT_DATE 0)
    set(GIT_REV 0)
    set(GIT_DIRTY 1)
    set(GIT_BRANCH Unknown)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
    RX_RAW_VERSION=${GIT_DATE}
    RX_GIT_REV=0x${GIT_REV}
    RX_GIT_DIRTY=${GIT_DIRTY}
    RX_GIT_BRANCH=${GIT_BRANCH}
)


# RX_TAG/RX_TAG_VERSION are defaulted by the top level CMakeLists *after* this
# directory has been processed. Default them here too, otherwise the version
# file below is written with empty tag fields ("date--branch-rev-dirty") and
# consumers such as .github/workflows/rpcsx.yml read the wrong fields.
if (NOT RX_TAG)
    set(RX_TAG 0)
endif()

if (NOT RX_TAG_VERSION)
    set(RX_TAG_VERSION 0)
endif()

file(WRITE "${CMAKE_SOURCE_DIR}/.rx.version" "${GIT_DATE}-${RX_TAG}-${RX_TAG_VERSION}-${GIT_BRANCH}-${GIT_REV}-${GIT_DIRTY}")
