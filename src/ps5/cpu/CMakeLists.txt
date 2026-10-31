# CPU affinity helpers of the PS5 processes: the CPU mask type, its list/hex
# forms, the probe for the kernel's cpuset size and the widening policy.
# Portable C++ (the syscalls stay in the frontend), so the host tests cover it.
add_library(prosperoweb_cpu STATIC
  cpu_mask.cpp
  cpuset_assign.cpp
)
add_library(prosperoweb::cpu ALIAS prosperoweb_cpu)
target_include_directories(prosperoweb_cpu PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(prosperoweb_cpu PUBLIC cxx_std_20)
target_link_libraries(prosperoweb_cpu PRIVATE prosperoweb_warnings)
