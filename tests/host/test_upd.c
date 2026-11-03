/* upd_*: the manifest decision (and the shared manifest vectors), the daily policy and the person's choices,
 * and the download over a fake transport into a firmware sink. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "epager/ota.h"
#include "epager/upd.h"
#include "test_support.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* ---------------------------------------------------------------- manifest */

#define SLOT_BIG 0x300000u
#define SHA_A "8ba345e0c909a3dee48a57ab059c88668f4562ca4e3d02e458072cf61cc2bbbe"

static upd_board_t paper(const char *running)
{
    return (upd_board_t){.board_id = "waveshare_s3_epaper_154_v2", .variant = PROTO_VARIANT_PAPER,
                         .running_version = running, .contract_version = "0.14.0", .slot_size = SLOT_BIG};
}

static upd_verdict_t decide_text(const char *json, const upd_board_t *b, upd_image_t *img)
{
    return upd_decide(json, strlen(json), b, img);
}

static const char *manifest_text(const char *version, const char *contract, const char *size)
{
    static char buf[1024];
    snprintf(buf, sizeof(buf),
             "{\"manifest_version\":1,\"version\":\"%s\",\"contract_version\":\"%s\",\"images\":["
             "{\"board_id\":\"waveshare_s3_epaper_154_v2\",\"variant\":\"paper\","
             "\"file\":\"epager-waveshare_s3_epaper_154_v2-%s.bin\",\"size\":%s,\"sha256\":\"" SHA_A "\"},"
             "{\"board_id\":\"ostb_xiaozhi_3st\",\"variant\":\"lcd\","
             "\"file\":\"epager-ostb_xiaozhi_3st-%s.bin\",\"size\":2822144,\"sha256\":\"" SHA_A "\"}],"
             "\"notes_url\":\"https://example.invalid/n\"}",
             version, contract, version, size, version);
    return buf;
}

static void test_a_newer_release_is_offered(void)
{
    upd_board_t b = paper("0.1.1");
    upd_image_t img;
    TEST_ASSERT_EQUAL_INT(UPD_AVAILABLE, decide_text(manifest_text("0.2.0", "0.14.0", "1511424"), &b, &img));
    TEST_ASSERT_EQUAL_STRING("0.2.0", img.version);
    TEST_ASSERT_EQUAL_STRING("epager-waveshare_s3_epaper_154_v2-0.2.0.bin", img.file);
    TEST_ASSERT_EQUAL_UINT32(1511424, img.size);
    TEST_ASSERT_EQUAL_UINT8(0x8b, img.sha256[0]);
    TEST_ASSERT_EQUAL_UINT8(0xbe, img.sha256[31]);
}

static void test_the_image_of_this_board_is_picked(void)
{
    upd_board_t b = {.board_id = "ostb_xiaozhi_3st", .variant = PROTO_VARIANT_LCD, .running_version = "0.1.1",
                     .contract_version = "0.14.0", .slot_size = SLOT_BIG};
    upd_image_t img;
    TEST_ASSERT_EQUAL_INT(UPD_AVAILABLE, decide_text(manifest_text("0.2.0", "0.14.0", "1511424"), &b, &img));
    TEST_ASSERT_EQUAL_STRING("epager-ostb_xiaozhi_3st-0.2.0.bin", img.file);
    TEST_ASSERT_EQUAL_UINT32(2822144, img.size);
}

static void test_up_to_date_and_older_releases(void)
{
    upd_board_t b = paper("0.2.0");
    upd_image_t img;
    TEST_ASSERT_EQUAL_INT(UPD_UP_TO_DATE, decide_text(manifest_text("0.2.0", "0.14.0", "100"), &b, &img));
    TEST_ASSERT_EQUAL_STRING("0.2.0", img.version);
    TEST_ASSERT_EQUAL_INT(UPD_UP_TO_DATE, decide_text(manifest_text("0.1.9", "0.14.0", "100"), &b, &img));
    TEST_ASSERT_EQUAL_INT(UPD_AVAILABLE, decide_text(manifest_text("0.2.1", "0.14.0", "100"), &b, &img));
    b.running_version = "0.3.0-rc.1"; /* a pre-release is older than its release */
    TEST_ASSERT_EQUAL_INT(UPD_AVAILABLE, decide_text(manifest_text("0.3.0", "0.14.0", "100"), &b, &img));
}

static void test_refusals_in_order(void)
{
    upd_board_t b = paper("0.1.1");
    upd_image_t img;
    /* another board only */
    b.board_id = "some_other_board";
    TEST_ASSERT_EQUAL_INT(UPD_NO_IMAGE, decide_text(manifest_text("0.2.0", "0.14.0", "100"), &b, &img));
    /* this board_id, another variant */
    b = paper("0.1.1");
    b.variant = PROTO_VARIANT_LCD;
    TEST_ASSERT_EQUAL_INT(UPD_VARIANT_MISMATCH, decide_text(manifest_text("0.2.0", "0.14.0", "100"), &b, &img));
    /* too large for the slot (and reported before the contract major) */
    b = paper("0.1.1");
    b.slot_size = 1000;
    TEST_ASSERT_EQUAL_INT(UPD_TOO_LARGE, decide_text(manifest_text("1.0.0", "1.0.0", "1001"), &b, &img));
    TEST_ASSERT_EQUAL_UINT32(1001, img.size);
    b.slot_size = 1001; /* exactly the slot fits */
    TEST_ASSERT_EQUAL_INT(UPD_CONTRACT_MAJOR, decide_text(manifest_text("1.0.0", "1.0.0", "1001"), &b, &img));
    TEST_ASSERT_EQUAL_STRING("1.0.0", img.version);
    /* a pre-release is not offered unless allowed */
    b = paper("0.1.1");
    TEST_ASSERT_EQUAL_INT(UPD_PRERELEASE, decide_text(manifest_text("0.2.0-rc.1", "0.14.0", "100"), &b, &img));
    b.allow_prerelease = true;
    TEST_ASSERT_EQUAL_INT(UPD_AVAILABLE, decide_text(manifest_text("0.2.0-rc.1", "0.14.0", "100"), &b, &img));
    /* while the contract is 0.x the major is 0 on both sides */
    b = paper("0.1.1");
    TEST_ASSERT_EQUAL_INT(UPD_AVAILABLE, decide_text(manifest_text("0.2.0", "0.7.0", "100"), &b, &img));
}

