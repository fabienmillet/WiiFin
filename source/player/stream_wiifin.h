#pragma once

/* Whether the HTTPS stream module verifies the server certificate (same rules
 * as JellyfinClient).  Set from JellyfinClient::sslVerify before playback. */
extern bool g_wiifin_stream_tls_verify;

/* HTTP status of the last stream open that failed (0 = it worked or never got
 * an answer), and the start of the server's error message. */
extern int  g_wiifin_stream_fail_status;
extern char g_wiifin_stream_fail_body[160];

/* Bytes of video received since start-up (every stream): with the cache,
 * PlaySession tells a dead connection from a picture stuck while the data
 * comes in. */
extern volatile unsigned long long g_wiifin_stream_bytes;
