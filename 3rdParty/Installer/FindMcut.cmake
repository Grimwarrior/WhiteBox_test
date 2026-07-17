#
# Copyright (c) Contributors to the Open 3D Engine Project.
# For complete copyright and license terms please see the LICENSE at the root of this distribution.
#
# SPDX-License-Identifier: Apache-2.0 OR MIT
#
#

if (TARGET 3rdParty::Mcut)
    return()
endif()

# This file is used in the INSTALLER version of O3DE.
# This file is included in cmake/3rdParty, which is already part of the search path for Findxxxxx.cmake files.

# The MCUT library is used privately inside the WhiteBox gem (for the CSG boolean operations exposed
# via Api::MeshBoolean), so neither its headers nor its static library needs to be distributed here.
# It is not expected for people to link to it, but rather use it via the WhiteBox Gem's public API.

# It is still worth notifying people that they are accepting a 3rd Party Library here, what license it
# uses, and where to get it.
message(STATUS "WhiteBox Gem uses MCUT (LGPL-3.0-or-later, dual-licensed) from https://github.com/cutdigital/mcut.git")
message(STATUS "    - NOTE: MCUT is LGPL. For commercial/closed-source distribution review its license terms.")

# By providing both an "McutInterface" and a "3rdParty::Mcut" target, we stop O3DE from doing anything
# automatically itself, such as attempting to invoke some other install script or find script or
# complaining about a missing target.
add_library(McutInterface IMPORTED INTERFACE GLOBAL)
add_library(3rdParty::Mcut ALIAS McutInterface)

# notify O3DE that we have satisfied the Mcut find_package requirements.
set(Mcut_FOUND TRUE)