static void test_broken_manifests_are_invalid(void)
{
    upd_board_t b = paper("0.1.1");
    upd_image_t img;
    static const char *const bad[] = {
        "", "{", "[]", "null", "{}",
        "{\"manifest_version\":1,\"version\":\"0.2.0\",\"contract_version\":\"0.14.0\",\"images\":[",      /* cut off */
        "{\"manifest_version\":1,\"version\":\"0.2.0\",\"contract_version\":\"0.14.0\",\"images\":[]}",
        "{\"manifest_version\":\"1\",\"version\":\"0.2.0\",\"contract_version\":\"0.14.0\",\"images\":[]}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(UPD_INVALID, decide_text(bad[i], &b, &img), bad[i]);
    }
    /* a version of 33 characters does not fit and is not silently cut */
    char longv[64];
    snprintf(longv, sizeof(longv), "0.2.0-%s", "abcdefghijklmnopqrstuvwxyz0");
    TEST_ASSERT_EQUAL_INT(UPD_INVALID, decide_text(manifest_text(longv, "0.14.0", "100"), &b, &img));
    /* the size is a JSON integer from 1 to 2^32-1 */
    TEST_ASSERT_EQUAL_INT(UPD_INVALID, decide_text(manifest_text("0.2.0", "0.14.0", "0"), &b, &img));
    TEST_ASSERT_EQUAL_INT(UPD_INVALID, decide_text(manifest_text("0.2.0", "0.14.0", "4294967296"), &b, &img));
    TEST_ASSERT_EQUAL_INT(UPD_INVALID, decide_text(manifest_text("0.2.0", "0.14.0", "\"100\""), &b, &img));
    TEST_ASSERT_EQUAL_INT(UPD_INVALID, decide_text(manifest_text("0.2.0", "0.14.0", "1.5"), &b, &img));
    TEST_ASSERT_EQUAL_INT(UPD_INVALID, decide_text(manifest_text("0.2.0", "0.14.0", "007"), &b, &img));
    b.slot_size = 0xFFFFFFFFu; /* the largest size allowed is still valid */
    TEST_ASSERT_EQUAL_INT(UPD_AVAILABLE, decide_text(manifest_text("0.2.0", "0.14.0", "4294967295"), &b, &img));
}

/* contracts/test-vectors/releases.json `manifests`: valid ones are never INVALID, the rest always are. */
static void test_shared_manifest_vectors(void)
{
    cJSON *v = load_vectors("releases.json");
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(v, "manifests");
    TEST_ASSERT_TRUE(cJSON_IsArray(list));
    const cJSON *c;
    int n = 0;
    cJSON_ArrayForEach(c, list)
    {
        const char *name = jstr(c, "name");
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(c, "manifest");
        const bool valid = cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(c, "problem"));
        char *text = cJSON_PrintUnformatted(m);
        upd_board_t b = paper("0.0.1");
        b.allow_prerelease = true;
        upd_image_t img;
        const upd_verdict_t r = decide_text(text, &b, &img);
        if (valid == (r == UPD_INVALID)) {
            CASE_FAIL(name, "verdict %d for a %s manifest", (int)r, valid ? "valid" : "invalid");
        }
        free(text);
        n++;
    }
    TEST_ASSERT_TRUE(n >= 20);
    TEST_ASSERT_EQUAL_INT(0, g_case_failures);
    cJSON_Delete(v);
}

static void test_only_github_hosts_are_allowed(void)
{
    TEST_ASSERT_TRUE(upd_url_allowed("https://github.com/tdpi95/epager-firmware/releases/latest/download/manifest.json"));
    TEST_ASSERT_TRUE(upd_url_allowed("https://GitHub.com/x"));
    TEST_ASSERT_TRUE(upd_url_allowed("https://github.com"));
    TEST_ASSERT_TRUE(upd_url_allowed("https://github.com:443/x"));
    TEST_ASSERT_TRUE(upd_url_allowed("https://release-assets.githubusercontent.com/github-production-release-asset/1?sp=r"));
    TEST_ASSERT_TRUE(upd_url_allowed("https://objects.githubusercontent.com/x"));
    TEST_ASSERT_FALSE(upd_url_allowed(NULL));
    TEST_ASSERT_FALSE(upd_url_allowed(""));
    TEST_ASSERT_FALSE(upd_url_allowed("http://github.com/x"));                 /* not https */
    TEST_ASSERT_FALSE(upd_url_allowed("/relative/path"));
    TEST_ASSERT_FALSE(upd_url_allowed("https://github.com@evil.example/x"));   /* a user name */
    TEST_ASSERT_FALSE(upd_url_allowed("https://github.com:8443/x"));
    TEST_ASSERT_FALSE(upd_url_allowed("https://evil-github.com/x"));
    TEST_ASSERT_FALSE(upd_url_allowed("https://github.com.evil.example/x"));
    TEST_ASSERT_FALSE(upd_url_allowed("https://evilgithubusercontent.com/x")); /* no dot before the name */
    TEST_ASSERT_FALSE(upd_url_allowed("https://githubusercontent.com/x"));     /* the bare domain is not listed */
    TEST_ASSERT_FALSE(upd_url_allowed("https://x.githubusercontent.com.evil.example/x"));
    TEST_ASSERT_FALSE(upd_url_allowed("https://x.githubusercontent.com\\@evil.example/x"));
}

