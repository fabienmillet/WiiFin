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

/* WiiFin's own reads of an HTTP(S) stream, outside MPlayer (the music that
 * goes on while browsing), with the same connections: open and read from
 * one thread; MPlayer's async_quit_request breaks off a read. */
void* wiifin_http_open(const char* url);                /* nullptr: failed */
int   wiifin_http_read(void* h, char* buf, int max);    /* 0: the end, or an error */
void  wiifin_http_close(void* h);
