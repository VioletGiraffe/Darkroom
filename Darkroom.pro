TEMPLATE = subdirs

# The apps and their libraries only - the test binary is a separate project, Tests.pro.
SUBDIRS += Darkroom Quickroom Shared qtutils cpputils cpp-template-utils magic-alignment image-processing thin_io
SUBDIRS += fileoperations fileoperationsui

fileoperations.subdir = file-operations/libraries/fileoperations
fileoperationsui.subdir = file-operations/libraries/fileoperationsui

qtutils.depends = cpputils
Darkroom.depends = Shared qtutils magic-alignment image-processing thin_io
Quickroom.depends = Shared qtutils magic-alignment image-processing thin_io fileoperations fileoperationsui
