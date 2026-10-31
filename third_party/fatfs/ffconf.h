/* FatFs R0.16 configuration for ProsenjitOS removable media.
 * FAT32 and exFAT are enabled; FAT12/FAT16 are rejected by the provider.
 * Keep FFCONF_DEF synchronized with the pinned upstream ff.h revision.
 */
#ifndef PROSENJIT_FATFS_FFCONF_H
#define PROSENJIT_FATFS_FFCONF_H
#define FFCONF_DEF 80386

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
#define FF_PRINT_LLI 0
#define FF_PRINT_FLOAT 0
#define FF_STRF_ENCODE 3

#define FF_CODE_PAGE 437
#define FF_USE_LFN 2
#define FF_MAX_LFN 255
#define FF_LFN_UNICODE 2
#define FF_LFN_BUF 765
#define FF_SFN_BUF 34
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

#define FF_FS_TINY 0
#define FF_FS_EXFAT 1
#define FF_FS_NORTC 1
#define FF_NORTC_MON 1
#define FF_NORTC_MDAY 1
#define FF_NORTC_YEAR 2026
#define FF_FS_CRTIME 0
/* Ignore untrusted FAT32 FSInfo free/next-cluster hints. */
#define FF_FS_NOFSINFO 3
#define FF_FS_LOCK 32
/* Provider adapter serializes all FatFs calls per volume. */
#define FF_FS_REENTRANT 0
#define FF_FS_TIMEOUT 1000

#endif
