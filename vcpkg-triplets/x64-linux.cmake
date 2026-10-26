set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)

# libjpeg-turbo's x86 SIMD needs NASM, and without it the build silently falls back to plain C
if(PORT STREQUAL "libjpeg-turbo")
	set(VCPKG_CMAKE_CONFIGURE_OPTIONS -DREQUIRE_SIMD=ON)
endif()

