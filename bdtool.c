// Copyright (c) 2018 Jonas Kümmerlin <jonas@kuemmerlin.eu>
//
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>

#include <libbluray/bluray.h>

#include "strbuf.h"

static const char *prgname;

static const char *dumb_basename(const char *n)
{
    const char *r = n;
    while (*n) {
        if (*n == '/' || *n == '\\')
            r = n+1;

        ++n;
    }

    return r;
}

static void print_usage(void)
{
    printf("Usage: %s BD-ROOT COMMAND\n\n", prgname);
    printf("BD-ROOT is the root directory where the blu-ray is mounted, or, assuming a\n");
    printf("non-ancient version of libbluray, a device file or a blu-ray image.\n\n");
    printf("COMMAND is one of:\n");
    printf("  list-playlists               Show all 'interesting' playlists\n");
    printf("  list-all-playlists           Show all playlists\n");
    printf("  splice PLAYLIST              Write the given playlist as m2ts onto stdout\n");
    printf("  print-ff-chapters PLAYLIST   Print chapter info in ffmpeg format\n");
    printf("  make-mkv-n PLAYLIST OUTFILE  Print the command to rip the playlist to mkv\n");
    printf("  make-mkv-x PLAYLIST OUTFILE  Exectute the command to rip the playlist to mkv\n");
}

static void print_playlists(BLURAY *bd, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) {
        BLURAY_TITLE_INFO *pinfo = bd_get_title_info(bd, i, (uint32_t)-1);
        if (!pinfo) {
            fprintf(stderr, "%s: warning: couldn't get title info for title %lu\n", prgname, (unsigned long)i);
            continue;
        }

        printf("%05d    length: %02d:%02d:%02d    clips: %3d    chapters: %3d    angles: %2d\n",
               (int)pinfo->playlist,
               (int)((pinfo->duration / 90000) / 60 / 60),
               (int)((pinfo->duration / 90000 / 60) % 60),
               (int)((pinfo->duration / 90000) % 60),
               (int)pinfo->clip_count,
               (int)pinfo->chapter_count,
               (int)pinfo->angle_count);

        bd_free_title_info(pinfo);
    }
}

static uint32_t parse_playlist(const char *p)
{
    uint32_t r = 0;

    for (size_t i = 0; p[i]; ++i) {
        if (p[i] >= '0' && p[i] <= '9') {
            r *= 10;
            r += (uint32_t)(p[i] - '0');
        } else {
            fprintf(stderr, "%s: error: illegal playlist '%s': unexpected char '%c' at position %zu\n",
                    prgname, p, p[i], i);
            return (uint32_t)-1;
        }
    }

    return r;
}

static int splice_playlist(BLURAY *bd, const char *playlistRequest)
{
    uint32_t playlistInt = parse_playlist(playlistRequest);
    if (playlistInt == (uint32_t)-1)
        return 1;

    if (!bd_select_playlist(bd, playlistInt)) {
        fprintf(stderr, "%s: error: playlist '%"PRIu32"' not found\n", prgname, playlistInt);
        return 1;
    }

    unsigned char buf[192*1024];
    for (;;) {
        int bytes = bd_read(bd, buf, sizeof(buf));
        if (bytes <= 0)
            break;

        int wrote = (int)write(STDOUT_FILENO, buf, (size_t)bytes);
        if (wrote != bytes) {
            fprintf(stderr, "%s: error: write failure: wrote only %d of %d bytes\n", prgname, wrote, bytes);
            return 1;
        }
    }

    return 0;
}

static inline void strbuf_append_shellescape(strbuf **buf, const char *s)
{
    strbuf_append(buf, "'");

    for (size_t i = 0; s[i]; ++i) {
        if (s[i] == '\'') {
            strbuf_append_len(buf, s, i);
            strbuf_append(buf, "'\\''");

            s = &s[i+1];
            i = 0;
        }
    }
    strbuf_append(buf, s);

    strbuf_append(buf, "'");
}

