cmake_minimum_required(VERSION 3.12)

project(SYNC_CLIPBOARD CXX)

#
# libfmt
#
find_package(fmt 7.0 REQUIRED CONFIG)

#
# Find xcb libraries
#
include(FindPkgConfig)
pkg_check_modules(XCB REQUIRED xcb)
add_library(xcb::xcb UNKNOWN IMPORTED)
set_target_properties(
    xcb::xcb PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${XCB_INCLUDE_DIRS}"
)
set_target_properties(
    xcb::xcb PROPERTIES
    IMPORTED_LINK_INTERFACE_LANGUAGES "C"
    IMPORTED_LOCATION "${XCB_LINK_LIBRARIES}"
)

pkg_check_modules(XCB_XFIXES REQUIRED xcb-xfixes)
add_library(xcb::xfixes UNKNOWN IMPORTED)
set_target_properties(
    xcb::xfixes PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${XCB_XFIXES_INCLUDE_DIRS}"
    INTERFACE_LINK_LIBRARIES xcb::xcb
    IMPORTED_LINK_INTERFACE_LANGUAGES "C"
    IMPORTED_LOCATION "${XCB_XFIXES_LINK_LIBRARIES}"
)

#
# Our code
#
add_executable(sync_clipboard src/main.cpp)
target_link_libraries(sync_clipboard fmt::fmt xcb::xfixes)
#target_link_libraries(sync_clipboard xcb-fixes)
target_compile_features(sync_clipboard PUBLIC cxx_std_17)
target_compile_options(sync_clipboard PRIVATE ${PEDANTIC_COMPILE_FLAGS})
