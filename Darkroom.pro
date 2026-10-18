TEMPLATE = subdirs

# The apps and their libraries only - the test binary is a separate project, Tests.pro.
SUBDIRS += Darkroom Quickroom Shared qtutils cpputils cpp-template-utils magic-alignment image-processing thin_io

qtutils.depends = cpputils
Darkroom.depends = Shared qtutils magic-alignment image-processing
Quickroom.depends = Shared qtutils image-processing thin_io
