#!/usr/bin/env python3
"""Configure MPlayer CE (config.mak, config.h) the way WiiFin uses it.

    python3 tools/mplayer/configure.py <mplayer-ce>/mplayer

Starts from the Wii configuration in MPlayer CE's repository and:
- builds a library (DISABLE_MAIN: main() becomes mplayer_main()) for the
  current devkitPPC: no -mpaired/-mstring, -O2, warnings that GCC 14+ made
  errors are warnings again
- drops what WiiFin does not use: networking (WiiFin brings its own HTTP
  and HTTPS streams), DVD, libass, md5sum output
- keeps only the FFmpeg decoders, parsers and demuxers below, compact
  code (CONFIG_SMALL) and precomputed tables (in .rodata instead of 256 KB
  of tables built in RAM); no encoders, zlib, DCT, RDFT, DWT or alpha in
  swscale
MPlayer tests its options with #ifdef, FFmpeg with #if: the first are
#undef'd, the second set to 0 or 1.
"""
import re
import sys

KEEP = '''
aac_decoder ac3_decoder amrnb_decoder dca_decoder flac_decoder flv_decoder
h263_decoder h264_decoder mjpeg_decoder mp2_decoder mp3_decoder
mpeg1video_decoder mpeg2video_decoder mpeg4_decoder mpegvideo_decoder
theora_decoder vc1_decoder vorbis_decoder vp3_decoder vp6_decoder
vp6f_decoder vp8_decoder wavpack_decoder wmapro_decoder wmav2_decoder
wmv3_decoder
aac_parser ac3_parser dca_parser flac_parser h263_parser h264_parser
mjpeg_parser mpeg4video_parser mpegaudio_parser mpegvideo_parser vc1_parser
vp3_parser vp8_parser
flv_demuxer matroska_demuxer mov_demuxer mpegts_demuxer ogg_demuxer wv_demuxer
'''.split()
KINDS = 'DECODER|ENCODER|HWACCEL|PARSER|BSF|DEMUXER|MUXER|PROTOCOL|INDEV|OUTDEV|FILTER'
WARNINGS = ('-Wno-implicit-function-declaration -Wno-incompatible-pointer-types '
            '-Wno-int-conversion -Wno-discarded-qualifiers')


def edit(path, fn):
    """Edit a CRLF file as text, keeping its line endings."""
    raw = open(path, 'rb').read()
    crlf = b'\r\n' in raw
    text = raw.decode('latin1').replace('\r\n', '\n')
    text = fn(text)
    if crlf:
        text = text.replace('\n', '\r\n')
    open(path, 'wb').write(text.encode('latin1'))


def sub1(pattern, repl, text, flags=re.M):
    new, n = re.subn(pattern, repl, text, flags=flags)
    if not n:
        sys.exit('configure.py: no match for %r' % pattern)
    return new


def config_mak(s):
    for flag in (' -mpaired', ' -mstring'):
        s = s.replace(flag, '')
    s = s.replace('-O3', '-O2')
    s = sub1(r'^(CFLAGS\s*=.*)$',
             r'\1 -DHW_RVL -DDISABLE_MAIN ' + WARNINGS +
             r' -I$(DEVKITPRO)/portlibs/ppc/include/freetype2 -Iwiifin', s)
    for key, value in [('HAVE_PAIRED', 'no'), ('HAVE_PTHREADS', 'yes'),
                       ('NETWORKING', 'no'), ('DVDNAV', 'no'), ('DVDNAV_INTERNAL', 'no'),
                       ('DVDREAD', 'no'), ('DVDREAD_INTERNAL', 'no'), ('LIBDVDCSS_INTERNAL', 'no'),
                       ('LIBASS', 'no'), ('LIBASS_INTERNAL', 'no'), ('MD5SUM', 'no'),
                       ('CONFIG_DCT', 'no'), ('CONFIG_RDFT', 'no'), ('CONFIG_DWT', 'no'), ('CONFIG_ENCODERS', 'no'),
                       ('CONFIG_ZLIB', 'no')]:
        s = sub1(r'^%s\s*=.*$' % key, '%s = %s' % (key, value), s)
    s += 'CONFIG_HARDCODED_TABLES=yes\n'

    def component(m):
        on = (m.group(1) + '_' + m.group(2)).lower() in KEEP
        return 'CONFIG_%s_%s=%s' % (m.group(1), m.group(2), 'yes' if on else 'no')
    return re.sub(r'^CONFIG_(\w+?)_(%s)=(yes|no)$' % KINDS, component, s, flags=re.M)


def config_h(s):
    for key, value in [('HAVE_PAIRED', 0), ('HAVE_PTHREADS', 1), ('HAVE_THREADS', 1),
                       ('CONFIG_NETWORK', 0), ('CONFIG_ENCODERS', 0), ('CONFIG_DCT', 0),
                       ('CONFIG_RDFT', 0), ('CONFIG_DWT', 0), ('CONFIG_SWSCALE_ALPHA', 0),
                       ('CONFIG_SMALL', 1), ('CONFIG_HARDCODED_TABLES', 1)]:
        s = sub1(r'^#define %s \S+' % key, '#define %s %d' % (key, value), s)
    for key in ('CONFIG_NETWORKING', 'CONFIG_FTP', 'CONFIG_DVDNAV', 'CONFIG_DVDREAD',
                'CONFIG_ASS', 'CONFIG_MD5SUM', 'CONFIG_ZLIB'):
        s = sub1(r'^#define %s \S+.*$' % key, '#undef %s' % key, s)
    s = sub1(r'^#define CONFIG_ICONV 1$', '#undef CONFIG_ICONV', s)
    # defined once in plat_gekko.c: GCC 10+ no longer merges common symbols
    s = sub1(r'^char (MPLAYER_(DATA|CONF|LIB)DIR\[100\]);', r'extern char \1;', s)
    s += '\n#ifndef MAXPATHLEN\n#define MAXPATHLEN 4096\n#endif\n'

    def component(m):
        on = (m.group(1) + '_' + m.group(2)).lower() in KEEP
        return '#define CONFIG_%s_%s %d' % (m.group(1), m.group(2), 1 if on else 0)
    return re.sub(r'^#define CONFIG_(\w+?)_(%s) [01]$' % KINDS, component, s, flags=re.M)


if __name__ == '__main__':
    d = sys.argv[1]
    edit(d + '/config.mak', config_mak)
    edit(d + '/config.h', config_h)