static void test_the_image_url_follows_the_manifest_url(void)
{
    char u[200];
    TEST_ASSERT_TRUE(upd_image_url("https://github.com/o/r/releases/latest/download/manifest.json", "0.2.0",
                                   "epager-b-0.2.0.bin", u, sizeof(u)));
    TEST_ASSERT_EQUAL_STRING("https://github.com/o/r/releases/download/v0.2.0/epager-b-0.2.0.bin", u);
    TEST_ASSERT_TRUE(upd_image_url("https://github.com/o/r/releases/download/v0.2.1-rc1/manifest.json", "0.2.1-rc1",
                                   "epager-b-0.2.1-rc1.bin", u, sizeof(u)));
    TEST_ASSERT_EQUAL_STRING("https://github.com/o/r/releases/download/v0.2.1-rc1/epager-b-0.2.1-rc1.bin", u);
    TEST_ASSERT_FALSE(upd_image_url("https://github.com/o/r/releases/latest", "0.2.0", "f.bin", u, sizeof(u)));
    TEST_ASSERT_FALSE(upd_image_url("https://github.com/o/r/xmanifest.json", "0.2.0", "f.bin", u, sizeof(u)));
    TEST_ASSERT_FALSE(upd_image_url("https://github.com/o/r/releases/latest/download/manifest.json", "0.2.0",
                                    "epager-b-0.2.0.bin", u, 20)); /* does not fit */
}

/* ---------------------------------------------------------------- policy */

#define DAY 86400u
#define T0 1800000000u /* a UTC time in 2027 */

static kv_mem_t kv;
static upd_policy_t pol;

static upd_policy_params_t pol_params(const char *running)
{
    return (upd_policy_params_t){.kv = &kv.iface, .running_version = running, .jitter_s = 600, .period_s = DAY,
                                 .retry_s = 3600, .max_tries = 3, .first_delay_s = 3600, .snooze_s = DAY};
}

static void pol_boot(const char *running, bool auto_check)
{
    upd_policy_params_t p = pol_params(running);
    upd_policy_init(&pol, &p, auto_check);
}

static void pol_fresh(const char *running)
{
    kv_mem_init(&kv);
    pol_boot(running, true);
}

static void test_first_check_waits_for_clock_and_boot_delay(void)
{
    pol_fresh("0.1.1");
    TEST_ASSERT_FALSE(upd_policy_check_due(&pol, 0, 99999));   /* no clock */
    TEST_ASSERT_FALSE(upd_policy_check_due(&pol, T0, 3599));   /* an hour after boot at the earliest */
    TEST_ASSERT_TRUE(upd_policy_check_due(&pol, T0, 3600));
}

static void test_one_check_a_day_with_jitter(void)
{
    pol_fresh("0.1.1");
    upd_policy_checked(&pol, T0, UPD_UP_TO_DATE, "0.1.1");
    TEST_ASSERT_FALSE(upd_policy_check_due(&pol, T0 + 1, 99999));
    TEST_ASSERT_FALSE(upd_policy_check_due(&pol, T0 + DAY + 599, 99999)); /* period + jitter */
    TEST_ASSERT_TRUE(upd_policy_check_due(&pol, T0 + DAY + 600, 99999));
    TEST_ASSERT_TRUE(upd_policy_check_due(&pol, T0 - 5, 99999)); /* the clock went back: due */
}

static void test_auto_check_off_stops_the_daily_check(void)
{
    pol_fresh("0.1.1");
    upd_policy_set_auto_check(&pol, false);
    TEST_ASSERT_FALSE(upd_policy_check_due(&pol, T0, 99999));
    upd_policy_set_auto_check(&pol, true);
    TEST_ASSERT_TRUE(upd_policy_check_due(&pol, T0, 99999));
}

static void test_failed_checks_retry_hourly_then_wait_a_day(void)
{
    pol_fresh("0.1.1");
    upd_policy_checked(&pol, T0, UPD_UP_TO_DATE, "0.1.1");
    uint32_t t = T0 + DAY + 600;
    TEST_ASSERT_TRUE(upd_policy_check_due(&pol, t, 99999));
    upd_policy_check_failed(&pol, t); /* 1 */
    TEST_ASSERT_FALSE(upd_policy_check_due(&pol, t + 3599, 99999));
    TEST_ASSERT_TRUE(upd_policy_check_due(&pol, t + 3600, 99999));
    t += 3600;
    upd_policy_check_failed(&pol, t); /* 2 */
    t += 3600;
    TEST_ASSERT_TRUE(upd_policy_check_due(&pol, t, 99999));
    upd_policy_check_failed(&pol, t); /* 3 = max_tries: now a whole day */
    TEST_ASSERT_FALSE(upd_policy_check_due(&pol, t + DAY - 1, 99999));
    TEST_ASSERT_TRUE(upd_policy_check_due(&pol, t + DAY, 99999));
    upd_policy_checked(&pol, t + DAY, UPD_UP_TO_DATE, "0.1.1"); /* a good check ends the streak */
    TEST_ASSERT_FALSE(upd_policy_check_due(&pol, t + DAY + 3600, 99999));
}

