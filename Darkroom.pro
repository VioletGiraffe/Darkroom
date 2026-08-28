TEMPLATE = subdirs

# The apps and their libraries only - the test binary is a separate project, Tests.pro.
SUBDIRS += app quickroom qtutils cpputils cpp-template-utils magic-alignment image-processing

qtutils.depends = cpputils
app.depends = qtutils magic-alignment image-processing
quickroom.depends = qtutils image-processing
