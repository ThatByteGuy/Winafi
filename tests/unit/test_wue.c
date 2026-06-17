// tests/unit/test_wue.c
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "platform/linux/wue.h"

static void require_true(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static void test_generate_xml_bypass_tpm(void) {
    char *xml = wue_generate_xml(WUE_BYPASS_TPM | WUE_BYPASS_SECUREBOOT, NULL, WUE_ARCH_X86_64);
    assert(xml != NULL);
    assert(strstr(xml, "BypassTPMCheck") != NULL);
    assert(strstr(xml, "BypassSecureBootCheck") != NULL);
    assert(strstr(xml, "<unattend") != NULL);
    free(xml);
}

// Each bypass must be independently controllable (Feature 1).
static void test_generate_xml_individual_bypasses(void) {
    // TPM only: must emit BypassTPMCheck and NOT the others.
    char *xml = wue_generate_xml(WUE_BYPASS_TPM, NULL, WUE_ARCH_X86_64);
    assert(xml != NULL);
    assert(strstr(xml, "BypassTPMCheck") != NULL);
    assert(strstr(xml, "BypassSecureBootCheck") == NULL);
    assert(strstr(xml, "BypassRAMCheck") == NULL);
    assert(strstr(xml, "BypassCPUCheck") == NULL);
    assert(strstr(xml, "BypassStorageCheck") == NULL);
    free(xml);

    // CPU only: BypassCPUCheck present, TPM absent.
    xml = wue_generate_xml(WUE_BYPASS_CPU, NULL, WUE_ARCH_X86_64);
    assert(xml != NULL);
    assert(strstr(xml, "BypassCPUCheck") != NULL);
    assert(strstr(xml, "BypassTPMCheck") == NULL);
    free(xml);

    // Storage only: BypassStorageCheck present.
    xml = wue_generate_xml(WUE_BYPASS_STORAGE, NULL, WUE_ARCH_X86_64);
    assert(xml != NULL);
    assert(strstr(xml, "BypassStorageCheck") != NULL);
    free(xml);

    // All five: every key present.
    xml = wue_generate_xml(WUE_BYPASS_ALL, NULL, WUE_ARCH_X86_64);
    assert(xml != NULL);
    assert(strstr(xml, "BypassTPMCheck") != NULL);
    assert(strstr(xml, "BypassSecureBootCheck") != NULL);
    assert(strstr(xml, "BypassRAMCheck") != NULL);
    assert(strstr(xml, "BypassCPUCheck") != NULL);
    assert(strstr(xml, "BypassStorageCheck") != NULL);
    free(xml);
}

// The bypass autounattend must be placeable at the media ROOT, where Setup reads it.
static void test_inject_autounattend_root(void) {
    char tmpdir[] = "/tmp/wue_root_XXXXXX";
    assert(mkdtemp(tmpdir) != NULL);
    char *xml = wue_generate_xml(WUE_BYPASS_ALL, NULL, WUE_ARCH_X86_64);
    assert(xml != NULL);
    assert(wue_inject_autounattend(xml, tmpdir) == 0);
    free(xml);
    char expected[512];
    snprintf(expected, sizeof(expected), "%s/autounattend.xml", tmpdir);
    assert(access(expected, F_OK) == 0);
    char cleanup[512];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", tmpdir);
    system(cleanup);
}

static void test_generate_xml_no_online_account(void) {
    char *xml = wue_generate_xml(WUE_NO_ONLINE_ACCOUNT, NULL, WUE_ARCH_X86_64);
    assert(xml != NULL);
    assert(strstr(xml, "HideOnlineAccountScreens") != NULL || strstr(xml, "SkipMachineOOBE") != NULL);
    free(xml);
}

static void test_generate_xml_with_username(void) {
    char *xml = wue_generate_xml(WUE_SET_USER, "alice", WUE_ARCH_X86_64);
    assert(xml != NULL);
    assert(strstr(xml, "alice") != NULL);
    free(xml);
}

static void test_generate_xml_zero_flags_returns_null(void) {
    char *xml = wue_generate_xml(0, NULL, WUE_ARCH_X86_64);
    require_true(xml == NULL, "zero WUE flags should not generate XML");
}

static void test_hide_install_media_always_emits_windowspe(void) {
    char *xml = wue_generate_xml(WUE_HIDE_INSTALL_MEDIA, NULL, WUE_ARCH_X86_64);
    require_true(xml != NULL, "hide-install-media XML should be generated");
    require_true(strstr(xml, "pass=\"windowsPE\"") != NULL, "hide-install-media should run in windowsPE");
    require_true(strstr(xml, "RunSynchronous") != NULL, "hide-install-media should use RunSynchronous");
    require_true(strstr(xml, "Set-Disk") != NULL, "hide-install-media should mark disks read-only");
    require_true(strstr(xml, "install.wim") != NULL, "hide-install-media should detect installer volumes");
    require_true(strstr(xml, "SanPolicy") == NULL, "hide-install-media should not depend on SanPolicy");
    free(xml);
}

static void test_hide_install_media_runs_before_bypasses(void) {
    char *xml = wue_generate_xml(WUE_HIDE_INSTALL_MEDIA | WUE_BYPASS_TPM, NULL, WUE_ARCH_X86_64);
    require_true(xml != NULL, "combined hide-install-media and bypass XML should be generated");
    const char *hide = strstr(xml, "block Windows Setup from installing to the USB media");
    const char *bypass = strstr(xml, "BypassTPMCheck");
    require_true(hide != NULL && bypass != NULL, "hide-install-media and bypass commands should both exist");
    require_true(hide < bypass, "hide-install-media should run before bypass commands");
    free(xml);
}

static void test_inject_xml_to_mount(void) {
    char tmpdir[] = "/tmp/wue_test_XXXXXX";
    assert(mkdtemp(tmpdir) != NULL);

    char *xml = wue_generate_xml(WUE_BYPASS_TPM, NULL, WUE_ARCH_X86_64);
    assert(xml != NULL);
    int rc = wue_inject_xml(xml, tmpdir);
    require_true(rc == 0, "wue_inject_xml should succeed");
    free(xml);

    // Verify file was written to correct location
    char expected[512];
    snprintf(expected, sizeof(expected),
             "%s/sources/$OEM$/$$/Panther/unattend.xml", tmpdir);
    assert(access(expected, F_OK) == 0);

    // Cleanup: Remove temporary directory and contents
    char cleanup_cmd[512];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", tmpdir);
    system(cleanup_cmd);
}

int main(void) {
    test_generate_xml_bypass_tpm();
    test_generate_xml_individual_bypasses();
    test_inject_autounattend_root();
    test_generate_xml_no_online_account();
    test_generate_xml_with_username();
    test_generate_xml_zero_flags_returns_null();
    test_hide_install_media_always_emits_windowspe();
    test_hide_install_media_runs_before_bypasses();
    test_inject_xml_to_mount();
    printf("All wue tests passed\n");
    return 0;
}
