# -*- cmake -*-

# A region's cached ground, free of the viewer: the region file with its
# layers, pyramids and sidecar, the index of a grid's region files, and
# the dataset's grey16 PNG cells and index.

set(
  skterrain_SOURCES
  skterraincell.cpp
  skterraindataset.cpp
  skterrainmanifest.cpp
  skterrainpng.cpp
)

set(
  skterrain_HEADERS
  skterraincell.h
  skterraindataset.h
  skterrainmanifest.h
  skterrainpng.h
)

add_library(skterrain STATIC)
target_sources(
  skterrain
  PRIVATE ${skterrain_SOURCES}
  PUBLIC FILE_SET HEADERS FILES ${skterrain_HEADERS}
)

target_link_libraries(
  skterrain
  PRIVATE al::flags ll::libpng ll::zlib-ng
  PUBLIC llcommon
)

if(AL_BUILD_TESTS)
  # The region file's round trip, the pyramid's decimation and masks, the
  # cache's index, and the dataset's grey16 cells; two real cells where
  # SK_TERRAIN_DATASET_DIR names a dataset release.
  al_add_test_binary(
    skterrain_tests
    PROJECT skterrain
    SOURCES
      tests/skterraincell_test.cpp
      tests/skterraindataset_test.cpp
      tests/skterrainmanifest_test.cpp
    LIBRARIES skterrain
    SUPPORT llcommon_test_checks
  )
endif()
