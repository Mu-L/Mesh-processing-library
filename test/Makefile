# Use make (GNU gmake), e.g.:
#  make CONFIG=win -j12
#  make clean Stack_test.ou

MeshRoot = ..

new_all: test

include $(MeshRoot)/make/Makefile_progs

# For assembly code, use ./test/opt/ instead.
# Map_test: cxxall += $(cxx_list_assembly_code)
# Vector4_test: cxxall += $(cxx_list_assembly_code)
# NetworkOrder_test: cxxall += $(cxx_list_assembly_code)

# Precompiled headers:
# - gcc and clang already benefit from "cxxall += -include $(MeshRoot)/libHh/precompiled_libHh.h" under
#    all $(MeshRoot) directories.
# - win is too difficult; it would not work with multiple executables in the same directory, each requiring its
#    own pdb file.
