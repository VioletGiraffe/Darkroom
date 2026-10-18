#!/bin/sh

# Builds Darkroom.pro: compiles and links the apps, runs nothing. Binaries land in bin/<configuration>.
# Exit code: 1 on a build failure, 2 when the environment is incomplete (see environment.sh).
# Arguments, all optional, in this order:
#   darkroom | quickroom  build that app only; both when omitted
#   debug                 build the debug configuration

set -u

TARGETS="sub-Darkroom sub-Quickroom"
case "${1:-}" in
	darkroom) TARGETS=sub-Darkroom; shift ;;
	quickroom) TARGETS=sub-Quickroom; shift ;;
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

run_qmake Darkroom.pro "${CONFIG}"
# shellcheck disable=SC2086 # TARGETS is a word list
make -j"${JOBS}" ${TARGETS} || exit 1
