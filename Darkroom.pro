TEMPLATE = subdirs

SUBDIRS += app qtutils cpputils cpp-template-utils

qtutils.depends = cpputils
app.depends = qtutils
