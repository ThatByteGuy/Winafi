// src/platform/linux/wue.h
#ifndef WINAFI_WUE_H
#define WINAFI_WUE_H

// Windows unattended-setup flags (UNATTEND_*-style bitmask).
// Each Windows 11 setup requirement bypass is an INDEPENDENT bit (Feature 1) so the
// user can enable/disable each one individually. Each maps to one LabConfig reg key.
#define WUE_BYPASS_TPM            0x0001  // LabConfig BypassTPMCheck
#define WUE_BYPASS_SECUREBOOT     0x0002  // LabConfig BypassSecureBootCheck
#define WUE_NO_ONLINE_ACCOUNT     0x0004  // OOBE: skip Microsoft account requirement
#define WUE_NO_DATA_COLLECTION    0x0008  // OOBE: disable diagnostic data
#define WUE_OFFLINE_DRIVES        0x0010  // offlineServicing: SanPolicy 4, offline internal fixed disks (WinToGo)
#define WUE_DUPLICATE_LOCALE      0x0020  // OOBE: match locale to ISO language
#define WUE_SET_USER              0x0040  // Create a local account with given username
#define WUE_DISABLE_BITLOCKER     0x0080  // Disable BitLocker during install
#define WUE_BYPASS_RAM            0x0100  // LabConfig BypassRAMCheck
#define WUE_BYPASS_CPU            0x0200  // LabConfig BypassCPUCheck
#define WUE_BYPASS_STORAGE        0x0400  // LabConfig BypassStorageCheck
#define WUE_SILENT_INSTALL        0x0800  // Suppress install UI (windowsPE pass)
#define WUE_QOL_ENHANCEMENTS      0x1000  // Disable suggested apps, Cortana, Teams, etc.
#define WUE_HIDE_INSTALL_MEDIA    0x2000  // windowsPE: mark installer USB read-only (always on for standard installs)

// Convenience mask: all five Windows 11 compatibility bypasses ("Apply all").
#define WUE_BYPASS_ALL  (WUE_BYPASS_TPM | WUE_BYPASS_SECUREBOOT | WUE_BYPASS_RAM | \
                         WUE_BYPASS_CPU | WUE_BYPASS_STORAGE)
// Mask of all bypass bits, for "does this flag set request any bypass?" checks.
#define WUE_BYPASS_MASK  WUE_BYPASS_ALL

typedef enum {
    WUE_ARCH_X86_32 = 0,
    WUE_ARCH_X86_64 = 1,
    WUE_ARCH_ARM_64 = 2,
} wue_arch_t;

/* Generate autounattend.xml content for the given flags.
 * username: local account name (used when WUE_SET_USER is set); may be NULL.
 * Returns heap-allocated XML string. Caller must free().
 * Returns NULL if flags == 0 (no customisation needed). */
char *wue_generate_xml(int flags, const char *username, wue_arch_t arch);

/* Write xml to <mount_point>/sources/$OEM$/$$/Panther/unattend.xml,
 * creating parent directories as needed. Used for $OEM$-style servicing.
 * Returns 0 on success, -1 on error. */
int wue_inject_xml(const char *xml, const char *mount_point);

/* Write xml to <mount_point>/autounattend.xml (the boot-media ROOT).
 * This is the location Windows Setup scans during the windowsPE pass, so it is
 * REQUIRED for the LabConfig requirement bypasses (TPM/SecureBoot/RAM/CPU/Storage)
 * to take effect. Returns 0 on success, -1 on error. */
int wue_inject_autounattend(const char *xml, const char *mount_point);

#endif
