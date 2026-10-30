# SPDX-License-Identifier: GPL-3.0-or-later
#
# Stand-in for Sony's proprietary Sign.cmake: homebrew ELF payloads are not
# signed, so sign() does nothing.

function(sign _target)
endfunction()
