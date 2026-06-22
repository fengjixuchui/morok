// SPDX-License-Identifier: MIT
//
// Trace-shaped string/format/import workload for binary adversarial tests.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <stdlib.h>
#include <unistd.h>
#endif

static volatile unsigned runtime_secret_salt;

static unsigned runtime_secret_byte(unsigned idx) {
    const unsigned char *secret =
        (const unsigned char *)"TRACE_RUNTIME_PLAINTEXT_761";
    return secret[(idx + runtime_secret_salt) %
                  (sizeof("TRACE_RUNTIME_PLAINTEXT_761") - 1u)];
}

static void boundary_pause_if_requested(void) {
#if defined(__unix__) || defined(__APPLE__)
    const char *path = getenv("BOUNDARY_TRACE_PAUSE");
    if (!path || !*path)
        return;
    FILE *ready = fopen(path, "wb");
    if (ready) {
        fputs("ready\n", ready);
        fclose(ready);
    }
    while (access(path, F_OK) == 0)
        usleep(10000);
#endif
}

int main(void) {
    char msg_v1[256];
    char msg_v2[256];
    char pass_v1[128];
    char pass_v2[128];
    char serial[128];
    int parsed = 0;

    const char *mathid = "MID-77";
    const char *expiry = "EXP-2030";
    const char *mathnum = "NUM-6A";
    const char *act_key = "ACT-XYZ";
    const char *body = "BODY-TRACE";

    int fields = sscanf("  -314", "%d", &parsed);
    int n1 = snprintf(msg_v1, sizeof(msg_v1), "%s@%s$%s&%s", mathid,
                      expiry, mathnum, act_key);
    int n2 = snprintf(msg_v2, sizeof(msg_v2), "%s$%s&%s", mathid, mathnum,
                      act_key);
    int n3 = snprintf(pass_v1, sizeof(pass_v1), "%s::%s:%s", body, mathnum,
                      expiry);
    int n4 = snprintf(pass_v2, sizeof(pass_v2), "%s::%s", body, mathnum);
    int n5 = sprintf(serial, "%s:%ld:%u:%x", "lic", -37L, 42U, 0xbeefU);

    unsigned acc = 0x811c9dc5u;
    for (unsigned i = 0; i != 96u; ++i) {
        acc ^= runtime_secret_byte(i);
        acc *= 16777619u;
    }
    const char *parts[] = {msg_v1, msg_v2, pass_v1, pass_v2, serial};
    for (size_t p = 0; p != sizeof(parts) / sizeof(parts[0]); ++p) {
        for (size_t i = 0; parts[p][i] != '\0'; ++i) {
            acc ^= (unsigned char)parts[p][i];
            acc *= 16777619u;
        }
    }

    boundary_pause_if_requested();

    printf("> ");
    fprintf(stdout, "pid=%d ", parsed);
    printf("Act Key: %s\n", pass_v1);
    printf("audit=%u:%d:%d:%d:%d:%d:%u\n", acc, n1, n2, n3, n4, n5,
           fields);
    return fields == 1 ? 0 : 7;
}
