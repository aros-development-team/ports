set(AROS 1)
set(AMIGA 1)
set(UNIX 1)
# Channel providers are linked statically; AROS has no libdl.
set(CMAKE_DL_LIBS "")
set(CMAKE_SYSTEM_PREFIX_PATH /)
set(CMAKE_SHARED_LIBRARY_SUPPORTED FALSE)