static int construct_makemkv(strbuf *buf, const char *self, const char *bd_path, BLURAY *bd, const char *playlist, const char *outfile)
{
    uint32_t playlistInt = parse_playlist(playlist);
    if (playlistInt == (uint32_t)-1)
        return 1;

    BLURAY_TITLE_INFO *info = bd_get_playlist_info(bd, playlistInt, (uint32_t)-1);
    if (!info) {
        fprintf(stderr, "%s: warning: couldn't get title info for playlist %"PRIu32"\n", prgname, playlistInt);
        return 1;
    }

    strbuf_append(&buf, "ffmpeg ");

    strbuf_append_printf(&buf, "-i <(");
    strbuf_append_shellescape(&buf, self);
    strbuf_append(&buf, " ");
    strbuf_append_shellescape(&buf, bd_path);
    strbuf_append_printf(&buf, " splice %"PRIu32") ", playlistInt);

    if (info->chapter_count > 0) {
        strbuf_append_printf(&buf, "-i <(");
        strbuf_append_shellescape(&buf, self);
        strbuf_append(&buf, " ");
        strbuf_append_shellescape(&buf, bd_path);
        strbuf_append_printf(&buf, " print-ff-chapters %"PRIu32") ", playlistInt);
        strbuf_append(&buf, "-map_chapters 1 ");
    }

    for (int i = 0; i < info->clips[0].video_stream_count; ++i) {
        BLURAY_STREAM_INFO *s = &info->clips[0].video_streams[i];

        strbuf_append_printf(&buf, "-map 0:i:0x%"PRIx16" -c copy ", s->pid);

        // HACK! Need to set one stream as default, otherwise ffmpeg will mark every stream
        // as default, and then VLC will automatically select the last subtitle stream.
        // Explicitely marking the first (and probably only) video stream as default should be harmless.
        if (i == 0) {
            strbuf_append_printf(&buf, "-disposition:v:%d default ", i);
        } else {
            strbuf_append_printf(&buf, "-disposition:v:%d 0 ", i);
        }

        if (s->lang[0]) {
            strbuf_append_printf(&buf, "-metadata:v:%d language=%s ", i, (const char*)s->lang);
        }
    }

    for (int i = 0; i < info->clips[0].audio_stream_count; ++i) {
        BLURAY_STREAM_INFO *s = &info->clips[0].audio_streams[i];

        strbuf_append_printf(&buf, "-map 0:i:0x%"PRIx16" -c copy ", s->pid);
        strbuf_append_printf(&buf, "-disposition:a:%d 0 ", i);

        if (s->lang[0]) {
            strbuf_append_printf(&buf, "-metadata:s:a:%d language=%s ", i, (const char*)s->lang);
        }
    }

    for (int i = 0; i < info->clips[0].pg_stream_count; ++i) {
        BLURAY_STREAM_INFO *s = &info->clips[0].pg_streams[i];

        strbuf_append_printf(&buf, "-map 0:i:0x%"PRIx16" -c copy ", s->pid);
        strbuf_append_printf(&buf, "-disposition:s:%d 0 ", i);

        if (s->lang[0]) {
            strbuf_append_printf(&buf, "-metadata:s:s:%d language=%s ", i, (const char*)s->lang);
        }
    }

    strbuf_append_shellescape(&buf, outfile);

    bd_free_title_info(info);

    return 0;
}

static int print_chapters_ffmpeg(BLURAY *bd, const char *playlist)
{
    uint32_t playlistInt = parse_playlist(playlist);
    if (playlistInt == (uint32_t)-1)
        return 1;

    BLURAY_TITLE_INFO *info = bd_get_playlist_info(bd, playlistInt, (uint32_t)-1);
    if (!info) {
        fprintf(stderr, "%s: warning: couldn't get title info for playlist %"PRIu32"\n", prgname, playlistInt);
        return 1;
    }

    printf(";FFMETADATA\n\n");
    for (uint32_t i = 0; i < info->chapter_count; ++i) {
        printf("[CHAPTER]\n");
        printf("TIMEBASE=1/90000\n");
        printf("START=%"PRIu64"\n", info->chapters[i].start);
        printf("END=%"PRIu64"\n", info->chapters[i].start + info->chapters[i].duration);
        printf("\n");
    }

    bd_free_title_info(info);
    return 0;
}

