TEMPLATE = subdirs

# The app and its libraries only - the test binary is a separate project, Tests.pro.
SUBDIRS += app qtutils cpputils cpp-template-utils magic-alignment

qtutils.depends = cpputils
app.depends = qtutils magic-alignment
