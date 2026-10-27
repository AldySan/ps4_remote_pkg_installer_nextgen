/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * Host unit tests for the portable core (util / sfo / pkg).
 * Built and run with the host C compiler (see `make test` in the Makefile
 * and the CI workflow). Fixtures live in tests/data/.
 */
#include "pkg.h"
#include "sfo.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            ++s_failures;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_STR(got, want)                                                 \
    do {                                                                     \
        if (strcmp((got), (want)) != 0) {                                    \
            printf("FAIL %s:%d: got \"%s\", want \"%s\"\n", __FILE__,        \
                   __LINE__, (got), (want));                                 \
            ++s_failures;                                                    \
        }                                                                    \
    } while (0)

static uint8_t* load_file(const char* path, size_t* out_size)
{
    FILE* f = fopen(path, "rb");
    uint8_t* buf;
    long size;

    if (!f) {
        printf("FAIL cannot open %s\n", path);
        ++s_failures;
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return NULL;
    }
    buf = (uint8_t*)malloc((size_t)size);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (size_t)size;
    return buf;
}

static void test_util(void)
{
    char buf[64];

    util_format_size(0, buf, sizeof(buf));
    CHECK_STR(buf, "0 B");
    util_format_size(512, buf, sizeof(buf));
    CHECK_STR(buf, "512 B");
    util_format_size(1024, buf, sizeof(buf));
    CHECK_STR(buf, "1.00 KB");
    util_format_size((uint64_t)13 * 1024 * 1024, buf, sizeof(buf));
    CHECK_STR(buf, "13.00 MB");
    util_format_size((uint64_t)50 * 1024 * 1024 * 1024, buf, sizeof(buf));
    CHECK_STR(buf, "50.00 GB");

    {
        uint8_t data[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
        util_hex_encode(data, sizeof(data), buf, sizeof(buf));
        CHECK_STR(buf, "deadbeef");
    }

    CHECK(util_starts_with("http://x", "http://"));
    CHECK(!util_starts_with("xhttp://", "http://"));

    {
        uint8_t be[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
        CHECK(util_be16(be) == 0x0102);
        CHECK(util_be32(be) == 0x01020304u);
        CHECK(util_be64(be) == 0x0102030405060708ull);
    }
}

static void test_sfo(const char* fixture_dir)
{
    char path[512];
    uint8_t* data;
    size_t size = 0;
    char value[256];
    uint32_t u32 = 0;

    snprintf(path, sizeof(path), "%s/param.sfo", fixture_dir);
    data = load_file(path, &size);
    if (!data) {
        return;
    }

    CHECK(sfo_get_string(data, size, "TITLE", value, sizeof(value)));
    CHECK(strlen(value) > 0);
    printf("  sfo TITLE = \"%s\"\n", value);

    CHECK(sfo_get_string(data, size, "TITLE_ID", value, sizeof(value)));
    CHECK_STR(value, "RPIN00001");

    CHECK(sfo_get_string(data, size, "CONTENT_ID", value, sizeof(value)));
    CHECK_STR(value, "IV0000-RPIN00001_00-RPINEXTGEN000000");

    CHECK(sfo_get_string(data, size, "CATEGORY", value, sizeof(value)));
    CHECK_STR(value, "gde");

    CHECK(sfo_get_u32(data, size, "ATTRIBUTE", &u32));
    CHECK(u32 == 32);

    CHECK(!sfo_get_string(data, size, "NO_SUCH_KEY", value, sizeof(value)));

    /* Localized title fallback: no TITLE_XX entries in the fixture. */
    CHECK(sfo_get_title(data, size, 1, value, sizeof(value)));
    CHECK(strlen(value) > 0);

    free(data);
}

static void test_pkg(const char* fixture_dir)
{
    char path[512];
    uint8_t* data;
    size_t size = 0;
    pkg_header_info_t hdr;
    pkg_entry_t entry;

    snprintf(path, sizeof(path), "%s/pkg_header.bin", fixture_dir);
    data = load_file(path, &size);
    if (!data) {
        return;
    }

    CHECK(pkg_parse_header(data, size, &hdr));
    CHECK_STR(hdr.content_id, "IV0000-RPIN00001_00-RPINEXTGEN000000");
    CHECK(hdr.package_size > PKG_HEADER_SIZE);
    CHECK(hdr.package_size >= size);
    CHECK(hdr.entry_count > 0);
    CHECK(hdr.entry_table_offset >= PKG_HEADER_SIZE);
    printf("  pkg content_type=0x%02X flags=0x%08X size=%llu entries=%u\n",
           hdr.content_type, hdr.content_flags,
           (unsigned long long)hdr.package_size, hdr.entry_count);

    CHECK(pkg_is_supported_content_type(hdr.content_type));
    CHECK(!pkg_is_patch(hdr.content_flags));
    CHECK(strlen(pkg_content_type_name(hdr.content_type)) == 2);

    /* The param.sfo entry must be present with a sane size. */
    CHECK(pkg_find_entry(data, size, PKG_ENTRY_ID_PARAM_SFO, &entry));
    CHECK(entry.id == PKG_ENTRY_ID_PARAM_SFO);
    CHECK(entry.size > 0);
    CHECK(entry.size < 0x10000);
    printf("  pkg PARAM_SFO entry: offset=%llu size=%llu\n",
           (unsigned long long)entry.offset, (unsigned long long)entry.size);

    CHECK(pkg_find_entry(data, size, PKG_ENTRY_ID_ICON0_PNG, &entry));
    CHECK(entry.size > 0);

    CHECK(!pkg_find_entry(data, size, 0xDEAD, &entry));

    /* Malformed inputs must be rejected, not crash. */
    {
        pkg_header_info_t bad;
        CHECK(!pkg_parse_header("short", 5, &bad));
        CHECK(!pkg_parse_header(data, PKG_HEADER_SIZE - 1, &bad));
    }

    free(data);
}

static void test_content_id(void)
{
    pkg_content_id_info_t info;

    CHECK(pkg_parse_content_id("IV0000-RPIN00001_00-RPINEXTGEN000000", &info));
    CHECK_STR(info.title_id, "RPIN00001");
    CHECK_STR(info.service_id, "IV0000-RPIN00001_00");
    CHECK_STR(info.label, "RPINEXTGEN000000");

    CHECK(pkg_parse_content_id("UP0001-CUSA09311_00-GAME000000000000", &info));
    CHECK_STR(info.title_id, "CUSA09311");

    CHECK(!pkg_parse_content_id("garbage", &info));
    CHECK(!pkg_parse_content_id("IV0000-RPIN00001_00", &info));
    CHECK(!pkg_parse_content_id("IV0000-RPIN00001-00-RPINEXTGEN0000000", &info));
}

int main(int argc, char** argv)
{
    const char* fixture_dir = (argc > 1) ? argv[1] : "tests/data";

    printf("== util ==\n");
    test_util();
    printf("== sfo (%s) ==\n", fixture_dir);
    test_sfo(fixture_dir);
    printf("== pkg (%s) ==\n", fixture_dir);
    test_pkg(fixture_dir);
    printf("== content id ==\n");
    test_content_id();

    if (s_failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", s_failures);
    return 1;
}
