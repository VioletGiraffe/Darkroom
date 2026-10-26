#!/bin/sh

# Builds Darkroom.pro: compiles and links the apps, runs nothing. Binaries land in bin/<configuration>.
# Exit code: 1 on a build failure, 2 when the environment is incomplete (see environment.sh).
# Arguments, all optional, in this order:
#   darkroom | quickroom  build that app only; both when omitted
#   rebuild               instead of an app: rebuild the whole project from scratch, both apps and every library they link
#   debug                 build the debug configuration

set -u

TARGETS="sub-Darkroom sub-Quickroom"
REBUILD=
case "${1:-}" in
	darkroom) TARGETS=sub-Darkroom; shift ;;
	quickroom) TARGETS=sub-Quickroom; shift ;;
	rebuild) TARGETS=; REBUILD=1; shift ;;
esac

CONFIG=release
if [ "${1:-}" = debug ]; then
	CONFIG=debug
	shift
fi

if [ $# -gt 0 ]; then
	echo "Unknown argument: $1" >&2
	exit 2
fi

. "$(cd "$(dirname "$0")" && pwd)/environment.sh"

install_vcpkg_dependencies
run_qmake Darkroom.pro "${CONFIG}"
if [ -n "${REBUILD}" ]; then
	make clean || exit 1
fi
# shellcheck disable=SC2086 # TARGETS is a word list; empty builds the default target, every subproject
make -j"${JOBS}" ${TARGETS} || exit 1