static void test_an_available_release_marks_and_prompts_once(void)
{
    pol_fresh("0.1.1");
    TEST_ASSERT_FALSE(upd_policy_mark(&pol));
    upd_policy_checked(&pol, T0, UPD_AVAILABLE, "0.2.0");
    TEST_ASSERT_EQUAL_STRING("0.2.0", upd_policy_available(&pol));
    TEST_ASSERT_TRUE(upd_policy_mark(&pol));
    TEST_ASSERT_TRUE(upd_policy_prompt_due(&pol, T0));
    upd_policy_prompt_shown(&pol, T0 + 10); /* left unanswered: counts as "remind later" */
    TEST_ASSERT_FALSE(upd_policy_prompt_due(&pol, T0 + 11));
    TEST_ASSERT_TRUE(upd_policy_mark(&pol)); /* the mark stays */
    TEST_ASSERT_FALSE(upd_policy_prompt_due(&pol, T0 + 10 + DAY - 1));
    TEST_ASSERT_TRUE(upd_policy_prompt_due(&pol, T0 + 10 + DAY));
}

static void test_remind_later_hides_the_prompt_for_a_day(void)
{
    pol_fresh("0.1.1");
    upd_policy_checked(&pol, T0, UPD_AVAILABLE, "0.2.0");
    upd_policy_remind_later(&pol, T0 + 100);
    TEST_ASSERT_FALSE(upd_policy_prompt_due(&pol, T0 + 101));
    TEST_ASSERT_TRUE(upd_policy_prompt_due(&pol, T0 + 100 + DAY));
    /* the next daily check finds the same release: the snooze is not reset */
    upd_policy_checked(&pol, T0 + DAY, UPD_AVAILABLE, "0.2.0");
    TEST_ASSERT_FALSE(upd_policy_prompt_due(&pol, T0 + DAY + 1) && T0 + DAY + 1 < T0 + 100 + DAY);
    TEST_ASSERT_TRUE(upd_policy_prompt_due(&pol, T0 + 100 + DAY));
    /* the clock jumped back by more than the snooze: it does not hide the prompt for ever */
    TEST_ASSERT_TRUE(upd_policy_prompt_due(&pol, T0 - 2 * DAY));
}

static void test_remind_later_without_a_clock_starts_at_the_next_check(void)
{
    pol_fresh("0.1.1");
    upd_policy_checked(&pol, T0, UPD_AVAILABLE, "0.2.0");
    upd_policy_remind_later(&pol, 0); /* the clock is not set */
    TEST_ASSERT_FALSE(upd_policy_prompt_due(&pol, 0));
    TEST_ASSERT_FALSE(upd_policy_prompt_due(&pol, T0 + 3 * DAY)); /* not before a check had a clock */
    upd_policy_checked(&pol, T0 + 3 * DAY, UPD_AVAILABLE, "0.2.0");
    TEST_ASSERT_FALSE(upd_policy_prompt_due(&pol, T0 + 3 * DAY + 1));
    TEST_ASSERT_TRUE(upd_policy_prompt_due(&pol, T0 + 4 * DAY));
}

static void test_prompt_needs_no_clock_before_it_was_shown(void)
{
    pol_fresh("0.1.1");
    upd_policy_checked(&pol, T0, UPD_AVAILABLE, "0.2.0");
    TEST_ASSERT_TRUE(upd_policy_prompt_due(&pol, 0));
}

static void test_skip_hides_one_version_and_survives_a_restart(void)
{
    pol_fresh("0.1.1");
    upd_policy_checked(&pol, T0, UPD_AVAILABLE, "0.2.0");
    upd_policy_skip(&pol);
    TEST_ASSERT_TRUE(upd_policy_skipped(&pol));
    TEST_ASSERT_FALSE(upd_policy_mark(&pol));
    TEST_ASSERT_FALSE(upd_policy_prompt_due(&pol, T0 + 10 * DAY));
    TEST_ASSERT_EQUAL_STRING("0.2.0", upd_policy_available(&pol)); /* still known for the page */
    pol_boot("0.1.1", true); /* restart: the NVS record */
    TEST_ASSERT_TRUE(upd_policy_skipped(&pol));
    upd_policy_checked(&pol, T0 + DAY, UPD_AVAILABLE, "0.2.0"); /* the next daily check: still skipped */
    TEST_ASSERT_FALSE(upd_policy_mark(&pol));
    upd_policy_checked(&pol, T0 + 2 * DAY, UPD_AVAILABLE, "0.2.1"); /* a newer release is offered again */
    TEST_ASSERT_FALSE(upd_policy_skipped(&pol));
    TEST_ASSERT_TRUE(upd_policy_mark(&pol));
    TEST_ASSERT_TRUE(upd_policy_prompt_due(&pol, T0 + 2 * DAY));
}

static void test_a_new_release_is_news_even_during_a_snooze(void)
{
    pol_fresh("0.1.1");
    upd_policy_checked(&pol, T0, UPD_AVAILABLE, "0.2.0");
    upd_policy_remind_later(&pol, T0);
    upd_policy_checked(&pol, T0 + DAY / 2, UPD_AVAILABLE, "0.2.1");
    TEST_ASSERT_TRUE(upd_policy_prompt_due(&pol, T0 + DAY / 2));
}

