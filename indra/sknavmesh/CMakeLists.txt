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
    sknavcensusio.cpp
    sknavcluster.cpp
    sknavenclosure.cpp
    sknavexport.cpp
    sknavfeatures.cpp
    sknavflights.cpp
    sknavlayout.cpp
    sknavprobecache.cpp
    sknavprobes.cpp
    sknavreplay.cpp
    sknavskymap.cpp
    sknavspaces.cpp
    sknavtilecache.cpp
    sknavwalls.cpp
    sknavwind.cpp
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
        sknavcensusio.h
        sknavcluster.h
        sknavenclosure.h
        sknavexport.h
        sknavfeatures.h
        sknavflights.h
        sknavlayout.h
        sknavprobecache.h
        sknavprobes.h
        sknavreplay.h
        sknavskymap.h
        sknavspaces.h
        sknavtilecache.h
        sknavtypes.h
        sknavwalls.h
        sknavwind.h
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

# TUT tests, kept registered until they are ported to Catch2.
if(AL_BUILD_TESTS AND COMMAND al_add_test)
  al_add_test(sknavair PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavairdistil PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavannotate PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavenclosure PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavcensusio PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavcluster PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavbuild PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavwalls PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavfeatures PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavflights PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavtilecache PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavprobecache PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavprobes PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavprobeinvariants PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavprobeseval PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavreplay PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavrooms PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavwind PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_bench(spacesdraw PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
endif()
