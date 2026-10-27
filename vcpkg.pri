# LibRaw and libjpeg-turbo with their dependencies, installed from vcpkg.json into vcpkg_installed/.
# A static library gets the include path only: qmake does not pass its LIBS on.
# Must be included after the LIBS of anything that links against them: GNU ld resolves static libraries in command-line order.

# The build scripts install the same triplet: see scripts/environment.*
win*: VCPKG_TRIPLET = x64-windows-static-md
mac*: VCPKG_TRIPLET = arm64-osx # Apple Silicon only
linux*|freebsd {
	contains(QMAKE_HOST.arch, arm64|aarch64): VCPKG_TRIPLET = arm64-linux
	else: VCPKG_TRIPLET = x64-linux
}
VCPKG_INSTALLED = $$PWD/vcpkg_installed/$${VCPKG_TRIPLET}

INCLUDEPATH += $${VCPKG_INSTALLED}/include

!staticlib {
	Release:VCPKG_LIB_DIR = $${VCPKG_INSTALLED}/lib
	Debug:VCPKG_LIB_DIR = $${VCPKG_INSTALLED}/debug/lib

	# Each library precedes the ones it uses
	# Debug builds suffix jasper with "d" everywhere, LibRaw and zlib on Windows only
	win* {
		Release:VCPKG_LIBS = raw_r jasper lcms2 turbojpeg spng_static jpeg zs
		Debug:VCPKG_LIBS = raw_rd jasperd lcms2 turbojpeg spng_static jpeg zsd
	} else {
		Release:VCPKG_LIBS = raw_r jasper lcms2 turbojpeg spng_static jpeg z
		Debug:VCPKG_LIBS = raw_r jasperd lcms2 turbojpeg spng_static jpeg z
	}

	for(name, VCPKG_LIBS) {
		win*: VCPKG_LIB_FILE = $${VCPKG_LIB_DIR}/$${name}.lib
		else: VCPKG_LIB_FILE = $${VCPKG_LIB_DIR}/lib$${name}.a
		LIBS += $${VCPKG_LIB_FILE}
		mac*|linux*|freebsd: PRE_TARGETDEPS += $${VCPKG_LIB_FILE}
	}

	# LibRaw's OpenMP runtime (vcpkg.json). MSVC's arrives through the library's default-library directive.
	linux*|freebsd: LIBS += -fopenmp
	# The libomp LibRaw was built against (vcpkg-triplets/arm64-osx.cmake), linked statically: nothing to bundle
	mac*: LIBS += /opt/homebrew/opt/libomp/lib/libomp.a
}