static void test_the_record_survives_a_restart(void)
{
    pol_fresh("0.1.1");
    upd_policy_checked(&pol, T0, UPD_AVAILABLE, "0.2.0");
    upd_policy_remind_later(&pol, T0 + 5);
    pol_boot("0.1.1", true);
    TEST_ASSERT_EQUAL_STRING("0.2.0", upd_policy_available(&pol));
    TEST_ASSERT_TRUE(upd_policy_mark(&pol)); /* the mark shows right after boot, without Wi-Fi */
    TEST_ASSERT_FALSE(upd_policy_prompt_due(&pol, T0 + 6));
    TEST_ASSERT_FALSE(upd_policy_check_due(&pol, T0 + 7, 99999)); /* the day counts across the restart */
}

static void test_up_to_date_and_other_answers_clear_the_offer(void)
{
    pol_fresh("0.1.1");
    upd_policy_checked(&pol, T0, UPD_AVAILABLE, "0.2.0");
    upd_policy_checked(&pol, T0 + DAY, UPD_UP_TO_DATE, "0.1.1"); /* the release was withdrawn */
    TEST_ASSERT_FALSE(upd_policy_mark(&pol));
    upd_policy_checked(&pol, T0 + 2 * DAY, UPD_AVAILABLE, "0.2.0");
    upd_policy_checked(&pol, T0 + 3 * DAY, UPD_CONTRACT_MAJOR, "1.0.0");
    TEST_ASSERT_FALSE(upd_policy_mark(&pol));
    TEST_ASSERT_EQUAL_INT(UPD_CONTRACT_MAJOR, pol.last_verdict);
}

static void test_an_installed_version_is_forgotten(void)
{
    pol_fresh("0.1.1");
    upd_policy_checked(&pol, T0, UPD_AVAILABLE, "0.2.0");
    upd_policy_skip(&pol);
    pol_boot("0.2.0", true); /* updated by another way (BLE, USB): not newer any more */
    TEST_ASSERT_EQUAL_STRING("", upd_policy_available(&pol));
    TEST_ASSERT_FALSE(upd_policy_skipped(&pol));
    TEST_ASSERT_FALSE(upd_policy_mark(&pol));
    /* a skip of a version that is still ahead is kept */
    pol_fresh("0.1.1");
    upd_policy_checked(&pol, T0, UPD_AVAILABLE, "0.3.0");
    upd_policy_skip(&pol);
    pol_boot("0.2.0", true);
    TEST_ASSERT_TRUE(upd_policy_skipped(&pol));
}

static void test_installed_clears_the_record(void)
{
    pol_fresh("0.1.1");
    upd_policy_checked(&pol, T0, UPD_AVAILABLE, "0.2.0");
    upd_policy_installed(&pol);
    pol_boot("0.1.1", true);
    TEST_ASSERT_FALSE(upd_policy_mark(&pol));
    TEST_ASSERT_EQUAL_STRING("", upd_policy_available(&pol));
}

static void test_a_damaged_record_is_ignored(void)
{
    pol_fresh("0.1.1");
    const uint8_t junk[] = {9, 1, 2, 3};
    TEST_ASSERT_EQUAL_INT(KV_OK, kv.iface.set(kv.iface.ctx, UPD_RECORD_KEY, junk, sizeof(junk)));
    pol_boot("0.1.1", true);
    TEST_ASSERT_EQUAL_STRING("", upd_policy_available(&pol));
    TEST_ASSERT_TRUE(upd_policy_check_due(&pol, T0, 99999));
    /* a version length that runs past the record */
    upd_policy_checked(&pol, T0, UPD_AVAILABLE, "0.2.0");
    uint8_t rec[80];
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(KV_OK, kv.iface.get(kv.iface.ctx, UPD_RECORD_KEY, rec, sizeof(rec), &n));
    rec[14] = 200;
    TEST_ASSERT_EQUAL_INT(KV_OK, kv.iface.set(kv.iface.ctx, UPD_RECORD_KEY, rec, n));
    pol_boot("0.1.1", true);
    TEST_ASSERT_EQUAL_STRING("", upd_policy_available(&pol));
}

/* ---------------------------------------------------------------- download */

#define IMG 10000u

static uint8_t image[IMG];
static uint8_t slot_mem[16384];
static fw_mem_t fw;

typedef struct {
    const uint8_t *body;        /* what the server has */
    uint32_t body_len;
    uint32_t declared;          /* Content-Length / Content-Range total to announce */
    bool ranges;                /* honours Range */
    int first_status;           /* status of the first open (0 = normal) */
    uint32_t cut_at;            /* the connection drops after this many body bytes in an attempt (0 = never) */
    int cuts;                   /* how many attempts get cut */
    uint32_t extra;             /* bytes appended past the declared size */
    uint32_t range_skew;        /* a server that continues at the wrong byte */
    uint32_t total_skew;        /* a 206 that names a file of another size */
    int opens, closes;
    uint32_t cur, end, sent, offsets[8];
    bool cut_now;
} net_t;

static int net_open(void *ctx, uint32_t offset, upd_http_head_t *h)
{
    net_t *n = ctx;
    n->offsets[n->opens < 8 ? n->opens : 7] = offset;
    const int attempt = n->opens++;
    n->sent = 0;
    n->cut_now = n->cut_at != 0 && attempt < n->cuts;
    if (attempt == 0 && n->first_status != 0) {
        h->status = n->first_status;
        return 0;
    }
    n->end = n->body_len + n->extra;
    h->content_len = UINT32_MAX;
    h->range_first = 0;
    h->range_total = UINT32_MAX;
    if (offset > 0 && n->ranges) {
        h->status = 206;
        h->range_first = offset + n->range_skew;
        h->range_total = n->declared + n->total_skew;
        h->content_len = n->end - offset;
        n->cur = offset;
    } else {
        h->status = 200;
        h->content_len = n->declared;
        n->cur = 0;
    }
    return 0;
}

