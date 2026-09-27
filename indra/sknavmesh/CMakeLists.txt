# -*- cmake -*-
#
# The navmesh proper: the data model the viewer fills in, and the algorithms
# over it. Nothing here knows what a viewer object, a region or a frame is,
# which is what makes it testable without one.

add_library(sknavmesh STATIC)

target_sources(
  sknavmesh
  PRIVATE
    sknavair.cpp
    sknavairdistil.cpp
    sknavannotate.cpp
    sknavbuild.cpp
    sknavcluster.cpp
    sknavfeatures.cpp
    sknavflights.cpp
    sknavskymap.cpp
    sknavtilecache.cpp
    sknavwalls.cpp
)

target_sources(
  sknavmesh
  PUBLIC
    FILE_SET HEADERS
      FILES
        sknavair.h
        sknavairdistil.h
        sknavannotate.h
        sknavbuild.h
        sknavcluster.h
        sknavfeatures.h
        sknavflights.h
        sknavskymap.h
        sknavtilecache.h
        sknavtypes.h
        sknavwalls.h
)

# The build fingerprint keys the tile cache (sknavfingerprint.cmake).
file(
  GLOB sknav_fingerprinted
  CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/*.h
)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/sknavfingerprint.h
  COMMAND
    ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
    -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generated/sknavfingerprint.h -P
    ${CMAKE_CURRENT_SOURCE_DIR}/sknavfingerprint.cmake
  DEPENDS ${sknav_fingerprinted} ${CMAKE_CURRENT_SOURCE_DIR}/sknavfingerprint.cmake
  COMMENT "Fingerprinting the navmesh sources"
)
target_sources(sknavmesh PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated/sknavfingerprint.h)
target_include_directories(sknavmesh PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

target_link_libraries(sknavmesh PRIVATE al::flags PUBLIC llmath llcommon ll::recast)

target_precompile_headers(sknavmesh REUSE_FROM llprecompiled)

if(AL_BUILD_TESTS)
  al_add_test(sknavair PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavairdistil PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavannotate PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavcluster PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavbuild PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavwalls PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavfeatures PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavflights PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavtilecache PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
endif()
