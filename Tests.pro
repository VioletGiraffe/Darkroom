TEMPLATE = subdirs

# Tests are their own project: Darkroom.pro, and the solution generated from it, is the app and its libraries.
# The libraries tests links are built here rather than taken from a Darkroom.pro build, so the test binary can be
# built and run on its own - including when the app itself does not compile.
SUBDIRS += cpputils qtutils tests

qtutils.depends = cpputils
tests.depends = cpputils qtutils
