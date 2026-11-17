#!/bin/sh

# Packages the already-built release binaries into one distributable AppImage per app; building them is the caller's job.
# Run from the repository root: ./scripts/create_appimage.sh <Qt install dir>
# Downloads the continuous builds of linuxdeploy and its Qt plugin into build/appimage-tools/ unless already there.

set -eu

MYSELF="$(basename "$0")"

if [ $# -ne 1 ]; then
	echo "Usage: ./scripts/${MYSELF} <Qt install dir>" >&2
	exit 1
fi

QT_DIR="$1"

APPS="Darkroom Quickroom"
TOOLS="build/appimage-tools"
APPDIR="build/AppDir"

# Every binary is checked before any is deployed, so a missing one fails before the first AppImage is made.
for app in ${APPS}; do
	if [ ! -x "bin/release/${app}" ]; then
		echo "${MYSELF}: bin/release/${app} not found - build the release configuration first" >&2
		exit 1
	fi
done

# The tools are named by `uname -m`; the AppImages say arm64, not aarch64
MACHINE="$(uname -m)"
case "${MACHINE}" in
	x86_64) ARCH_NAME="x86_64" ;;
	aarch64) ARCH_NAME="arm64" ;;
	*) echo "${MYSELF}: unsupported architecture ${MACHINE}" >&2; exit 1 ;;
esac

mkdir -p "${TOOLS}"
for tool in linuxdeploy linuxdeploy-plugin-qt; do
	TOOL_FILE="${TOOLS}/${tool}-${MACHINE}.AppImage"
	if [ ! -x "${TOOL_FILE}" ]; then
		echo "${MYSELF}: downloading ${tool}"
		curl -fsSL -o "${TOOL_FILE}" "https://github.com/linuxdeploy/${tool}/releases/download/continuous/${tool}-${MACHINE}.AppImage"
		chmod +x "${TOOL_FILE}"
	fi
done

# The tools run without FUSE, which containers and CI runners lack
export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE="${QT_DIR}/bin/qmake"
export LD_LIBRARY_PATH="${QT_DIR}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
# xcb is always deployed; Wayland also brings its shell integration and decoration plugins
EXTRA_PLATFORM_PLUGINS="$(find "${QT_DIR}/plugins/platforms" -name 'libqwayland*.so' -printf '%f;')"
export EXTRA_PLATFORM_PLUGINS
# The Qt plugin deploys every multimedia backend. GStreamer's is unused, FFmpeg being Qt's default: its libraries are skipped
# here and its plugin is deleted after deployment.
export LINUXDEPLOY_EXCLUDED_LIBRARIES='libgst*'

LINUXDEPLOY="${TOOLS}/linuxdeploy-${MACHINE}.AppImage"

for app in ${APPS}; do
	ID="$(echo "${app}" | tr '[:upper:]' '[:lower:]')"
	OUTPUT="${app}-${ARCH_NAME}.AppImage"

	echo "${MYSELF}: deploying ${app} into ${APPDIR}"
	rm -rf "${APPDIR}"
	"${LINUXDEPLOY}" --appdir "${APPDIR}" --executable "bin/release/${app}" \
		--desktop-file "${app}/res/${ID}.desktop" --icon-file "${app}/res/${ID}.svg" --plugin qt

	# The apps use neither QNetworkInformation nor TLS
	rm -rf "${APPDIR}/usr/plugins/networkinformation" "${APPDIR}/usr/plugins/tls"
	# See LINUXDEPLOY_EXCLUDED_LIBRARIES
	rm -f "${APPDIR}/usr/plugins/multimedia/libgstreamermediaplugin.so"

	mkdir -p "${APPDIR}/usr/share/doc/${ID}"
	cp LICENSE NOTICE "${APPDIR}/usr/share/doc/${ID}/"

	echo "${MYSELF}: creating ${OUTPUT}"
	LDAI_OUTPUT="${OUTPUT}" "${LINUXDEPLOY}" --appdir "${APPDIR}" --output appimage
	echo "${MYSELF}: ready for distribution: ${OUTPUT}"
done

rm -rf "${APPDIR}"
