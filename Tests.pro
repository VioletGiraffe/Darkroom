TEMPLATE = subdirs

# Tests are their own project: Darkroom.pro, and the solution generated from it, is the apps and their libraries.
# The libraries the tests link are built here, so the test binary builds and runs even when the apps' own code does not compile.
SUBDIRS += cpputils qtutils Shared tests

qtutils.depends = cpputils
tests.depends = cpputils qtutils Shared
