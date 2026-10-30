# SPDX-License-Identifier: GPL-3.0-or-later
#
# Stand-in for Sony's proprietary PlayStationModule.cmake, which
# Source/cmake/OptionsPlayStation.cmake includes unconditionally.
#
# The original copies every dependency's .sprx next to the binaries and
# exposes <Name>_LOAD_AT paths for dlopen(). The PS5 homebrew build links
# everything statically into one executable, so all of these are no-ops.

macro(playstation_module _name)
endmacro()

macro(playstation_setup_libc _target)
endmacro()

macro(playstation_setup_fp _target)
endmacro()
