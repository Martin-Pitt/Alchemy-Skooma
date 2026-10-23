# -*- cmake -*-
#
# The air cells: every open gap over the world's solids, read off a solid
# raster of the census's records and land, with the walls' faces, ledges and
# run-off and the sky map over them. Nothing here knows what a viewer object,
# a region or a frame is, which is what makes it testable without one.

add_library(skair STATIC)

target_sources(
  skair
  PRIVATE
    skair.cpp
    skaircensus.cpp
    skaircolumns.cpp
    skairdebug.cpp
    skairdistil.cpp
    skairevict.cpp
    skairfaceoffsets.cpp
    skairlayout.cpp
    skairraster.cpp
    skairskymap.cpp
    skairsoup.cpp
    skairwalls.cpp
)

target_sources(
  skair
  PUBLIC
    FILE_SET HEADERS
      FILES
        skair.h
        skaircensus.h
        skaircolumns.h
        skairdebug.h
        skairdistil.h
        skairevict.h
        skairfaceoffsets.h
        skairlayout.h
        skairraster.h
        skairskymap.h
        skairsoup.h
        skairstats.h
        skairwalls.h
)

target_link_libraries(skair PRIVATE al::flags PUBLIC skcensus)

target_precompile_headers(skair REUSE_FROM llprecompiled)

# The column raster clips triangles at cell edges, and a triangle that only
# touches a column there gives it a span or not by the last bit of the clip.
# Built with IEEE's arithmetic a step at a time, without the viewer's float
# flags (00-Common.cmake), it clips the same on every compiler, so two tiles
# agree at their seam and the raster matches its oracle exactly.
if(WINDOWS)
  set(skair_ieee_options /fp:precise)
else()
  set(
    skair_ieee_options
    -ffp-contract=off
    -fno-associative-math
    -fno-reciprocal-math
  )
endif()
set_source_files_properties(
  skaircolumns.cpp
  PROPERTIES COMPILE_OPTIONS "${skair_ieee_options}" SKIP_PRECOMPILE_HEADERS TRUE
)

if(AL_BUILD_TESTS)
  al_add_test_binary(
    skair_tests
    PROJECT skair
    SOURCES
      tests/skair_test.cpp
      tests/skairbytes_test.cpp
      tests/skaircolumns_test.cpp
      tests/skairdistil_test.cpp
      tests/skairfaceoffsets_test.cpp
      tests/skairraster_test.cpp
      tests/skairskymap_test.cpp
      tests/skairtileclip_test.cpp
      tests/skairwalls_test.cpp
    LIBRARIES skair llcommon
  )
endif()
