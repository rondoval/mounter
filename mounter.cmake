# SPDX-License-Identifier: BSD-2-Clause
# The mounter's sources, for a host to add to its target: include() this file,
# then list ${MOUNTER_SOURCES}; ${MOUNTER_C_SOURCES} is the C files alone, for
# per-file compile options. A host that serves virtual drives adds
# ${VDRIVE_SOURCES} as well, all C files.
set(MOUNTER_C_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/mounter.c
    ${CMAKE_CURRENT_LIST_DIR}/mounter_io.c
    ${CMAKE_CURRENT_LIST_DIR}/mounter_fs.c
    ${CMAKE_CURRENT_LIST_DIR}/mounter_volume.c
    ${CMAKE_CURRENT_LIST_DIR}/mounter_table.c
    ${CMAKE_CURRENT_LIST_DIR}/mounter_hunk.c
    ${CMAKE_CURRENT_LIST_DIR}/mounter_rdb.c
    ${CMAKE_CURRENT_LIST_DIR}/mounter_legacy.c
    ${CMAKE_CURRENT_LIST_DIR}/mounter_cd.c)
set(MOUNTER_SOURCES ${MOUNTER_C_SOURCES} ${CMAKE_CURRENT_LIST_DIR}/bootpoint.S)
set(VDRIVE_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/vdrive.c
    ${CMAKE_CURRENT_LIST_DIR}/vdrive_fence.c
    ${CMAKE_CURRENT_LIST_DIR}/vdrive_device.c
    ${CMAKE_CURRENT_LIST_DIR}/vdrive_forward.c
    ${CMAKE_CURRENT_LIST_DIR}/vdrive_scsi.c
    ${CMAKE_CURRENT_LIST_DIR}/vdrive_unit.c)
