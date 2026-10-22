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

target_link_libraries(skair PRIVATE al::flags ll::recast PUBLIC skcensus)

target_precompile_headers(skair REUSE_FROM llprecompiled)

if(AL_BUILD_TESTS)
  al_add_test_binary(
    skair_tests
    PROJECT skair
    SOURCES
      tests/skair_test.cpp
      tests/skairbytes_test.cpp
      tests/skairdistil_test.cpp
      tests/skairfaceoffsets_test.cpp
      tests/skairraster_test.cpp
      tests/skairskymap_test.cpp
      tests/skairtileclip_test.cpp
      tests/skairwalls_test.cpp
    LIBRARIES skair llcommon ll::recast
  )
endif()