static int net_read(void *ctx, void *buf, size_t cap)
{
    net_t *n = ctx;
    if (n->cut_now && n->sent >= n->cut_at) {
        return -1;
    }
    size_t left = n->end - n->cur;
    size_t take = left < cap ? left : cap;
    if (n->cut_now && take > n->cut_at - n->sent) {
        take = n->cut_at - n->sent;
    }
    for (size_t i = 0; i < take; i++) {
        ((uint8_t *)buf)[i] = n->cur + i < n->body_len ? n->body[n->cur + i] : 0xEE;
    }
    n->cur += (uint32_t)take;
    n->sent += (uint32_t)take;
    return (int)take;
}

static void net_close(void *ctx) { ((net_t *)ctx)->closes++; }

typedef struct {
    int progress_calls;
    uint32_t last_done;
    int cancel_after_calls; /* 0 = never */
} run_t;

static void on_progress(void *ctx, uint32_t done, uint32_t total)
{
    run_t *r = ctx;
    TEST_ASSERT_EQUAL_UINT32(IMG, total);
    TEST_ASSERT_TRUE(done >= r->last_done);
    r->last_done = done;
    r->progress_calls++;
}

static bool on_cancelled(void *ctx)
{
    run_t *r = ctx;
    return r->cancel_after_calls != 0 && r->progress_calls >= r->cancel_after_calls;
}

static net_t net;
static run_t run;
static uint8_t blk[4096];

static void dl_setup(void)
{
    for (uint32_t i = 0; i < IMG; i++) {
        image[i] = (uint8_t)(i * 31 + 7);
    }
    memset(slot_mem, 0xFF, sizeof(slot_mem));
    fw_mem_init(&fw, slot_mem, sizeof(slot_mem));
    memset(&net, 0, sizeof(net));
    net.body = image;
    net.body_len = IMG;
    net.declared = IMG;
    net.ranges = true;
    memset(&run, 0, sizeof(run));
}

static upd_download_params_t dl_params(const xfer_fw_sink_t *sink)
{
    upd_download_params_t p = {.sink = sink, .http = {net_open, net_read, net_close, &net}, .size = IMG,
                               .max_resumes = 3, .buf = blk, .buf_len = sizeof(blk), .progress = on_progress,
                               .cancelled = on_cancelled, .ctx = &run};
    proto_sha256(image, IMG, p.sha256);
    return p;
}

static upd_dl_result_t dl(const xfer_fw_sink_t *sink, proto_status_t *st)
{
    upd_download_params_t p = dl_params(sink);
    return upd_download(&p, st);
}

static void test_download_whole_image(void)
{
    dl_setup();
    proto_status_t st;
    TEST_ASSERT_EQUAL_INT(UPD_DL_OK, dl(&fw.iface, &st));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, st);
    TEST_ASSERT_EQUAL_MEMORY(image, slot_mem, IMG);
    TEST_ASSERT_TRUE(fw.boot_set);
    TEST_ASSERT_EQUAL_UINT32(IMG, run.last_done);
    TEST_ASSERT_EQUAL_INT(1, net.opens);
    TEST_ASSERT_EQUAL_INT(1, net.closes);
}

static void test_download_resumes_a_dropped_connection_with_a_range(void)
{
    dl_setup();
    net.cut_at = 3000;
    net.cuts = 2; /* the first two connections drop after 3000 bytes each */
    proto_status_t st;
    TEST_ASSERT_EQUAL_INT(UPD_DL_OK, dl(&fw.iface, &st));
    TEST_ASSERT_EQUAL_MEMORY(image, slot_mem, IMG);
    TEST_ASSERT_EQUAL_INT(3, net.opens);
    TEST_ASSERT_EQUAL_UINT32(0, net.offsets[0]);
    TEST_ASSERT_EQUAL_UINT32(3000, net.offsets[1]);
    TEST_ASSERT_EQUAL_UINT32(6000, net.offsets[2]);
    TEST_ASSERT_EQUAL_INT(3, net.closes); /* every connection is closed */
}

static void test_download_gives_up_after_max_resumes(void)
{
    dl_setup();
    net.cut_at = 1000;
    net.cuts = 99;
    proto_status_t st;
    TEST_ASSERT_EQUAL_INT(UPD_DL_NETWORK, dl(&fw.iface, &st));
    TEST_ASSERT_EQUAL_INT(4, net.opens); /* the first try and three resumes */
    TEST_ASSERT_FALSE(fw.boot_set);
}

static void test_download_without_range_support_starts_over(void)
{
    dl_setup();
    net.ranges = false;
    net.cut_at = 4000;
    net.cuts = 1;
    proto_status_t st;
    TEST_ASSERT_EQUAL_INT(UPD_DL_OK, dl(&fw.iface, &st)); /* 200 to the Range request: restart, hash again */
    TEST_ASSERT_EQUAL_MEMORY(image, slot_mem, IMG);
    TEST_ASSERT_EQUAL_INT(2, net.opens);
}

static void test_a_short_body_is_a_dropped_connection(void)
{
    dl_setup();
    net.body_len = 7000; /* the server ends early, twice as long as a drop */
    net.declared = IMG;
    proto_status_t st;
    TEST_ASSERT_EQUAL_INT(UPD_DL_NETWORK, dl(&fw.iface, &st));
    TEST_ASSERT_FALSE(fw.boot_set);
}