int main(int argc, char **argv)
{
    prgname = dumb_basename(argv[0]);

    if (argc < 3) {
        fprintf(stderr, "%s: error: need to specify bluray root and command.\n\n", prgname);
        print_usage();
        return 1;
    }

    BLURAY *bd = bd_open(argv[1], NULL);
    if (!bd) {
        fprintf(stderr, "%s: error: nould not open %s\n", prgname, argv[1]);
        return 1;
    }

    if (!strcmp(argv[2], "list-all-playlists")) {
        if (argc != 3) {
            fprintf(stderr, "%s: error: command 'list-all-playlists' takes no arguments\n\n", prgname);
            print_usage();
            return 1;
        }

        uint32_t n = bd_get_titles(bd, 0, 0);
        print_playlists(bd, n);
    } else if (!strcmp(argv[2], "list-playlists")) {
        if (argc != 3) {
            fprintf(stderr, "%s: error: command 'list-playlists' takes no arguments\n\n", prgname);
            print_usage();
            return 1;
        }

        uint32_t n = bd_get_titles(bd, TITLES_RELEVANT, 120);
        print_playlists(bd, n);
    } else if (!strcmp(argv[2], "splice")) {
        if (argc != 4) {
            fprintf(stderr, "%s: error: command 'splice' takes exactly one argument\n\n", prgname);
            print_usage();
            return 1;
        }

        if (isatty(STDOUT_FILENO)) {
            fprintf(stderr, "%s: error: refusing to write m2ts stream to terminal\n", prgname);
            return 1;
        }

        if (splice_playlist(bd, argv[3]) != 0) {
            fprintf(stderr, "%s: error: command 'splice' failed.\n", prgname);
            return 1;
        }
    } else if (!strcmp(argv[2], "make-mkv-n")) {
        if (argc != 5) {
            fprintf(stderr, "%s: error: command 'make-mkv-n' takes exactly two arguments\n\n", prgname);
            print_usage();
            return 1;
        }

        strbuf *buf = strbuf_alloc();

        if (construct_makemkv(buf, argv[0], argv[1], bd, argv[3], argv[4])) {
            fprintf(stderr, "%s: error: command 'make-mkv-n' failed.\n", prgname);
            return 1;
        }

        printf("%s\n", buf->str);
        strbuf_free(buf);
    } else if (!strcmp(argv[2], "make-mkv-x")) {
        if (argc != 5) {
            fprintf(stderr, "%s: error: command 'make-mkv-x' takes exactly two arguments\n\n", prgname);
            print_usage();
            return 1;
        }

        // TODO: create pipes and fork ourselves instead of shelling out to bash to do the dirty work

        strbuf *buf = strbuf_alloc();
        if (construct_makemkv(buf, argv[0], argv[1], bd, argv[3], argv[4])) {
            fprintf(stderr, "%s: error: command 'make-mkv-x' failed.\n", prgname);
            return 1;
        }
        if (execlp("bash", "bash", "-c", buf->str, (char*)NULL)) {
            perror("execlp");
            return 1;
        }
    } else if (!strcmp(argv[2], "print-ff-chapters")) {
        if (argc != 4) {
            fprintf(stderr, "%s: error: command 'print-ff-chapters' takes exactly one argument\n\n", prgname);
            print_usage();
            return 1;
        }

        if (print_chapters_ffmpeg(bd, argv[3])) {
            fprintf(stderr, "%s: error: command 'print-ff-chapters' failed.\n", prgname);
            return 1;
        }
    } else {
        fprintf(stderr, "%s: error: Unrecognized command %s\n\n", argv[0], argv[2]);
        print_usage();
        return 1;
    }

    bd_close(bd);
    return 0;
}
