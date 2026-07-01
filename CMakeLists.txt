cmake_minimum_required(VERSION 3.16)
project(fritz-rctd CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED PJ_DIR)
    if(DEFINED ENV{PJ_DIR})
        set(PJ_DIR $ENV{PJ_DIR})
    else()
        set(PJ_DIR /tmp/pjproject)
    endif()
endif()

if(NOT EXISTS "${PJ_DIR}/pjsip/include/pjsua2.hpp")
    message(FATAL_ERROR
        "PJPROJECT not found under ${PJ_DIR}. "
        "Run scripts/build_pjproject.sh first, or set PJ_DIR.")
endif()

# PJPROJECT's own build tags every library with its autoconf target name
# (e.g. libpjsip-x86_64-pc-linux-gnu.a) - pull that exact string out of
# build.mak instead of matching it with a glob wildcard. A wildcard here
# is ambiguous: e.g. lib${name}-*.a for name=pjsip also matches
# libpjsip-simple-*.a and libpjsip-ua-*.a (and for name=pjmedia also
# matches libpjmedia-audiodev/-codec/-videodev-*.a), so the first
# alphabetical match silently isn't the library that was asked for.
if(NOT EXISTS "${PJ_DIR}/build.mak")
    message(FATAL_ERROR
        "${PJ_DIR}/build.mak not found - incomplete PJPROJECT build?")
endif()
file(STRINGS "${PJ_DIR}/build.mak" _target_name_line REGEX "^export TARGET_NAME[ \t]*:=")
if(NOT _target_name_line)
    message(FATAL_ERROR
        "Could not find TARGET_NAME in ${PJ_DIR}/build.mak")
endif()
string(REGEX REPLACE "^export TARGET_NAME[ \t]*:=[ \t]*([^ \t]+).*" "\\1"
       PJ_TARGET_NAME "${_target_name_line}")

# Link statically against the .a archives (not the shared libraries), so
# the finished executable runs without any DYLD_LIBRARY_PATH/LD_LIBRARY_PATH
# gymnastics and can be invoked freely from any calling process.
function(find_pj_static_lib outvar name)
    file(GLOB _matches "${PJ_DIR}/*/lib/lib${name}-${PJ_TARGET_NAME}.a")
    list(LENGTH _matches _count)
    if(_count EQUAL 0)
        message(FATAL_ERROR "lib${name}-${PJ_TARGET_NAME}.a not found under ${PJ_DIR} - incomplete build?")
    endif()
    list(GET _matches 0 _first)
    set(${outvar} ${_first} PARENT_SCOPE)
endfunction()

set(PJ_STATIC_LIB_NAMES
    pjsua2 pjsua pjsip-ua pjsip-simple pjsip
    pjmedia-codec pjmedia-videodev pjmedia-audiodev pjmedia
    pjnath pjlib-util
    srtp resample gsmcodec speex ilbccodec g7221codec webrtc
    pj
)

set(PJ_STATIC_LIBS "")
foreach(_name ${PJ_STATIC_LIB_NAMES})
    find_pj_static_lib(_lib ${_name})
    list(APPEND PJ_STATIC_LIBS ${_lib})
endforeach()

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

if(NOT APPLE)
    # PJLIB's GUID generator (guid_uuid.c) calls into libuuid on Linux;
    # macOS uses a different backend (CFUUID) so this isn't needed there.
    find_library(UUID_LIBRARY uuid REQUIRED)
endif()

add_executable(fritz-rctd src/fritz_rctd.cpp)

target_include_directories(fritz-rctd PRIVATE
    ${PJ_DIR}/pjlib/include
    ${PJ_DIR}/pjlib-util/include
    ${PJ_DIR}/pjnath/include
    ${PJ_DIR}/pjmedia/include
    ${PJ_DIR}/pjsip/include
)

if(APPLE)
    # Apple's ld resolves cyclic references between archives listed
    # multiple times (pjsua2 <-> pjsua <-> pjmedia etc.) on its own during
    # a normal link - this matches exactly the order PJPROJECT's own
    # build.mak uses to link the same archives for its pjsua2_hello_reg
    # example. Deliberately NO CoreAudio/AudioUnit/AudioToolbox: the
    # --disable-sound build (see scripts/build_pjproject.sh) doesn't
    # effectively pull in coreaudio_dev.o as long as linking happens
    # normally (not via -force_load) - matching PJPROJECT's own working
    # native pjsua2_hello_reg example.
    target_link_libraries(fritz-rctd PRIVATE
        ${PJ_STATIC_LIBS}
        OpenSSL::SSL OpenSSL::Crypto
        Threads::Threads
        m
        "-framework Foundation" "-framework AppKit"
    )
else()
    # GNU ld (the default on Linux) processes static archives strictly
    # left to right and, unlike Apple's ld, does not revisit earlier
    # archives to resolve references introduced by later ones. The
    # PJPROJECT libraries reference each other cyclically (pjsua2 <->
    # pjsua <-> pjmedia etc.), so the whole group is wrapped in
    # --start-group/--end-group to let the linker rescan the archives
    # until every symbol resolves.
    target_link_libraries(fritz-rctd PRIVATE
        -Wl,--start-group
        ${PJ_STATIC_LIBS}
        -Wl,--end-group
        OpenSSL::SSL OpenSSL::Crypto
        Threads::Threads
        ${UUID_LIBRARY}
        m
    )
endif()

target_compile_definitions(fritz-rctd PRIVATE
    PJ_AUTOCONF=1
    PJ_HAS_IPV6=1
)