static void test_extra_bytes_are_refused(void)
{
    dl_setup();
    net.extra = 5;
    proto_status_t st;
    TEST_ASSERT_EQUAL_INT(UPD_DL_BAD_SIZE, dl(&fw.iface, &st));
    TEST_ASSERT_FALSE(fw.boot_set);
}

static void test_a_wrong_content_length_is_refused(void)
{
    dl_setup();
    net.declared = IMG + 1;
    proto_status_t st;
    TEST_ASSERT_EQUAL_INT(UPD_DL_BAD_SIZE, dl(&fw.iface, &st));
    TEST_ASSERT_EQUAL_INT(1, net.opens);
}

static void test_another_file_fails_the_hash(void)
{
    dl_setup();
    static uint8_t other[IMG];
    memcpy(other, image, IMG);
    other[5000] ^= 1;
    net.body = other;
    proto_status_t st;
    TEST_ASSERT_EQUAL_INT(UPD_DL_HASH, dl(&fw.iface, &st));
    TEST_ASSERT_FALSE(fw.boot_set); /* never asked to install it */
}

static void test_a_resume_must_continue_at_the_right_byte(void)
{
    dl_setup();
    net.cut_at = 3000;
    net.cuts = 1;
    net.range_skew = 1; /* 206, but not from the byte that was asked for */
    proto_status_t st;
    TEST_ASSERT_EQUAL_INT(UPD_DL_NETWORK, dl(&fw.iface, &st));
    TEST_ASSERT_FALSE(fw.boot_set);
    dl_setup();
    net.cut_at = 3000;
    net.cuts = 1;
    net.total_skew = 7; /* 206 of a file of another size */
    TEST_ASSERT_EQUAL_INT(UPD_DL_NETWORK, dl(&fw.iface, &st));
}

static void test_http_errors(void)
{
    proto_status_t st;
    dl_setup();
    net.first_status = 404;
    TEST_ASSERT_EQUAL_INT(UPD_DL_NETWORK, dl(&fw.iface, &st));
    TEST_ASSERT_EQUAL_INT(1, net.opens); /* a 4xx is final: no retries */
    dl_setup();
    net.first_status = 503;
    TEST_ASSERT_EQUAL_INT(UPD_DL_OK, dl(&fw.iface, &st)); /* a 5xx is tried again */
    TEST_ASSERT_EQUAL_INT(2, net.opens);
}

static void test_cancel_stops_and_aborts(void)
{
    dl_setup();
    run.cancel_after_calls = 3;
    proto_status_t st;
    TEST_ASSERT_EQUAL_INT(UPD_DL_CANCELLED, dl(&fw.iface, &st));
    TEST_ASSERT_FALSE(fw.boot_set);
    TEST_ASSERT_EQUAL_INT(1, net.closes);
    TEST_ASSERT_EQUAL_UINT32(0, fw.written); /* abort() ran */
}

static void test_the_sink_can_refuse(void)
{
    proto_status_t st;
    dl_setup();
    fw.image_status = PROTO_SIGNATURE_INVALID; /* esp_ota_end says the signature is wrong */
    TEST_ASSERT_EQUAL_INT(UPD_DL_REJECTED, dl(&fw.iface, &st));
    TEST_ASSERT_EQUAL_INT(PROTO_SIGNATURE_INVALID, st);
    dl_setup();
    fw.image_status = PROTO_IMAGE_INVALID;
    TEST_ASSERT_EQUAL_INT(UPD_DL_REJECTED, dl(&fw.iface, &st));
    TEST_ASSERT_EQUAL_INT(PROTO_IMAGE_INVALID, st);
    dl_setup();
    fw.image_status = PROTO_INTERNAL;
    TEST_ASSERT_EQUAL_INT(UPD_DL_SINK, dl(&fw.iface, &st));
    dl_setup();
    int32_t power = 1; /* the flash fails on the second block */
    fw.power = &power;
    TEST_ASSERT_EQUAL_INT(UPD_DL_SINK, dl(&fw.iface, &st));
    TEST_ASSERT_EQUAL_INT(PROTO_INTERNAL, st);
    dl_setup();
    fw_mem_init(&fw, slot_mem, 5000); /* the image does not fit the slot */
    TEST_ASSERT_EQUAL_INT(UPD_DL_SINK, dl(&fw.iface, &st));
    TEST_ASSERT_EQUAL_INT(PROTO_NO_SPACE, st);
    TEST_ASSERT_EQUAL_INT(0, net.opens); /* nothing was downloaded */
}

/* ---------------------------------------------------------------- download through ota_sink */

typedef struct {
    uint8_t mem[16384];
    bool open;
    uint32_t pos;
    ota_end_t end_st;
    int aborts;
    ota_flash_t iface;
} fake_flash_t;

static ota_flash_status_t ff_begin(void *c) { fake_flash_t *f = c; f->open = true; f->pos = 0; return OTA_FLASH_OK; }
static ota_flash_status_t ff_resume(void *c, uint32_t o) { fake_flash_t *f = c; f->open = true; f->pos = o; return OTA_FLASH_OK; }
static ota_flash_status_t ff_write(void *c, const void *d, size_t n)
{
    fake_flash_t *f = c;
    memcpy(f->mem + f->pos, d, n);
    f->pos += (uint32_t)n;
    return OTA_FLASH_OK;
}
static ota_flash_status_t ff_read(void *c, uint32_t o, void *b, size_t n) { memcpy(b, ((fake_flash_t *)c)->mem + o, n); return OTA_FLASH_OK; }
static ota_end_t ff_end(void *c, uint32_t s) { (void)s; ((fake_flash_t *)c)->open = false; return ((fake_flash_t *)c)->end_st; }
static void ff_abort(void *c) { ((fake_flash_t *)c)->aborts++; ((fake_flash_t *)c)->open = false; }

