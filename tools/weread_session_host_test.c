/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：微信读书会话结构的主机回归：校验、cookie 吸收与请求头拼装。
 * English: Host regression for the WeRead session record: validation, cookie absorption and header build.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "weread_session.h"

int main(void) {
    wr_session_t session;
    memset(&session, 0, sizeof(session));
    assert(!wr_session_valid(&session));

    session.magic = WR_SESSION_MAGIC;
    session.version = WR_SESSION_VERSION;
    assert(wr_session_set_cookie(&session, "wr_vid", 6, "1234567890", 10));
    assert(wr_session_set_cookie(&session, "wr_skey", 7, "abc_DEF-123", 11));
    assert(wr_session_set_cookie(&session, "wr_fp", 5, "ignored", 7) == false);
    assert(wr_session_valid(&session));
    assert(!session.rt[0]);

    char header[512];
    assert(wr_session_cookie_header(&session, header, sizeof(header)));
    assert(!strcmp(header, "wr_vid=1234567890; wr_skey=abc_DEF-123"));

    assert(wr_session_set_cookie(&session, "wr_rt", 5, "rt%2Fvalue", 10));
    assert(wr_session_cookie_header(&session, header, sizeof(header)));
    assert(!strcmp(header, "wr_vid=1234567890; wr_skey=abc_DEF-123; wr_rt=rt%2Fvalue"));

    assert(!wr_session_set_cookie(&session, "wr_vid", 6, "bad;value", 9));
    assert(!wr_session_set_cookie(&session, "wr_vid", 6, "bad value", 9));
    assert(!wr_session_set_cookie(&session, "wr_vid", 6, "", 0));
    assert(session.vid[0] && !strcmp(session.vid, "1234567890"));

    char too_small[16];
    assert(!wr_session_cookie_header(&session, too_small, sizeof(too_small)));

    session.skey[0] = 0;
    assert(!wr_session_valid(&session));
    assert(!wr_session_cookie_header(&session, header, sizeof(header)));

    wr_session_clear(&session);
    assert(!wr_session_valid(&session));

    puts("weread session: OK");
    return 0;
}
