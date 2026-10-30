# SPDX-License-Identifier: GPL-3.0-or-later
#
# OptionsPlayStation.cmake calls find_package(libdl) to link Sony's libdl
# wrapper into every executable. The payload SDK's libc already provides
# dlopen()/dlsym() (weak shims over libkernel_web), so report "not found" and
# let OptionsPlayStation skip it.

set(libdl_FOUND FALSE)
