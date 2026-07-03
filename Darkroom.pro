TEMPLATE = subdirs

SUBDIRS += app qtutils cpputils cpp-template-utils magic-alignment

qtutils.depends = cpputils
app.depends = qtutils magic-alignment
