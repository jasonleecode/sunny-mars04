# SPDX-License-Identifier: MIT
if(NOT APPLE)
  message(FATAL_ERROR "This packaging step requires macOS")
endif()
if(NOT IS_DIRECTORY "${MARS04_APP}/Contents/MacOS")
  message(FATAL_ERROR "Set MARS04_APP to an existing app bundle")
endif()
include(BundleUtilities)
fixup_bundle("${MARS04_APP}/Contents/MacOS/mars04-viewer" "" "${MARS04_LIBRARY_DIRS}")
