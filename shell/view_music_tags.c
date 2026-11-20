// SPDX-License-Identifier: MIT
/*
 * view_music_tags.c - titolo e artista dai tag ID3 degli mp3, al risparmio.
 *
 * Gira nel processo figlio della scansione (media_scan.c), quindi non puo'
 * rallentare l'interfaccia; legge comunque pochissimo: i primi 8 kB per
 * ID3v2 (2.2, 2.3, 2.4: TIT2/TPE1 stanno quasi sempre prima delle
 * copertine) e, se non basta, gli ultimi 128 byte per ID3v1. Niente altro
 * formato: flac, ogg e gli altri mostrano il nome del file finche' il
 * lettore non comunica il titolo.
 */
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "media.h"

#define HEAD 8192

static int put_utf8(char *d, int n, int cap, unsigned cp)
{
	if (cp < 0x80) {
		if (n + 1 < cap)
			d[n++] = (char)cp;
	} else if (cp < 0x800) {
		if (n + 2 < cap) {
			d[n++] = (char)(0xC0 | (cp >> 6));
			d[n++] = (char)(0x80 | (cp & 0x3F));
		}
	} else if (n + 3 < cap) {
		d[n++] = (char)(0xE0 | (cp >> 12));
		d[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
		d[n++] = (char)(0x80 | (cp & 0x3F));
	}
	return n;
}

/* testo di un frame ID3 (enc: 0 Latin-1, 1 UTF-16 con BOM, 2 UTF-16BE, 3 UTF-8) */
static void decode(char *out, int cap, int enc, const unsigned char *s, int len)
{
	char tmp[MS_TEXT * 2];
	int n = 0, i, be = enc == 2;

	if (enc == 3) {
		ms_clean(out, cap, (const char *)s, len);
		return;
	}
	if (enc == 1 && len >= 2) {
		be = s[0] == 0xFE && s[1] == 0xFF;
		if (be || (s[0] == 0xFF && s[1] == 0xFE)) {
			s += 2;
			len -= 2;
		}
	}
	if (enc == 0) {
		for (i = 0; i < len && s[i]; i++)
			n = put_utf8(tmp, n, (int)sizeof(tmp), s[i]);
	} else {
		for (i = 0; i + 1 < len; i += 2) {
			unsigned cp = be ? (unsigned)(s[i] << 8 | s[i + 1]) : (unsigned)(s[i + 1] << 8 | s[i]);

			if (!cp)
				break;
			if (cp >= 0xD800 && cp < 0xE000)
				cp = '?';	/* fuori dal piano di base: i font non lo coprono comunque */
			n = put_utf8(tmp, n, (int)sizeof(tmp), cp);
		}
	}
	ms_clean(out, cap, tmp, n);
}

static unsigned be32(const unsigned char *p, int syncsafe)
{
	if (syncsafe)
		return (unsigned)(p[0] & 0x7F) << 21 | (unsigned)(p[1] & 0x7F) << 14 |
		       (unsigned)(p[2] & 0x7F) << 7 | (unsigned)(p[3] & 0x7F);
	return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}

static void id3v2(const unsigned char *b, int n, char *title, char *artist, int cap)
{
	int ver, pos = 10, end, hdr;

	if (n < 10 || memcmp(b, "ID3", 3))
		return;
	ver = b[3];
	if (ver < 2 || ver > 4 || (b[5] & 0xC0))
		return;		/* versione ignota, non sincronizzato o con intestazione estesa */
	end = 10 + (int)be32(b + 6, 1);
	if (end > n)
		end = n;
	hdr = ver == 2 ? 6 : 10;
	while (pos + hdr <= end && b[pos] && !(title[0] && artist[0])) {
		const unsigned char *f = b + pos;
		unsigned size = ver == 2 ? (unsigned)f[3] << 16 | (unsigned)f[4] << 8 | f[5] :
					   be32(f + 4, ver == 4);
		int is_title = !memcmp(f, ver == 2 ? "TT2" : "TIT2", ver == 2 ? 3 : 4);
		int is_artist = !memcmp(f, ver == 2 ? "TP1" : "TPE1", ver == 2 ? 3 : 4);

		pos += hdr;
		if (size > (unsigned)(end - pos))
			size = (unsigned)(end - pos);	/* frame che esce dal blocco letto */
		if (size > 1 && (is_title || is_artist))
			decode(is_title ? title : artist, cap, f[hdr], f + hdr + 1, (int)size - 1);
		pos += (int)size;
	}
}

static void id3v1(int fd, char *title, char *artist, int cap)
{
	unsigned char t[128];
	off_t sz = lseek(fd, 0, SEEK_END);

	if (sz < 128 || lseek(fd, sz - 128, SEEK_SET) < 0)
		return;
	if (read(fd, t, sizeof(t)) != (ssize_t)sizeof(t) || memcmp(t, "TAG", 3))
		return;
	if (!title[0])
		decode(title, cap, 0, t + 3, 30);
	if (!artist[0])
		decode(artist, cap, 0, t + 33, 30);
}

void mp_probe_tags(const char *path, char *title, char *artist, int cap)
{
	unsigned char b[HEAD];
	const char *dot = strrchr(path, '.');
	ssize_t n;
	int fd;

	if (!dot || strcasecmp(dot, ".mp3"))
		return;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	n = read(fd, b, sizeof(b));
	if (n > 0)
		id3v2(b, (int)n, title, artist, cap);
	if (!title[0] || !artist[0])
		id3v1(fd, title, artist, cap);
	close(fd);
}
