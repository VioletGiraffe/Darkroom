set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 13.3) # global.pri's QMAKE_MACOSX_DEPLOYMENT_TARGET

# LibRaw's OpenMP runtime is Homebrew's libomp (brew install libomp): Apple's clang ships none
if(PORT STREQUAL "libraw")
	set(HOMEBREW_LIBOMP /opt/homebrew/opt/libomp)
	set(VCPKG_CMAKE_CONFIGURE_OPTIONS
		"-DOpenMP_C_FLAGS=-Xclang -fopenmp -I${HOMEBREW_LIBOMP}/include"
		"-DOpenMP_CXX_FLAGS=-Xclang -fopenmp -I${HOMEBREW_LIBOMP}/include"
		-DOpenMP_C_LIB_NAMES=omp
		-DOpenMP_CXX_LIB_NAMES=omp
		"-DOpenMP_omp_LIBRARY=${HOMEBREW_LIBOMP}/lib/libomp.a")
endif()
