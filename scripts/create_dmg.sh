#!/bin/sh

# Packages the already-built release bundles into one distributable DMG; building them is the caller's job.
# Run from the repository root: ./scripts/create_dmg.sh <Qt install dir>

set -eu

MYSELF="$(basename "$0")"

if [ $# -ne 1 ]; then
	echo "Usage: ./scripts/${MYSELF} <Qt install dir>" >&2
	exit 1
fi

QT_DIR="$1"

APPS="Darkroom Quickroom"
VOLUME="Darkroom"
DMG="${VOLUME}.dmg"
STAGE="build/dmg-staging"

# Every bundle is checked before any is deployed, so a missing one fails before the first is modified.
for app in ${APPS}; do
	if [ ! -d "bin/release/${app}.app" ]; then
		echo "${MYSELF}: bin/release/${app}.app not found - build the release configuration first" >&2
		exit 1
	fi
done

rm -rf "${STAGE}"
mkdir -p "${STAGE}"

for app in ${APPS}; do
	BUNDLE="bin/release/${app}.app"
	echo "${MYSELF}: deploying Qt frameworks into ${BUNDLE}"

	# macdeployqt exits 0 even when it deploys nothing, so its output is the only signal of failure
	DEPLOY_OUTPUT="$("${QT_DIR}/bin/macdeployqt" "${BUNDLE}" 2>&1)"
	echo "${DEPLOY_OUTPUT}"
	if echo "${DEPLOY_OUTPUT}" | grep -q "^ERROR"; then
		echo "${MYSELF}: macdeployqt failed, refusing to package an undeployed bundle" >&2
		exit 1
	fi

	cp -R "${BUNDLE}" "${STAGE}/"
done

echo "${MYSELF}: creating ${DMG}"

ln -s /Applications "${STAGE}/"

# -srcfolder populates via a private nobrowse mount - no volume appears under /Volumes for Spotlight to grab and pin.
hdiutil create "${DMG}" -ov -volname "${VOLUME}" -fs "HFS+" -format ULMO -srcfolder "${STAGE}"

rm -rf "${STAGE}"

echo "${MYSELF}: ready for distribution: ${DMG}"
