/* ProsenjitOS FatFs R0.16 configuration.
 * FAT32 + exFAT only at the ProsenjitOS provider boundary.
 * All FatFs API calls are serialized by fatfs_enter(); static LFN workspaces
 * therefore avoid consuming >1 KiB of kernel stack per filesystem operation.
 * Do not enable formatting (FF_USE_MKFS remains disabled).
 */
#define FFCONF_DEF 80386

/* FATFS.win is CPU-accessed cached filesystem metadata, not a DMA buffer.
 * Keep the window naturally aligned for the x86-64 kernel's 64-bit accesses.
 * This setting is consumed by the small ProsenjitOS alignment patch in ff.h.
 */
#define FS_FATFS_WINDOW_ALIGNMENT 16

#define FF_FS_READONLY 0
#define FF_FS_MINIMIZE 0
#define FF_USE_FIND 0
#define FF_USE_MKFS 0
#define FF_USE_FASTSEEK 0
#define FF_USE_EXPAND 0
#define FF_USE_CHMOD 0
#define FF_USE_LABEL 1
#define FF_USE_FORWARD 0
#define FF_USE_STRFUNC 0
#define FF_CODE_PAGE 437
#define FF_USE_LFN 1
#define FF_MAX_LFN 255
#define FF_LFN_UNICODE 2
#define FF_LFN_BUF 1020
#define FF_SFN_BUF 12
#define FF_FS_RPATH 0
#define FF_PATH_DEPTH 10
#define FF_VOLUMES 8
#define FF_STR_VOLUME_ID 0
#define FF_MULTI_PARTITION 0
#define FF_MIN_SS 512
#define FF_MAX_SS 4096
#define FF_LBA64 1
#define FF_MIN_GPT 0x10000000
#define FF_USE_TRIM 0
#define FF_FS_TINY 1
#define FF_FS_EXFAT 1
#define FF_FS_NORTC 1
#define FF_NORTC_MON 1
#define FF_NORTC_MDAY 1
#define FF_NORTC_YEAR 2026
#define FF_FS_CRTIME 0
#define FF_FS_NOFSINFO 3
#define FF_FS_LOCK 32
#define FF_FS_REENTRANT 0
#define FF_FS_TIMEOUT 1000
