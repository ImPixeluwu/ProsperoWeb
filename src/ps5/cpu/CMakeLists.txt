# CPU affinity helpers of the PS5 processes: the CPU mask type, its list/hex
# forms, the probe for the kernel's cpuset size and the widening policy.
# Portable C++ (the syscalls stay in the frontend), so the host tests cover it.
add_library(prosperobrowser_cpu STATIC
  cpu_mask.cpp
  cpuset_assign.cpp
)
add_library(prosperobrowser::cpu ALIAS prosperobrowser_cpu)
target_include_directories(prosperobrowser_cpu PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(prosperobrowser_cpu PUBLIC cxx_std_20)
target_link_libraries(prosperobrowser_cpu PRIVATE prosperobrowser_warnings)
