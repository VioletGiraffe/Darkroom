TEMPLATE = subdirs

SUBDIRS += app qtutils cpputils cpp-template-utils magic-alignment tests

qtutils.depends = cpputils
app.depends = qtutils magic-alignment
tests.depends = cpputils
