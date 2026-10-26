# Sourced by build.sh and run_tests.sh. Sets:
#   REPOSITORY_ROOT - the Darkroom checkout
#   QMAKE           - the qmake to run
#   JOBS            - the parallel job count for make
#   VCPKG           - the vcpkg to run
# Exits with code 2 when the environment is incomplete.
# The Qt kit is resolved in order: QT_ROOT_DIR as already set (what jurplel/install-qt-action exports on CI);
# local-env.sh beside this script, git-ignored, where a developer sets it for their machine; a qmake6 or qmake
# already on PATH (a distribution's Qt).
# vcpkg comes from VCPKG_ROOT, else from PATH.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPOSITORY_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

[ -z "${QT_ROOT_DIR:-}" ] && [ -f "${SCRIPT_DIR}/local-env.sh" ] && . "${SCRIPT_DIR}/local-env.sh"

if [ -n "${QT_ROOT_DIR:-}" ]; then
	QMAKE="${QT_ROOT_DIR}/bin/qmake"
	if [ ! -x "${QMAKE}" ]; then
		echo "No qmake under \"${QT_ROOT_DIR}/bin\"." >&2
		exit 2
	fi
elif command -v qmake6 >/dev/null 2>&1; then
	QMAKE=qmake6
elif command -v qmake >/dev/null 2>&1; then
	QMAKE=qmake
else
	echo "QT_ROOT_DIR is not set and no qmake is on PATH. Set QT_ROOT_DIR to the Qt kit directory, the one holding bin/qmake, or set it in a git-ignored local-env.sh beside this script." >&2
	exit 2
fi

JOBS="$(getconf _NPROCESSORS_ONLN)"

if [ -n "${VCPKG_ROOT:-}" ]; then
	VCPKG="${VCPKG_ROOT}/vcpkg"
elif command -v vcpkg >/dev/null 2>&1; then
	VCPKG=vcpkg
else
	echo "vcpkg was not found: set VCPKG_ROOT to its directory or put it on PATH." >&2
	exit 2
fi

# Installs vcpkg.json's dependencies for the triplet vcpkg.pri links against. Exits with code 1 when the install fails.
install_vcpkg_dependencies() {
	case "$(uname -m)" in
		arm64|aarch64) arch=arm64 ;;
		*) arch=x64 ;;
	esac
	case "$(uname -s)" in
		Darwin) triplet="${arch}-osx" ;;
		*) triplet="${arch}-linux" ;;
	esac
	"${VCPKG}" install --triplet "${triplet}" --x-manifest-root="${REPOSITORY_ROOT}" || exit 1
}

# Runs qmake on a top-level project in the repository root for configuration $2. Exits with code 1 when qmake fails.
# Tests.pro and Darkroom.pro both generate the top-level Makefile, so every build regenerates it for its own project.
run_qmake() {
	# A kept .qmake.stash pins the toolchain probed when it was written, so every run probes the current one instead.
	# One is written per subproject build directory, and qmake also searches upward, so the sweep covers the repository.
	find "${REPOSITORY_ROOT}" -name .qmake.stash -delete
	cd "${REPOSITORY_ROOT}" || exit 2
	"${QMAKE}" -v
	"${QMAKE}" -r CONFIG+="$2" "$1" || exit 1
}