static void test_download_into_the_real_sink_and_the_other_path_is_busy(void)
{
    static fake_flash_t f;
    static ota_sink_t sink;
    dl_setup();
    memset(&f, 0, sizeof(f));
    f.iface = (ota_flash_t){.slot_size = sizeof(f.mem), .begin = ff_begin, .resume = ff_resume, .write = ff_write,
                            .read = ff_read, .end = ff_end, .abort = ff_abort, .ctx = &f};
    ota_sink_init(&sink, &f.iface);
    proto_status_t st;
    /* the BLE path started an image: the Wi-Fi download is refused */
    TEST_ASSERT_EQUAL_INT(PROTO_OK, sink.iface.begin(sink.iface.ctx, IMG));
    TEST_ASSERT_EQUAL_INT(UPD_DL_SINK, dl(&sink.wifi, &st));
    TEST_ASSERT_EQUAL_INT(PROTO_BUSY, st);
    TEST_ASSERT_EQUAL_INT(0, net.opens);
    TEST_ASSERT_TRUE(sink.open);
    ota_sink_ble_gone(&sink); /* the phone left: the sink is free again */
    TEST_ASSERT_FALSE(ota_sink_busy(&sink));
    TEST_ASSERT_EQUAL_INT(UPD_DL_OK, dl(&sink.wifi, &st));
    TEST_ASSERT_EQUAL_MEMORY(image, f.mem, IMG);
    TEST_ASSERT_FALSE(ota_sink_busy(&sink)); /* released after the install */
    /* and the other way round: a BLE transfer is refused while Wi-Fi writes */
    TEST_ASSERT_EQUAL_INT(PROTO_OK, sink.wifi.begin(sink.wifi.ctx, IMG));
    TEST_ASSERT_EQUAL_INT(PROTO_BUSY, sink.iface.begin(sink.iface.ctx, IMG));
    TEST_ASSERT_EQUAL_INT(PROTO_BUSY, sink.iface.resume(sink.iface.ctx, IMG, 0));
    TEST_ASSERT_EQUAL_INT(PROTO_BUSY, sink.iface.write(sink.iface.ctx, 0, image, 10));
    TEST_ASSERT_EQUAL_INT(PROTO_BUSY, sink.iface.finish(sink.iface.ctx, IMG));
    const int aborts = f.aborts;
    sink.iface.abort(sink.iface.ctx); /* the transfer's clean-up must not end the Wi-Fi image */
    TEST_ASSERT_EQUAL_INT(aborts, f.aborts);
    TEST_ASSERT_TRUE(ota_sink_busy(&sink));
    ota_sink_ble_gone(&sink); /* nor does a BLE disconnect */
    TEST_ASSERT_TRUE(ota_sink_busy(&sink));
    sink.wifi.abort(sink.wifi.ctx);
    TEST_ASSERT_FALSE(ota_sink_busy(&sink));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_newer_release_is_offered);
    RUN_TEST(test_the_image_of_this_board_is_picked);
    RUN_TEST(test_up_to_date_and_older_releases);
    RUN_TEST(test_refusals_in_order);
    RUN_TEST(test_broken_manifests_are_invalid);
    RUN_TEST(test_shared_manifest_vectors);
    RUN_TEST(test_only_github_hosts_are_allowed);
    RUN_TEST(test_the_image_url_follows_the_manifest_url);
    RUN_TEST(test_first_check_waits_for_clock_and_boot_delay);
    RUN_TEST(test_one_check_a_day_with_jitter);
    RUN_TEST(test_auto_check_off_stops_the_daily_check);
    RUN_TEST(test_failed_checks_retry_hourly_then_wait_a_day);
    RUN_TEST(test_an_available_release_marks_and_prompts_once);
    RUN_TEST(test_remind_later_hides_the_prompt_for_a_day);
    RUN_TEST(test_remind_later_without_a_clock_starts_at_the_next_check);
    RUN_TEST(test_prompt_needs_no_clock_before_it_was_shown);
    RUN_TEST(test_skip_hides_one_version_and_survives_a_restart);
    RUN_TEST(test_a_new_release_is_news_even_during_a_snooze);
    RUN_TEST(test_the_record_survives_a_restart);
    RUN_TEST(test_up_to_date_and_other_answers_clear_the_offer);
    RUN_TEST(test_an_installed_version_is_forgotten);
    RUN_TEST(test_installed_clears_the_record);
    RUN_TEST(test_a_damaged_record_is_ignored);
    RUN_TEST(test_download_whole_image);
    RUN_TEST(test_download_resumes_a_dropped_connection_with_a_range);
    RUN_TEST(test_download_gives_up_after_max_resumes);
    RUN_TEST(test_download_without_range_support_starts_over);
    RUN_TEST(test_a_short_body_is_a_dropped_connection);
    RUN_TEST(test_extra_bytes_are_refused);
    RUN_TEST(test_a_wrong_content_length_is_refused);
    RUN_TEST(test_another_file_fails_the_hash);
    RUN_TEST(test_a_resume_must_continue_at_the_right_byte);
    RUN_TEST(test_http_errors);
    RUN_TEST(test_cancel_stops_and_aborts);
    RUN_TEST(test_the_sink_can_refuse);
    RUN_TEST(test_download_into_the_real_sink_and_the_other_path_is_busy);
    return UNITY_END();
}
