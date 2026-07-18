cmake_minimum_required(VERSION 3.16)
project(liblkmdbg VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)
include(CTest)
include(CMakePackageConfigHelpers)

set(LKMDBG_SOURCE src/lkmdbg.c)
set(LKMDBG_PUBLIC_HEADERS
    include/lkmdbg/lkmdbg.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/lkmdbg_ioctl.h)

add_library(lkmdbg SHARED ${LKMDBG_SOURCE})
add_library(lkmdbg_static STATIC ${LKMDBG_SOURCE})
add_library(lkmdbg::shared ALIAS lkmdbg)
add_library(lkmdbg::static ALIAS lkmdbg_static)

foreach(target lkmdbg lkmdbg_static)
    target_compile_features(${target} PUBLIC c_std_11)
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    set_target_properties(${target} PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
endforeach()

set_target_properties(lkmdbg PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME lkmdbg)
set_target_properties(lkmdbg_static PROPERTIES OUTPUT_NAME lkmdbg)

install(TARGETS lkmdbg lkmdbg_static
    EXPORT lkmdbgTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/lkmdbg/lkmdbg.h ../include/lkmdbg_ioctl.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lkmdbg)
install(EXPORT lkmdbgTargets
    FILE lkmdbgTargets.cmake
    NAMESPACE lkmdbg::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lkmdbg)
configure_package_config_file(
    cmake/lkmdbgConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/lkmdbgConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lkmdbg)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/lkmdbgConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/lkmdbgConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/lkmdbgConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lkmdbg)

configure_file(liblkmdbg.pc.in liblkmdbg.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/liblkmdbg.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

if(BUILD_TESTING)
    add_executable(lkmdbg_api_test tests/test_api.c)
    target_link_libraries(lkmdbg_api_test PRIVATE lkmdbg_static)
    add_test(NAME lkmdbg_api_test COMMAND lkmdbg_api_test)
endif()
