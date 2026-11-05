set(PEARE_NTFS_ROOT "${CMAKE_CURRENT_LIST_DIR}")
if(WIN32)
    set(PEARE_NTFS_CONFIG_DIR "${PEARE_NTFS_ROOT}/msvc")
else()
    set(PEARE_NTFS_CONFIG_DIR "${PEARE_NTFS_ROOT}/posix")
endif()
set(PEARE_NTFS_SOURCES
    libntfs-3g/acls.c
    libntfs-3g/attrib.c
    libntfs-3g/attrlist.c
    libntfs-3g/bitmap.c
    libntfs-3g/bootsect.c
    libntfs-3g/cache.c
    libntfs-3g/collate.c
    libntfs-3g/compat.c
    libntfs-3g/compress.c
    libntfs-3g/debug.c
    libntfs-3g/device.c
    libntfs-3g/dir.c
    libntfs-3g/ea.c
    libntfs-3g/efs.c
    libntfs-3g/index.c
    libntfs-3g/inode.c
    libntfs-3g/ioctl.c
    libntfs-3g/lcnalloc.c
    libntfs-3g/logfile.c
    libntfs-3g/logging.c
    libntfs-3g/mft.c
    libntfs-3g/misc.c
    libntfs-3g/mst.c
    libntfs-3g/object_id.c
    libntfs-3g/realpath.c
    libntfs-3g/reparse.c
    libntfs-3g/runlist.c
    libntfs-3g/security.c
    libntfs-3g/unistr.c
    libntfs-3g/volume.c
    libntfs-3g/xattrs.c
    libntfs-3g/peare_bridge.c
    ntfsprogs/attrdef.c
    ntfsprogs/boot.c
    ntfsprogs/mkntfs.c
    ntfsprogs/sd.c
    ntfsprogs/utils.c
)

list(TRANSFORM PEARE_NTFS_SOURCES PREPEND "${PEARE_NTFS_ROOT}/")
add_library(PeareNtfs3g STATIC ${PEARE_NTFS_SOURCES})
target_compile_definitions(PeareNtfs3g PRIVATE
    PEARE_NTFS_STATIC
    PEARE_NTFS_INTEGRATED
    HAVE_CONFIG_H
    NO_NTFS_DEVICE_DEFAULT_IO_OPS
)
if(WIN32)
    target_compile_definitions(PeareNtfs3g PRIVATE HAVE_WINDOWS_H WINDOWS _CRT_SECURE_NO_WARNINGS)
endif()
target_include_directories(PeareNtfs3g BEFORE PRIVATE
    "${PEARE_NTFS_CONFIG_DIR}"
    "${PEARE_NTFS_ROOT}/include"
    "${PEARE_NTFS_ROOT}/include/ntfs-3g"
    "${PEARE_NTFS_ROOT}/include/fuse-lite"
    "${PEARE_NTFS_ROOT}/libntfs-3g"
    "${PEARE_NTFS_ROOT}/ntfsprogs"
)
set_target_properties(PeareNtfs3g PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
if(MSVC)
    target_compile_options(PeareNtfs3g PRIVATE
        "/FI${PEARE_NTFS_ROOT}/msvc/peare_msvc_compat.h"
        /Zc:preprocessor
    )
else()
    target_compile_options(PeareNtfs3g PRIVATE
        "-include${PEARE_NTFS_ROOT}/include/ntfs-3g/compat.h"
    )
    target_compile_definitions(PeareNtfs3g PRIVATE __timespec_defined _DEFAULT_SOURCE)
endif()
target_link_libraries(PeareWriter PRIVATE PeareNtfs3g)
target_compile_definitions(PeareWriter PRIVATE PEARE_NTFS_WRITER)
