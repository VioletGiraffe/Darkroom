#!/bin/sh

# Builds Tests.pro and runs DarkroomTests. Exit code: 1 on a build or test failure, 2 when the environment is incomplete
# (see environment.sh).
# Arguments, all optional, in this order:
#   debug           build and run the debug configuration
#   build           only build; nobuild only runs what is already built
#   <args>          the rest go to DarkroomTests, e.g. a Catch2 test spec

set -u

CONFIG=release
if [ "${1:-}" = debug ]; then
	CONFIG=debug
	shift
fi

BUILD=1
RUN=1
if [ "${1:-}" = build ]; then
	RUN=""
	shift
elif [ "${1:-}" = nobuild ]; then
	BUILD=""
	shift
fi

. "$(cd "$(dirname "$0")" && pwd)/environment.sh"

if [ -n "${BUILD}" ]; then
	install_vcpkg_dependencies
	run_qmake Tests.pro "${CONFIG}"
	make -j"${JOBS}" sub-tests || exit 1
fi
[ -n "${RUN}" ] || exit 0

EXE="${REPOSITORY_ROOT}/bin/${CONFIG}/DarkroomTests"
if [ ! -x "${EXE}" ]; then
	echo "DarkroomTests: not built"
	exit 1
fi

# --warn NoTests: a filter that matches nothing exits 0 otherwise, so a broken one would pass silently
"${EXE}" "$@" --warn NoTests || exit 1
