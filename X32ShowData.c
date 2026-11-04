/*
 * X32ShowData.c
 *
 * Show, scene, snippet and preset-library retention for the X32 emulator.
 * This file is included at the bottom of X32.c. Do not compile it alone.
 *
 * Part of Patrick-Gilles Maillot's X32 emulator. Same license as X32.c.
 * Scene/snippet/library commands follow his unofficial X32 OSC protocol.
 *
 * Files live in ./x32data and are rewritten as OSC save/load/delete commands
 * arrive. A scene snapshot is the live engine (the desk does not upload a
 * blob: X32-Edit applies parameters, then /save copies them into a slot).
 *
 * With -d <folder>, that directory is the store instead: one console .shw,
 * .scn scene files, and a snippets/ directory of .snp files. Recall applies
 * the file's text lines; /save rewrites those same files in console format.
 */

#define X32DATA "x32data"
#define X32_PATHMAX 768

int x32_scene_idx = 0;
int x32_snippet_idx = 0;
int x32_cue_idx = 0;

static int x32_proj_on = 0;
static int x32_unlink_bodies = 1;
static char x32_proj_dir[X32_PATHMAX];
static char x32_shw_path[X32_PATHMAX];
static char x32_shw_tag[16];
static char x32_shw_edit[64];
static char x32_scene_path[100][X32_PATHMAX];
static char x32_snip_path[100][X32_PATHMAX];
static unsigned char x32_scene_listed[100];
static unsigned char x32_snip_listed[100];

static void x32_save_shw(void);
static int x32_write_console_scene(int idx);
static int x32_write_console_snip(int idx);
static int x32_apply_console_file(const char *path, int safes, int idx);
static void x32_load_project(void);
static int x32_numbered(char *dst, int cap, const char *dir, const char *fmt,
		int idx, const char *nm);

static int x32_tag_at;
static int x32_arg_at;

static void x32_set_str(X32command *c, const char *s) {
	if (c->value.str) {
		free(c->value.str);
		c->value.str = NULL;
	}
	if (s && s[0]) {
		c->value.str = malloc(strlen(s) + 1);
		if (c->value.str) strcpy(c->value.str, s);
	}
}

static void x32_copy_clean(char *dst, const char *src, int cap) {
	int w = 0;
	if (!src) src = "";
	while (*src && w < cap - 1) {
		char c = *src++;
		if (c == '\n' || c == '\r' || c == '\t' || c == '"') c = ' ';
		dst[w++] = c;
	}
	dst[w] = 0;
}

static void x32_sanitize_file(char *s) {
	if (!s) return;
	for (; *s; s++) {
		if (*s == '/' || *s == '\\' || *s == ':' || *s == '*' || *s == '?' ||
				*s == '"' || *s == '<' || *s == '>' || *s == '|')
			*s = '_';
	}
}

static int x32_bit(int v, int b) {
	if (b < 0 || b > 31) return 0;
	return (int)((((unsigned)v) >> b) & 1u);
}

int x32_osc_has_arg(void) {
	int i = 0;
	while (i < r_len && r_buf[i]) i++;
	i = (i + 4) & ~3;
	return (i < r_len && r_buf[i] == ',' && r_buf[i + 1] != 0);
}

static int x32_osc_prepare(void) {
	int i = 0;
	while (i < r_len && r_buf[i]) i++;
	i = (i + 4) & ~3;
	if (i >= r_len || r_buf[i] != ',') return 0;
	x32_tag_at = i;
	while (i < r_len && r_buf[i]) i++;
	x32_arg_at = (i + 4) & ~3;
	return 1;
}

static int x32_ntags(char kind) {
	int i = x32_tag_at + 1;
	int n = 0;
	while (i < BSIZE && r_buf[i]) {
		if (r_buf[i] == kind) n++;
		i++;
	}
	return n;
}

static char *x32_take_str(void) {
	char *s = r_buf + x32_arg_at;
	int n = (int)strlen(s);
	x32_arg_at = (x32_arg_at + n + 4) & ~3;
	return s;
}

static int x32_take_int(void) {
	int j = 4;
	endian.ii = 0;
	while (j) endian.cc[--j] = r_buf[x32_arg_at++];
	return endian.ii;
}

static int x32_show_control(void) {
	int i;
	for (i = 0; i < Xprefs_max; i++) {
		if (strcmp(Xprefs[i].command, "/-prefs/show_control") == 0)
			return Xprefs[i].value.ii;
	}
	return 0;
}

static void x32_mkdir(const char *p) {
#ifdef __WIN32__
	_mkdir(p);
#else
	mkdir(p, 0755);
#endif
}

static void x32_commit(const char *final, const char *tmp) {
	remove(final);
	rename(tmp, final);
}

static X32command *x32_scene_at(int n) {
	int b = 1 + n * 5;
	if (n < 0 || n > 99 || b + 4 >= Xscene_max) return NULL;
	return &Xscene[b];
}

static X32command *x32_snip_at(int n) {
	int b = 1 + n * 7;
	if (n < 0 || n > 99 || b + 6 >= Xsnippet_max) return NULL;
	return &Xsnippet[b];
}

static X32command *x32_cue_at(int n) {
	int b = 16 + n * 10;
	if (n < 0 || n > 499 || b + 9 >= Xshow_max) return NULL;
	return &Xshow[b];
}

/* kind: 0 channel, 1 fx, 2 routing. Returns the slot header. */
static X32command *x32_lib_at(int kind, int n) {
	int b;
	if (n < 0 || n > 99) return NULL;
	if (kind == 0) {
		b = 2 + n * 6;
		if (b + 5 >= Xlibsc_max) return NULL;
		return &Xlibsc[b];
	}
	if (kind == 1) {
		b = 1 + n * 6;
		if (b + 5 >= Xlibsf_max) return NULL;
		return &Xlibsf[b];
	}
	b = 1 + n * 6;
	if (b + 5 >= Xlibsr_max) return NULL;
	return &Xlibsr[b];
}

static void Xreply_now(void) {
	if (!s_len) return;
	if (Xverbose) {
		Xfdump("X->", s_buf, s_len, Xdebug);
		fflush(stdout);
	}
	if (sendto(Xfd, s_buf, s_len, 0, Client_ip_pt, Client_ip_len) < 0)
		perror("Error while sending data");
}

static void x32_publish(X32command *c) {
	if (!c) return;
	if (c->format.typ == S32)
		s_len = Xfprint(s_buf, 0, c->command, 's', c->value.str);
	else
		s_len = Xfprint(s_buf, 0, c->command, 'i', &c->value.ii);
	Xsend(S_REM);
	Xreply_now();
}

static int x32_reply_si(const char *addr, const char *typ, int ok) {
	s_len = Xsprint(s_buf, 0, 's', (char *)addr);
	s_len = Xsprint(s_buf, s_len, 's', ",si");
	s_len = Xsprint(s_buf, s_len, 's', (char *)typ);
	s_len = Xsprint(s_buf, s_len, 'i', ok ? &one : &zero);
	return S_SND;
}

static int x32_read_lstr(FILE *f, char *buf, int cap) {
	int n, i, ch, keep;
	if (fscanf(f, "%d", &n) != 1 || n < 0) return -1;
	ch = fgetc(f);
	(void)ch;
	keep = n;
	if (keep > cap - 1) keep = cap - 1;
	for (i = 0; i < keep; i++) {
		ch = fgetc(f);
		buf[i] = (ch < 0) ? 0 : (char)ch;
	}
	buf[keep] = 0;
	for (i = keep; i < n; i++) fgetc(f);
	return n;
}

static void x32_write_lstr(FILE *f, const char *s) {
	int n = (s && s[0]) ? (int)strlen(s) : 0;
	fprintf(f, "%d ", n);
	if (n) fwrite(s, 1, n, f);
}

static void x32_save_show(void) {
	FILE *f;
	int i;
	if (x32_proj_on) {
		x32_save_shw();
		return;
	}
	f = fopen(X32DATA "/show.txt.tmp", "w");
	if (!f) return;
	x32_write_lstr(f, Xshow[4].value.str);
	for (i = 0; i < 10; i++) fprintf(f, " %d", Xshow[5 + i].value.ii);
	fputc('\n', f);
	fclose(f);
	x32_commit(X32DATA "/show.txt", X32DATA "/show.txt.tmp");
}

static void x32_load_show(void) {
	FILE *f;
	char name[128];
	int i, n;
	f = fopen(X32DATA "/show.txt", "r");
	if (!f) return;
	if (x32_read_lstr(f, name, 128) >= 0) {
		x32_set_str(&Xshow[4], name);
		for (i = 0; i < 10; i++) {
			if (fscanf(f, "%d", &n) == 1) Xshow[5 + i].value.ii = n;
		}
	}
	fclose(f);
}

static void x32_save_recall(void) {
	FILE *f;
	if (x32_proj_on) return;
	f = fopen(X32DATA "/recall.txt.tmp", "w");
	if (!f) return;
	fprintf(f, "%d %d %d\n", x32_scene_idx, x32_snippet_idx, x32_cue_idx);
	fclose(f);
	x32_commit(X32DATA "/recall.txt", X32DATA "/recall.txt.tmp");
}

static void x32_load_recall(void) {
	FILE *f;
	int a, b, c;
	f = fopen(X32DATA "/recall.txt", "r");
	if (!f) return;
	if (fscanf(f, "%d %d %d", &a, &b, &c) == 3) {
		if (a >= 0 && a <= 99) x32_scene_idx = a;
		if (b >= 0 && b <= 99) x32_snippet_idx = b;
		if (c >= 0 && c <= 499) x32_cue_idx = c;
	}
	fclose(f);
}

static void x32_save_scenes_idx(void) {
	FILE *f;
	int n;
	if (x32_proj_on) {
		x32_save_shw();
		return;
	}
	f = fopen(X32DATA "/scenes.idx.tmp", "w");
	if (!f) return;
	for (n = 0; n < 100; n++) {
		X32command *p = x32_scene_at(n);
		if (!p) break;
		fprintf(f, "%d ", n);
		x32_write_lstr(f, (p + 1)->value.str);
		fputc(' ', f);
		x32_write_lstr(f, (p + 2)->value.str);
		fprintf(f, " %d %d\n", (p + 3)->value.ii, (p + 4)->value.ii);
	}
	fclose(f);
	x32_commit(X32DATA "/scenes.idx", X32DATA "/scenes.idx.tmp");
}

static void x32_load_scenes_idx(void) {
	FILE *f;
	char name[128], notes[256];
	int n, safes, has;
	f = fopen(X32DATA "/scenes.idx", "r");
	if (!f) return;
	while (fscanf(f, "%d", &n) == 1) {
		X32command *p;
		if (x32_read_lstr(f, name, 128) < 0) break;
		if (x32_read_lstr(f, notes, 256) < 0) break;
		if (fscanf(f, "%d %d", &safes, &has) != 2) break;
		p = x32_scene_at(n);
		if (!p) continue;
		x32_set_str(p + 1, name);
		x32_set_str(p + 2, notes);
		(p + 3)->value.ii = safes;
		(p + 4)->value.ii = has;
	}
	fclose(f);
}

static void x32_save_snips_idx(void) {
	FILE *f;
	int n;
	if (x32_proj_on) {
		x32_save_shw();
		return;
	}
	f = fopen(X32DATA "/snippets.idx.tmp", "w");
	if (!f) return;
	for (n = 0; n < 100; n++) {
		X32command *p = x32_snip_at(n);
		if (!p) break;
		fprintf(f, "%d ", n);
		x32_write_lstr(f, (p + 1)->value.str);
		fprintf(f, " %d %d %d %d %d\n",
			(p + 2)->value.ii, (p + 3)->value.ii, (p + 4)->value.ii,
			(p + 5)->value.ii, (p + 6)->value.ii);
	}
	fclose(f);
	x32_commit(X32DATA "/snippets.idx", X32DATA "/snippets.idx.tmp");
}

static void x32_load_snips_idx(void) {
	FILE *f;
	char name[128];
	int n, ev, ch, ax, mg, has;
	f = fopen(X32DATA "/snippets.idx", "r");
	if (!f) return;
	while (fscanf(f, "%d", &n) == 1) {
		X32command *p;
		if (x32_read_lstr(f, name, 128) < 0) break;
		if (fscanf(f, "%d %d %d %d %d", &ev, &ch, &ax, &mg, &has) != 5) break;
		p = x32_snip_at(n);
		if (!p) continue;
		x32_set_str(p + 1, name);
		(p + 2)->value.ii = ev;
		(p + 3)->value.ii = ch;
		(p + 4)->value.ii = ax;
		(p + 5)->value.ii = mg;
		(p + 6)->value.ii = has;
	}
	fclose(f);
}

static int x32_cue_used(X32command *p) {
	if ((p + 2)->value.str && (p + 2)->value.str[0]) return 1;
	if ((p + 1)->value.ii != 0) return 1;
	return 0;
}

static void x32_save_cues_idx(void) {
	FILE *f;
	int n;
	if (x32_proj_on) {
		x32_save_shw();
		return;
	}
	f = fopen(X32DATA "/cues.idx.tmp", "w");
	if (!f) return;
	for (n = 0; n < 500; n++) {
		X32command *p = x32_cue_at(n);
		if (!p) break;
		if (!x32_cue_used(p)) continue;
		fprintf(f, "%d %d ", n, (p + 1)->value.ii);
		x32_write_lstr(f, (p + 2)->value.str);
		fprintf(f, " %d %d %d %d %d %d %d\n",
			(p + 3)->value.ii, (p + 4)->value.ii, (p + 5)->value.ii,
			(p + 6)->value.ii, (p + 7)->value.ii, (p + 8)->value.ii,
			(p + 9)->value.ii);
	}
	fclose(f);
	x32_commit(X32DATA "/cues.idx", X32DATA "/cues.idx.tmp");
}

static void x32_load_cues_idx(void) {
	FILE *f;
	char name[128];
	int n, numb, skip, scene, snip, mt, mc, p1, p2;
	f = fopen(X32DATA "/cues.idx", "r");
	if (!f) return;
	while (fscanf(f, "%d %d", &n, &numb) == 2) {
		X32command *p;
		if (x32_read_lstr(f, name, 128) < 0) break;
		if (fscanf(f, "%d %d %d %d %d %d %d", &skip, &scene, &snip, &mt, &mc, &p1, &p2) != 7)
			break;
		p = x32_cue_at(n);
		if (!p) continue;
		(p + 1)->value.ii = numb;
		x32_set_str(p + 2, name);
		(p + 3)->value.ii = skip;
		(p + 4)->value.ii = scene;
		(p + 5)->value.ii = snip;
		(p + 6)->value.ii = mt;
		(p + 7)->value.ii = mc;
		(p + 8)->value.ii = p1;
		(p + 9)->value.ii = p2;
	}
	fclose(f);
}

static void x32_save_libs_idx(void) {
	FILE *f;
	int kind, n;
	const char *kn[] = {"ch", "fx", "rt"};
	if (x32_proj_on) return;
	f = fopen(X32DATA "/libs.idx.tmp", "w");
	if (!f) return;
	for (kind = 0; kind < 3; kind++) {
		for (n = 0; n < 100; n++) {
			X32command *p = x32_lib_at(kind, n);
			if (!p) break;
			fprintf(f, "%s %d %d %d %d %d ", kn[kind], n,
				(p + 1)->value.ii, (p + 3)->value.ii, (p + 4)->value.ii, (p + 5)->value.ii);
			x32_write_lstr(f, (p + 2)->value.str);
			fputc('\n', f);
		}
	}
	fclose(f);
	x32_commit(X32DATA "/libs.idx", X32DATA "/libs.idx.tmp");
}

static void x32_load_libs_idx(void) {
	FILE *f;
	char kind[8], name[128];
	int n, pos, type, flags, has, k;
	f = fopen(X32DATA "/libs.idx", "r");
	if (!f) return;
	while (fscanf(f, "%7s %d %d %d %d %d", kind, &n, &pos, &type, &flags, &has) == 6) {
		X32command *p;
		if (x32_read_lstr(f, name, 128) < 0) break;
		k = 0;
		if (strcmp(kind, "fx") == 0) k = 1;
		else if (strcmp(kind, "rt") == 0) k = 2;
		p = x32_lib_at(k, n);
		if (!p) continue;
		(p + 1)->value.ii = pos;
		x32_set_str(p + 2, name);
		(p + 3)->value.ii = type;
		(p + 4)->value.ii = flags;
		(p + 5)->value.ii = has;
	}
	fclose(f);
}

static void x32_sync_lists(void) {
	x32_save_show();
	x32_save_scenes_idx();
	x32_save_snips_idx();
	x32_save_cues_idx();
	x32_save_libs_idx();
	x32_save_recall();
}

static int x32_strip_id(const char *p, const char *pre, int w, int *num) {
	int i, n = 0;
	int pl = (int)strlen(pre);
	if (strncmp(p, pre, pl) != 0) return 0;
	p += pl;
	for (i = 0; i < w; i++) {
		if (p[i] < '0' || p[i] > '9') return 0;
		n = n * 10 + (p[i] - '0');
	}
	*num = n;
	return 1;
}

/* Show-level channel safes plus the 9 scene-safe bits. Return 1 to skip. */
static int x32_scene_skip(const char *p, int safes, int idx) {
	int n;
	if (!p || p[0] != '/') return 1;
	if (x32_strip_id(p, "/ch/", 2, &n) && n >= 1 && n <= 32) {
		if (x32_bit(Xshow[5].value.ii, n - 1)) return 1;
		if (n <= 16 && x32_bit(Xshow[9].value.ii, n - 1)) return 1;
		if (n > 16 && x32_bit(Xshow[10].value.ii, n - 17)) return 1;
	}
	if (x32_strip_id(p, "/fxrtn/", 2, &n) && x32_bit(Xshow[11].value.ii, n - 1)) return 1;
	if (x32_strip_id(p, "/bus/", 2, &n) && x32_bit(Xshow[12].value.ii, n - 1)) return 1;
	if (x32_strip_id(p, "/mtx/", 2, &n) && x32_bit(Xshow[13].value.ii, n - 1)) return 1;
	if (strncmp(p, "/main/st", 8) == 0 && x32_bit(Xshow[13].value.ii, 6)) return 1;
	if (strncmp(p, "/main/m", 7) == 0 && x32_bit(Xshow[13].value.ii, 7)) return 1;
	if (x32_strip_id(p, "/dca/", 1, &n) && x32_bit(Xshow[13].value.ii, 7 + n)) return 1;
	if (x32_strip_id(p, "/fx/", 1, &n) && x32_bit(Xshow[14].value.ii, n - 1)) return 1;
	if (idx == 0) safes = 0;
	if ((safes & 0x2) && strstr(p, "/talk")) return 1;
	if ((safes & 0x4) && strncmp(p, "/fx/", 4) == 0) return 1;
	if ((safes & 0x8) && strncmp(p, "/bus/", 5) == 0) return 1;
	if (safes & 0x10) {
		if (strncmp(p, "/ch/", 4) == 0 || strncmp(p, "/auxin/", 7) == 0 || strncmp(p, "/fxrtn/", 7) == 0) {
			if (!strstr(p, "/preamp") && !strstr(p, "/delay") && !strstr(p, "/config")) return 1;
		}
	}
	if ((safes & 0x20) && strncmp(p, "/config", 7) == 0 && !strstr(p, "/routing")) return 1;
	if ((safes & 0x40) && (strncmp(p, "/headamp/", 9) == 0 || strstr(p, "/preamp") || strstr(p, "/delay")))
		return 1;
	if ((safes & 0x80) && strncmp(p, "/outputs", 8) == 0) return 1;
	if ((safes & 0x100) && strstr(p, "/routing")) return 1;
	return 0;
}

static int x32_ev_of(const char *p) {
	const char *s;
	int n;
	if (strstr(p, "/preamp") || strstr(p, "/delay") || strncmp(p, "/headamp/", 9) == 0) return 0;
	if (strstr(p, "/eq")) return 2;
	if (strstr(p, "/gate") || strstr(p, "/dyn")) return 3;
	if (strstr(p, "/insert")) return 4;
	if (strstr(p, "/grp")) return 5;
	if (strstr(p, "/mix/fader") || strstr(p, "/mix/pan") || strstr(p, "/mix/st") || strstr(p, "/mix/mono"))
		return 6;
	if (strstr(p, "/mix/on")) return 7;
	if (strstr(p, "/mix/mlevel")) return 11;
	s = strstr(p, "/mix/");
	if (s && s[5] >= '0' && s[6] >= '0' && (s[7] == '/' || s[7] == 0)) {
		n = (s[5] - '0') * 10 + (s[6] - '0');
		if (n >= 1 && n <= 8) return 8;
		if (n >= 9 && n <= 12) return 9;
		if (n >= 13 && n <= 16) return 10;
	}
	if (strncmp(p, "/config/solo", 12) == 0) return 22;
	if (strncmp(p, "/config/routing", 15) == 0) return 23;
	if (strncmp(p, "/config", 7) == 0) return 21;
	if (strncmp(p, "/outputs", 8) == 0) return 24;
	if (strncmp(p, "/fx/", 4) == 0 && p[4] >= '1' && p[4] <= '8') return 12 + (p[4] - '0');
	if (strstr(p, "/config")) return 1;
	return -1;
}

static int x32_strip_on(const char *p, int ch, int ax, int mg) {
	int n;
	if (strncmp(p, "/config", 7) == 0 || strncmp(p, "/outputs", 8) == 0 || strncmp(p, "/fx/", 4) == 0)
		return 1;
	if (strncmp(p, "/headamp/", 9) == 0) {
		n = (p[9] - '0') * 100 + (p[10] - '0') * 10 + (p[11] - '0');
		if (n >= 0 && n < 32) return x32_bit(ch, n);
		return 0;
	}
	if (x32_strip_id(p, "/ch/", 2, &n)) return (n >= 1 && n <= 32 && x32_bit(ch, n - 1));
	if (x32_strip_id(p, "/auxin/", 2, &n)) return x32_bit(ax, n - 1);
	if (x32_strip_id(p, "/fxrtn/", 2, &n)) return x32_bit(ax, 7 + n);
	if (x32_strip_id(p, "/bus/", 2, &n)) return x32_bit(ax, 15 + n);
	if (x32_strip_id(p, "/mtx/", 2, &n)) return x32_bit(mg, n - 1);
	if (strncmp(p, "/main/st", 8) == 0) return x32_bit(mg, 6);
	if (strncmp(p, "/main/m", 7) == 0) return x32_bit(mg, 7);
	if (x32_strip_id(p, "/dca/", 1, &n)) return x32_bit(mg, 7 + n);
	return 0;
}

static int x32_snip_keep(const char *p, int ev, int ch, int ax, int mg) {
	int e;
	if (!p || !x32_strip_on(p, ch, ax, mg)) return 0;
	e = x32_ev_of(p);
	if (e < 0) return 0;
	return x32_bit(ev, e);
}

static void x32_wr_val(FILE *f, X32command *c) {
	if (c->format.typ == S32) {
		int n = (c->value.str) ? (int)strlen(c->value.str) : 0;
		fprintf(f, "S %d ", n);
		if (n) fwrite(c->value.str, 1, n, f);
		fputc('\n', f);
	} else {
		fprintf(f, "I %d\n", c->value.ii);
	}
}

static int x32_rd_val(FILE *f, X32command *c, int apply) {
	char k;
	int n, i, ch, keep;
	char buf[256];
	if (fscanf(f, " %c %d", &k, &n) != 2) return 0;
	if (k == 'S') {
		if (n < 0) n = 0;
		fgetc(f);
		keep = n;
		if (keep > 255) keep = 255;
		for (i = 0; i < keep; i++) {
			ch = fgetc(f);
			buf[i] = (ch < 0) ? 0 : (char)ch;
		}
		buf[keep] = 0;
		for (i = keep; i < n; i++) fgetc(f);
		fgetc(f);
		if (apply && c) x32_set_str(c, buf);
	} else if (apply && c) {
		c->value.ii = n;
	}
	return 1;
}

static void x32_for_engine(void (*fn)(X32command *, int, void *), void *ctx) {
	int j;
	fn(Xconfig, Xconfig_max, ctx);
	fn(Xmain, Xmain_max, ctx);
	fn(Xchannelset[0], Xchannel01_max, ctx);
	for (j = 1; j < 32; j++) fn(Xchannelset[j], Xchannel02_max, ctx);
	fn(Xauxinset[0], Xauxin01_max, ctx);
	for (j = 1; j < 8; j++) fn(Xauxinset[j], Xauxin02_max, ctx);
	fn(Xfxrtnset[0], Xfxrtn01_max, ctx);
	for (j = 1; j < 8; j++) fn(Xfxrtnset[j], Xfxrtn02_max, ctx);
	fn(Xbusset[0], Xbus01_max, ctx);
	for (j = 1; j < 16; j++) fn(Xbusset[j], Xbus02_max, ctx);
	fn(Xmtxset[0], Xmtx01_max, ctx);
	for (j = 1; j < 6; j++) fn(Xmtxset[j], Xmtx02_max, ctx);
	fn(Xdca, Xdca_max, ctx);
	fn(Xfxset[0], Xfx1_max, ctx);
	for (j = 1; j < 4; j++) fn(Xfxset[j], Xfx2_max, ctx);
	for (j = 4; j < 8; j++) fn(Xfxset[j], Xfx5_max, ctx);
	fn(Xoutput, Xoutput_max, ctx);
	fn(Xheadmpset[0], Xheadamp_max, ctx);
	for (j = 1; j < 128; j++) fn(Xheadmpset[j], Xheadamp1_max, ctx);
}

typedef struct {
	FILE *f;
	int mode;
	int safes;
	int idx;
	int ev, ch, ax, mg;
	int lines;
} X32walk;

static void x32_walk_arr(X32command *a, int n, void *v) {
	X32walk *w = (X32walk *)v;
	int i, count;
	if (w->mode < 0) return;
	if (w->mode == 0) {
		fprintf(w->f, "A %d\n", n);
		for (i = 0; i < n; i++) x32_wr_val(w->f, &a[i]);
	} else if (w->mode == 1) {
		if (fscanf(w->f, " A %d", &count) != 1 || count != n) {
			w->mode = -1;
			return;
		}
		for (i = 0; i < n; i++) {
			int apply = !x32_scene_skip(a[i].command, w->safes, w->idx);
			if (!x32_rd_val(w->f, &a[i], apply)) {
				w->mode = -1;
				return;
			}
		}
	} else if (w->mode == 2) {
		for (i = 0; i < n; i++) {
			if (a[i].flags & F_FND) continue;
			if (!x32_snip_keep(a[i].command, w->ev, w->ch, w->ax, w->mg)) continue;
			fprintf(w->f, "P %s\n", a[i].command);
			x32_wr_val(w->f, &a[i]);
			w->lines++;
		}
	}
}

static int x32_write_scene_body(int idx) {
	X32walk w;
	char path[80], tmp[88];
	FILE *f;
	if (x32_proj_on) return x32_write_console_scene(idx);
	sprintf(path, X32DATA "/scene/%03d.scn", idx);
	sprintf(tmp, X32DATA "/scene/%03d.scn.tmp", idx);
	f = fopen(tmp, "w");
	if (!f) return 0;
	fprintf(f, "#X32SCENE\n");
	w.f = f;
	w.mode = 0;
	w.safes = 0;
	w.idx = idx;
	w.lines = 0;
	x32_for_engine(x32_walk_arr, &w);
	fclose(f);
	x32_commit(path, tmp);
	return 1;
}

static int x32_read_scene_body(int idx, int safes) {
	X32walk w;
	char path[80], line[64];
	FILE *f;
	if (x32_proj_on) return x32_apply_console_file(x32_scene_path[idx], safes, idx);
	sprintf(path, X32DATA "/scene/%03d.scn", idx);
	f = fopen(path, "r");
	if (!f) return 0;
	if (!fgets(line, 64, f) || strncmp(line, "#X32SCENE", 9) != 0) {
		fclose(f);
		return 0;
	}
	w.f = f;
	w.mode = 1;
	w.safes = safes;
	w.idx = idx;
	w.lines = 0;
	x32_for_engine(x32_walk_arr, &w);
	fclose(f);
	return w.mode == 1;
}

static X32command *x32_scan(X32command *a, int n, const char *path) {
	int i;
	for (i = 0; i < n; i++) {
		if (strcmp(a[i].command, path) == 0) return &a[i];
	}
	return NULL;
}

static X32command *x32_find_param(const char *path) {
	int n, m;
	if (strncmp(path, "/ch/", 4) == 0 && path[4] && path[5]) {
		n = (path[4] - '0') * 10 + (path[5] - '0');
		if (n >= 1 && n <= 32)
			return x32_scan(Xchannelset[n - 1], (n == 1) ? Xchannel01_max : Xchannel02_max, path);
	}
	if (strncmp(path, "/auxin/", 7) == 0) {
		n = (path[7] - '0') * 10 + (path[8] - '0');
		if (n >= 1 && n <= 8)
			return x32_scan(Xauxinset[n - 1], (n == 1) ? Xauxin01_max : Xauxin02_max, path);
	}
	if (strncmp(path, "/fxrtn/", 7) == 0) {
		n = (path[7] - '0') * 10 + (path[8] - '0');
		if (n >= 1 && n <= 8)
			return x32_scan(Xfxrtnset[n - 1], (n == 1) ? Xfxrtn01_max : Xfxrtn02_max, path);
	}
	if (strncmp(path, "/bus/", 5) == 0) {
		n = (path[5] - '0') * 10 + (path[6] - '0');
		if (n >= 1 && n <= 16)
			return x32_scan(Xbusset[n - 1], (n == 1) ? Xbus01_max : Xbus02_max, path);
	}
	if (strncmp(path, "/mtx/", 5) == 0) {
		n = (path[5] - '0') * 10 + (path[6] - '0');
		if (n >= 1 && n <= 6)
			return x32_scan(Xmtxset[n - 1], (n == 1) ? Xmtx01_max : Xmtx02_max, path);
	}
	if (strncmp(path, "/fx/", 4) == 0 && path[4] >= '1' && path[4] <= '8') {
		n = path[4] - '0';
		m = (n == 1) ? Xfx1_max : ((n <= 4) ? Xfx2_max : Xfx5_max);
		return x32_scan(Xfxset[n - 1], m, path);
	}
	if (strncmp(path, "/headamp/", 9) == 0) {
		n = (path[9] - '0') * 100 + (path[10] - '0') * 10 + (path[11] - '0');
		if (n >= 0 && n < 128)
			return x32_scan(Xheadmpset[n], (n == 0) ? Xheadamp_max : Xheadamp1_max, path);
	}
	if (strncmp(path, "/dca/", 5) == 0) return x32_scan(Xdca, Xdca_max, path);
	if (strncmp(path, "/outputs", 8) == 0) return x32_scan(Xoutput, Xoutput_max, path);
	if (strncmp(path, "/config", 7) == 0) return x32_scan(Xconfig, Xconfig_max, path);
	if (strncmp(path, "/main", 5) == 0) return x32_scan(Xmain, Xmain_max, path);
	return NULL;
}

static const char *x32_after_strip(const char *p) {
	int sl = 0;
	const char *s = p;
	while (*s) {
		if (*s == '/') {
			sl++;
			if (sl == 3) return s;
		}
		s++;
	}
	return NULL;
}

static void x32_note_recall(int kind, int idx) {
	int sc = x32_show_control();
	int moved = 0;
	if (kind == 1 && idx >= 0 && idx <= 99) x32_scene_idx = idx;
	if (kind == 2 && idx >= 0 && idx <= 99) x32_snippet_idx = idx;
	if (kind == 0 && idx >= 0 && idx <= 499) x32_cue_idx = idx;
	if (sc == kind) {
		if (Xshow[0].value.ii != idx) {
			Xshow[0].value.ii = idx;
			moved = 1;
		}
	}
	x32_save_recall();
	s_len = Xfprint(s_buf, 0, "/-stat/scene/index", 'i', &x32_scene_idx);
	Xsend(S_REM);
	Xreply_now();
	s_len = Xfprint(s_buf, 0, "/-stat/snippet/index", 'i', &x32_snippet_idx);
	Xsend(S_REM);
	Xreply_now();
	if (moved) x32_publish(&Xshow[0]);
}

static int x32_recall_scene(int idx) {
	X32command *p = x32_scene_at(idx);
	int safes;
	if (!p || (p + 4)->value.ii == 0) return 0;
	safes = (p + 3)->value.ii;
	if (!x32_read_scene_body(idx, safes)) return 0;
	x32_note_recall(1, idx);
	return 1;
}

static int x32_save_scene(int idx, const char *name, const char *notes, int safes, int have_safes) {
	X32command *p = x32_scene_at(idx);
	char nm[64], nt[128];
	if (!p) return 0;
	x32_copy_clean(nm, name, 64);
	x32_copy_clean(nt, notes, 128);
	x32_set_str(p + 1, nm);
	x32_set_str(p + 2, nt);
	if (have_safes) (p + 3)->value.ii = safes;
	if (!x32_write_scene_body(idx)) return 0;
	(p + 4)->value.ii = 1;
	x32_scene_listed[idx] = 1;
	x32_publish(p + 1);
	x32_publish(p + 2);
	x32_publish(p + 3);
	x32_publish(p + 4);
	x32_save_scenes_idx();
	x32_note_recall(1, idx);
	return 1;
}

static int x32_write_snip_body(int idx) {
	X32walk w;
	X32command *p = x32_snip_at(idx);
	char path[80], tmp[88];
	FILE *f;
	if (!p) return 0;
	if (x32_proj_on) return x32_write_console_snip(idx);
	sprintf(path, X32DATA "/snippet/%03d.snp", idx);
	sprintf(tmp, X32DATA "/snippet/%03d.snp.tmp", idx);
	f = fopen(tmp, "w");
	if (!f) return 0;
	fprintf(f, "#X32SNIPPET %d %d %d %d\n",
		(p + 2)->value.ii, (p + 3)->value.ii, (p + 4)->value.ii, (p + 5)->value.ii);
	w.f = f;
	w.mode = 2;
	w.ev = (p + 2)->value.ii;
	w.ch = (p + 3)->value.ii;
	w.ax = (p + 4)->value.ii;
	w.mg = (p + 5)->value.ii;
	w.lines = 0;
	x32_for_engine(x32_walk_arr, &w);
	fclose(f);
	x32_commit(path, tmp);
	return 1;
}

static int x32_apply_path_file(FILE *f, int kind, int dest, int mask) {
	char line[160];
	char built[160];
	while (fgets(line, sizeof(line), f)) {
		X32command *c;
		char *path;
		const char *tail;
		int n;
		if (line[0] != 'P') continue;
		path = line + 2;
		n = (int)strlen(path);
		while (n > 0 && (path[n - 1] == '\n' || path[n - 1] == '\r')) path[--n] = 0;
		built[0] = 0;
		if (kind >= 0 && dest >= 0) {
			tail = x32_after_strip(path);
			if (strncmp(path, "/headamp/", 9) == 0) {
				if (kind == 0 && dest < 32 && tail)
					sprintf(built, "/headamp/%03d%s", dest, tail);
				else {
					if (!x32_rd_val(f, NULL, 0)) return 0;
					continue;
				}
			} else if (kind == 0 && tail) {
				if (dest < 32) sprintf(built, "/ch/%02d%s", dest + 1, tail);
				else if (dest < 40) sprintf(built, "/auxin/%02d%s", dest - 31, tail);
				else if (dest < 48) sprintf(built, "/fxrtn/%02d%s", dest - 39, tail);
				else if (dest < 64) sprintf(built, "/bus/%02d%s", dest - 47, tail);
				else if (dest < 70) sprintf(built, "/mtx/%02d%s", dest - 63, tail);
				else strcpy(built, path);
			} else if (kind == 1 && tail) {
				sprintf(built, "/fx/%d%s", dest + 1, tail);
			} else {
				strcpy(built, path);
			}
			if (kind == 0 && mask != 0x3f) {
				const char *t = tail ? tail : path;
				int ok = 0;
				if ((strstr(t, "/preamp") || strstr(t, "/delay") || strstr(t, "/headamp")) && (mask & C_HA)) ok = 1;
				else if (strstr(t, "/gate") && (mask & C_GATE)) ok = 1;
				else if (strstr(t, "/dyn") && (mask & C_DYN)) ok = 1;
				else if (strstr(t, "/eq") && (mask & C_EQ)) ok = 1;
				else if (strstr(t, "/mix") && (mask & C_SEND)) ok = 1;
				else if ((strstr(t, "/config") || strstr(t, "/grp") || strstr(t, "/insert")) && (mask & C_CONFIG)) ok = 1;
				if (!ok) {
					if (!x32_rd_val(f, NULL, 0)) return 0;
					continue;
				}
			}
			path = built[0] ? built : path;
		}
		c = x32_find_param(path);
		if (!x32_rd_val(f, c, c != NULL)) return 0;
	}
	return 1;
}

static int x32_recall_snip(int idx) {
	X32command *p = x32_snip_at(idx);
	char path[80];
	FILE *f;
	if (!p || (p + 6)->value.ii == 0) return 0;
	if (x32_proj_on) {
		if (!x32_apply_console_file(x32_snip_path[idx], -1, idx)) return 0;
		x32_note_recall(2, idx);
		return 1;
	}
	sprintf(path, X32DATA "/snippet/%03d.snp", idx);
	f = fopen(path, "r");
	if (!f) return 0;
	if (!x32_apply_path_file(f, -1, -1, 0)) {
		fclose(f);
		return 0;
	}
	fclose(f);
	x32_note_recall(2, idx);
	return 1;
}

static int x32_save_snip(int idx, const char *name) {
	X32command *p = x32_snip_at(idx);
	char nm[64];
	if (!p) return 0;
	x32_copy_clean(nm, name, 64);
	x32_set_str(p + 1, nm);
	if (!x32_write_snip_body(idx)) return 0;
	(p + 6)->value.ii = 1;
	x32_snip_listed[idx] = 1;
	x32_publish(p + 1);
	x32_publish(p + 6);
	x32_save_snips_idx();
	x32_note_recall(2, idx);
	return 1;
}

static void x32_clear_scene(int n) {
	X32command *p = x32_scene_at(n);
	char path[80];
	if (!p) return;
	x32_set_str(p + 1, NULL);
	x32_set_str(p + 2, NULL);
	(p + 3)->value.ii = 0;
	(p + 4)->value.ii = 0;
	x32_scene_listed[n] = 0;
	if (x32_proj_on) {
		if (x32_unlink_bodies && x32_scene_path[n][0]) remove(x32_scene_path[n]);
		x32_scene_path[n][0] = 0;
	} else {
		sprintf(path, X32DATA "/scene/%03d.scn", n);
		remove(path);
	}
	x32_publish(p + 1);
	x32_publish(p + 2);
	x32_publish(p + 4);
}

static void x32_clear_snip(int n) {
	X32command *p = x32_snip_at(n);
	char path[80];
	if (!p) return;
	x32_set_str(p + 1, NULL);
	(p + 6)->value.ii = 0;
	x32_snip_listed[n] = 0;
	if (x32_proj_on) {
		if (x32_unlink_bodies && x32_snip_path[n][0]) remove(x32_snip_path[n]);
		x32_snip_path[n][0] = 0;
	} else {
		sprintf(path, X32DATA "/snippet/%03d.snp", n);
		remove(path);
	}
	x32_publish(p + 1);
	x32_publish(p + 6);
}

static const char *x32_lib_dir(int kind) {
	if (kind == 1) return X32DATA "/libfx";
	if (kind == 2) return X32DATA "/librt";
	return X32DATA "/libch";
}

static int x32_save_lib_body(int kind, int idx, int source) {
	char path[96], tmp[104];
	FILE *f;
	X32command *arr = NULL;
	int nmax = 0;
	if (x32_proj_on) return 0;
	sprintf(path, "%s/%03d.pre", x32_lib_dir(kind), idx);
	sprintf(tmp, "%s.tmp", path);
	f = fopen(tmp, "w");
	if (!f) return 0;
	fprintf(f, "#X32PRE %d %d\n", kind, source);
	if (kind == 0 && source >= 0 && source < 32) {
		arr = Xchannelset[source];
		nmax = (source == 0) ? Xchannel01_max : Xchannel02_max;
	} else if (kind == 1 && source >= 0 && source < 8) {
		arr = Xfxset[source];
		nmax = (source == 0) ? Xfx1_max : ((source < 4) ? Xfx2_max : Xfx5_max);
	}
	if (arr) {
		int i;
		for (i = 0; i < nmax; i++) {
			if (arr[i].flags & F_FND) continue;
			fprintf(f, "P %s\n", arr[i].command);
			x32_wr_val(f, &arr[i]);
		}
	}
	if (kind == 0 && source >= 0 && source < 32) {
		X32command *h = Xheadmpset[source];
		int hn = (source == 0) ? Xheadamp_max : Xheadamp1_max;
		int i;
		for (i = 0; i < hn; i++) {
			if (h[i].flags & F_FND) continue;
			fprintf(f, "P %s\n", h[i].command);
			x32_wr_val(f, &h[i]);
		}
	}
	if (kind == 2) {
		int i;
		for (i = 0; i < Xconfig_max; i++) {
			if (Xconfig[i].flags & F_FND) continue;
			if (!strstr(Xconfig[i].command, "/routing")) continue;
			fprintf(f, "P %s\n", Xconfig[i].command);
			x32_wr_val(f, &Xconfig[i]);
		}
		for (i = 0; i < Xoutput_max; i++) {
			if (Xoutput[i].flags & F_FND) continue;
			fprintf(f, "P %s\n", Xoutput[i].command);
			x32_wr_val(f, &Xoutput[i]);
		}
	}
	fclose(f);
	x32_commit(path, tmp);
	return 1;
}

static int x32_recall_lib(int kind, int idx, int dest, int have_dest, int mask) {
	X32command *p = x32_lib_at(kind, idx);
	char path[96];
	FILE *f;
	int source;
	if (!p || (p + 5)->value.ii == 0) return 0;
	sprintf(path, "%s/%03d.pre", x32_lib_dir(kind), idx);
	f = fopen(path, "r");
	if (!f) return 0;
	source = (p + 3)->value.ii;
	if (!have_dest) dest = source;
	if (kind == 2) dest = -1;
	if (!x32_apply_path_file(f, kind, dest, mask)) {
		fclose(f);
		return 0;
	}
	fclose(f);
	return 1;
}

static int x32_save_lib(int kind, int idx, const char *name, int source) {
	X32command *p = x32_lib_at(kind, idx);
	char nm[64];
	if (!p) return 0;
	x32_copy_clean(nm, name, 64);
	if (!x32_save_lib_body(kind, idx, source)) return 0;
	(p + 1)->value.ii = idx + 1;
	x32_set_str(p + 2, nm);
	(p + 3)->value.ii = source;
	(p + 4)->value.ii = (kind == 0) ? 0x3F00 : 1;
	(p + 5)->value.ii = 1;
	x32_publish(p + 2);
	x32_publish(p + 5);
	x32_save_libs_idx();
	return 1;
}

static void x32_clear_lib(int kind, int n) {
	X32command *p = x32_lib_at(kind, n);
	char path[96];
	if (!p) return;
	x32_set_str(p + 2, NULL);
	(p + 3)->value.ii = 0;
	(p + 4)->value.ii = 0;
	(p + 5)->value.ii = 0;
	sprintf(path, "%s/%03d.pre", x32_lib_dir(kind), n);
	remove(path);
	x32_publish(p + 2);
	x32_publish(p + 5);
}

static int x32_copy_file(const char *src, const char *dst) {
	FILE *in, *out;
	char buf[2048];
	int n;
	in = fopen(src, "rb");
	if (!in) return 0;
	out = fopen(dst, "wb");
	if (!out) {
		fclose(in);
		return 0;
	}
	while ((n = (int)fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
	fclose(in);
	fclose(out);
	return 1;
}

static void x32_copy_val(X32command *d, X32command *s) {
	if (s->format.typ == S32) x32_set_str(d, s->value.str);
	else d->value.ii = s->value.ii;
}

static void x32_copy_range(X32command *d, X32command *s, int a, int b, int nmax) {
	int i;
	if (b > nmax) b = nmax;
	for (i = a; i < b; i++) x32_copy_val(&d[i], &s[i]);
}

static int x32_copy_scene(int src, int dst) {
	X32command *a = x32_scene_at(src);
	X32command *b = x32_scene_at(dst);
	char ps[80], pd[80];
	if (!a || !b) return 0;
	x32_set_str(b + 1, (a + 1)->value.str);
	x32_set_str(b + 2, (a + 2)->value.str);
	(b + 3)->value.ii = (a + 3)->value.ii;
	(b + 4)->value.ii = (a + 4)->value.ii;
	if (x32_proj_on) {
		if ((a + 4)->value.ii && x32_scene_path[src][0]) {
			if (!x32_scene_path[dst][0]) {
				char nm[64];
				int okp;
				x32_copy_clean(nm, (b + 1)->value.str, 60);
				x32_sanitize_file(nm);
				if (nm[0])
					okp = x32_numbered(x32_scene_path[dst], X32_PATHMAX, x32_proj_dir, "%03d %.60s.scn", dst, nm);
				else
					okp = x32_numbered(x32_scene_path[dst], X32_PATHMAX, x32_proj_dir, "%03d.scn", dst, "");
				if (!okp) x32_scene_path[dst][0] = 0;
			}
			if (!x32_copy_file(x32_scene_path[src], x32_scene_path[dst])) {
				(b + 4)->value.ii = 0;
				x32_scene_path[dst][0] = 0;
			}
		} else {
			if (x32_unlink_bodies && x32_scene_path[dst][0]) remove(x32_scene_path[dst]);
			x32_scene_path[dst][0] = 0;
		}
		x32_scene_listed[dst] = 1;
	} else {
		sprintf(ps, X32DATA "/scene/%03d.scn", src);
		sprintf(pd, X32DATA "/scene/%03d.scn", dst);
		if ((a + 4)->value.ii) {
			if (!x32_copy_file(ps, pd)) (b + 4)->value.ii = 0;
		} else {
			remove(pd);
		}
	}
	x32_publish(b + 1);
	x32_publish(b + 4);
	x32_save_scenes_idx();
	return 1;
}

static int x32_copy_lib(int kind, int src, int dst) {
	X32command *a = x32_lib_at(kind, src);
	X32command *b = x32_lib_at(kind, dst);
	char ps[96], pd[96];
	if (!a || !b) return 0;
	x32_set_str(b + 2, (a + 2)->value.str);
	(b + 1)->value.ii = dst + 1;
	(b + 3)->value.ii = (a + 3)->value.ii;
	(b + 4)->value.ii = (a + 4)->value.ii;
	(b + 5)->value.ii = (a + 5)->value.ii;
	sprintf(ps, "%s/%03d.pre", x32_lib_dir(kind), src);
	sprintf(pd, "%s/%03d.pre", x32_lib_dir(kind), dst);
	if ((a + 5)->value.ii) {
		if (!x32_copy_file(ps, pd)) (b + 5)->value.ii = 0;
	} else {
		remove(pd);
	}
	x32_publish(b + 2);
	x32_publish(b + 5);
	x32_save_libs_idx();
	return 1;
}

static int x32_kind_of(const char *s) {
	if (strcmp(s, "scene") == 0) return 10;
	if (strcmp(s, "snippet") == 0) return 11;
	if (strcmp(s, "libchan") == 0) return 0;
	if (strcmp(s, "libfx") == 0) return 1;
	if (strcmp(s, "librout") == 0) return 2;
	if (strcmp(s, "cue") == 0) return 12;
	return -1;
}

static void x32_init_show(void) {
	int n;
	x32_unlink_bodies = 0;
	x32_set_str(&Xshow[4], NULL);
	for (n = 0; n < 10; n++) Xshow[5 + n].value.ii = 0;
	Xshow[0].value.ii = 0;
	for (n = 0; n < 100; n++) {
		x32_clear_scene(n);
		x32_clear_snip(n);
	}
	for (n = 0; n < 500; n++) {
		X32command *p = x32_cue_at(n);
		if (!p) break;
		(p + 1)->value.ii = 0;
		x32_set_str(p + 2, NULL);
		(p + 3)->value.ii = 0;
		(p + 4)->value.ii = 0;
		(p + 5)->value.ii = 0;
	}
	x32_scene_idx = 0;
	x32_snippet_idx = 0;
	x32_cue_idx = 0;
	x32_unlink_bodies = 1;
	x32_sync_lists();
}

static void x32_init_libs(void) {
	int k, n;
	for (k = 0; k < 3; k++) {
		for (n = 0; n < 100; n++) x32_clear_lib(k, n);
	}
	x32_save_libs_idx();
}

static int x32_add_cue(int numb, const char *name) {
	int n;
	char nm[64];
	X32command *p = NULL;
	x32_copy_clean(nm, name, 64);
	for (n = 0; n < 500; n++) {
		p = x32_cue_at(n);
		if (p && !x32_cue_used(p)) break;
		p = NULL;
	}
	if (!p) return 0;
	(p + 1)->value.ii = numb;
	x32_set_str(p + 2, nm);
	(p + 3)->value.ii = 0;
	(p + 4)->value.ii = x32_scene_idx;
	(p + 5)->value.ii = x32_snippet_idx;
	(p + 6)->value.ii = 0;
	(p + 7)->value.ii = 1;
	(p + 8)->value.ii = 0;
	(p + 9)->value.ii = 0;
	x32_publish(p + 2);
	x32_save_cues_idx();
	x32_note_recall(0, n);
	return 1;
}

static int x32_go_cue(int idx) {
	X32command *p = x32_cue_at(idx);
	int scene, snip, ok = 1;
	if (!p || !x32_cue_used(p)) return 0;
	scene = (p + 4)->value.ii;
	snip = (p + 5)->value.ii;
	x32_note_recall(0, idx);
	if (scene >= 0 && scene <= 99 && x32_scene_at(scene) && (x32_scene_at(scene) + 4)->value.ii)
		ok = x32_recall_scene(scene);
	if (snip >= 0 && snip <= 99 && x32_snip_at(snip) && (x32_snip_at(snip) + 6)->value.ii)
		ok = x32_recall_snip(snip) && ok;
	return ok;
}

static void x32_bits9(char *dst, int v) {
	int i;
	dst[0] = '%';
	for (i = 0; i < 9; i++) dst[1 + i] = x32_bit(v, 8 - i) ? '1' : '0';
	dst[10] = 0;
}

static void x32_send_node(const char *text) {
	s_len = Xsprint(s_buf, 0, 's', "node");
	s_len = Xsprint(s_buf, s_len, 's', ",s");
	s_len = Xsprint(s_buf, s_len, 's', (char *)text);
	if (Xverbose) {
		Xfdump("X->", s_buf, s_len, Xdebug);
		fflush(stdout);
	}
	sendto(Xfd, s_buf, s_len, 0, Client_ip_pt, Client_ip_len);
#ifndef __WIN32__
	usleep(200);
#else
	Sleep(1);
#endif
}

static void x32_qstr(char *dst, int cap, const char *s) {
	int w = 0;
	if (!s) s = "";
	if (cap < 3) {
		if (cap) dst[0] = 0;
		return;
	}
	dst[w++] = '"';
	while (*s && w < cap - 2) {
		char c = *s++;
		if (c == '"' || c == '\n' || c == '\r') c = ' ';
		dst[w++] = c;
	}
	dst[w++] = '"';
	dst[w] = 0;
}

void x32_log_osc(void) {
	int i = 0, t, n, a;
	char tags[16];
	const char *extra = "";
	if (r_len <= 0) return;
	while (i < r_len && r_buf[i]) i++;
	t = (i + 4) & ~3;
	tags[0] = 0;
	if (t < r_len && r_buf[t] == ',') {
		n = 0;
		while (n < 14 && (t + n) < r_len && r_buf[t + n]) {
			tags[n] = r_buf[t + n];
			n++;
		}
		tags[n] = 0;
		a = (t + n + 1 + 3) & ~3;
		if (a < r_len && (strcmp(r_buf, "/save") == 0 || strcmp(r_buf, "/load") == 0 ||
			strcmp(r_buf, "/delete") == 0 || strcmp(r_buf, "/copy") == 0 ||
			strcmp(r_buf, "/add") == 0 || strcmp(r_buf, "/node") == 0 ||
			strcmp(r_buf, "/") == 0))
			extra = r_buf + a;
	}
	if (extra[0]) printf("OSC %s %s %s\n", r_buf, tags, extra);
	else if (tags[0]) printf("OSC %s %s\n", r_buf, tags);
	else printf("OSC %s\n", r_buf);
	fflush(stdout);
}

/* ---- console project folder (-d <folder>) ---------------------------- */

void x32_proj_set(const char *path) {
	int n;
	if (!path || !path[0]) return;
	n = (int)strlen(path);
	while (n > 1 && (path[n - 1] == '/' || path[n - 1] == '\\')) n--;
	if (n >= X32_PATHMAX) n = X32_PATHMAX - 1;
	memcpy(x32_proj_dir, path, n);
	x32_proj_dir[n] = 0;
	x32_proj_on = 1;
	snprintf(x32_shw_tag, sizeof x32_shw_tag, "#4.0#");
	snprintf(x32_shw_edit, sizeof x32_shw_edit, "%s", XVERSION);
}

static int x32_casecmp(const char *a, const char *b) {
	if (!a) a = "";
	if (!b) b = "";
	while (*a && *b) {
		unsigned char ca = (unsigned char)*a;
		unsigned char cb = (unsigned char)*b;
		if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
		if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
		if (ca != cb) return (int)ca - (int)cb;
		a++;
		b++;
	}
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int x32_has_ext(const char *name, const char *ext) {
	int n, m, i;
	if (!name || !ext) return 0;
	n = (int)strlen(name);
	m = (int)strlen(ext);
	if (n < m) return 0;
	for (i = 0; i < m; i++) {
		char a = name[n - m + i];
		char b = ext[i];
		if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
		if (a != b) return 0;
	}
	return 1;
}

static int x32_join(char *dst, int cap, const char *dir, const char *name) {
	int na, nb;
	if (!dst || cap < 2 || !dir || !name) return 0;
	na = (int)strlen(dir);
	nb = (int)strlen(name);
	if (na + 1 + nb >= cap) return 0;
	memcpy(dst, dir, na);
	dst[na] = '/';
	memcpy(dst + na + 1, name, nb);
	dst[na + 1 + nb] = 0;
	return 1;
}

/* Numbered scene/snippet names stay short so a long project path still fits. */
static int x32_numbered(char *dst, int cap, const char *dir, const char *fmt,
		int idx, const char *nm) {
	char tail[96];
	int n;
	if (!nm) nm = "";
	n = snprintf(tail, sizeof tail, fmt, idx, nm);
	if (n < 0 || n >= (int)sizeof tail) return 0;
	return x32_join(dst, cap, dir, tail);
}

static int x32_is_int_tok(const char *t) {
	if (!t || !*t) return 0;
	if (*t == '-' || *t == '+') t++;
	if (*t < '0' || *t > '9') return 0;
	while (*t) {
		if (*t < '0' || *t > '9') return 0;
		t++;
	}
	return 1;
}

static int x32_parse_bits(const char *t) {
	int v = 0;
	if (!t || !*t) return 0;
	if (*t == '%') t++;
	if (*t != '0' && *t != '1') return atoi(t);
	while (*t == '0' || *t == '1') {
		v = (v << 1) | (*t == '1');
		t++;
	}
	return v;
}

static void x32_skip_ws(const char **pp) {
	const char *p = *pp;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
	*pp = p;
}

static int x32_preview(const char *p, char *buf, int cap) {
	int i = 0;
	x32_skip_ws(&p);
	if (!*p || cap < 2) {
		if (cap) buf[0] = 0;
		return 0;
	}
	if (*p == '"') {
		p++;
		while (*p && *p != '"' && i < cap - 1) buf[i++] = *p++;
		buf[i] = 0;
		return 1;
	}
	while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && i < cap - 1)
		buf[i++] = *p++;
	buf[i] = 0;
	return 1;
}

static int x32_next_is_quote(const char *p) {
	x32_skip_ws(&p);
	return *p == '"';
}

static char *x32_tok(char **pp) {
	char *p = *pp;
	char *s;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
	if (!*p) {
		*pp = p;
		return NULL;
	}
	if (*p == '"') {
		p++;
		s = p;
		while (*p && *p != '"') p++;
		if (*p == '"') *p++ = 0;
		*pp = p;
		return s;
	}
	s = p;
	while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
	if (*p) *p++ = 0;
	*pp = p;
	return s;
}

static int x32_slot_kind(const char *tok, const char *kind, int *idx) {
	const char *p;
	int v = 0;
	if (!tok || !kind) return 0;
	p = strstr(tok, kind);
	if (!p) return 0;
	if (p != tok && p[-1] != '/') return 0;
	p += strlen(kind);
	if (*p < '0' || *p > '9') return 0;
	while (*p >= '0' && *p <= '9') {
		v = v * 10 + (*p - '0');
		p++;
	}
	if (*p) return 0;
	*idx = v;
	return 1;
}

static int x32_preview_is_slot(const char *p) {
	char peek[96];
	int idx;
	if (!x32_preview(p, peek, sizeof peek)) return 0;
	return x32_slot_kind(peek, "cue/", &idx) ||
		x32_slot_kind(peek, "scene/", &idx) ||
		x32_slot_kind(peek, "snippet/", &idx);
}

static const char *x32_take_name(char **pp) {
	if (x32_preview_is_slot(*pp)) return "";
	{
		char *t = x32_tok(pp);
		return t ? t : "";
	}
}

static int x32_take_ints(char **pp, int *out, int maxn) {
	int n = 0;
	char peek[64];
	while (n < maxn && x32_preview(*pp, peek, sizeof peek) && x32_is_int_tok(peek)) {
		char *t = x32_tok(pp);
		if (!t) break;
		out[n++] = atoi(t);
	}
	return n;
}

static char *x32_read_all(const char *path) {
	FILE *f;
	long n;
	char *b;
	f = fopen(path, "rb");
	if (!f) return NULL;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	n = ftell(f);
	if (n < 0 || n > 2 * 1024 * 1024) {
		fclose(f);
		return NULL;
	}
	if (fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	b = malloc((size_t)n + 1);
	if (!b) {
		fclose(f);
		return NULL;
	}
	if (n && fread(b, 1, (size_t)n, f) != (size_t)n) {
		free(b);
		fclose(f);
		return NULL;
	}
	b[n] = 0;
	fclose(f);
	return b;
}

static void x32_blank_catalog(void) {
	int n;
	x32_set_str(&Xshow[4], NULL);
	for (n = 0; n < 10; n++) Xshow[5 + n].value.ii = 0;
	for (n = 0; n < 100; n++) {
		X32command *p = x32_scene_at(n);
		if (p) {
			x32_set_str(p + 1, NULL);
			x32_set_str(p + 2, NULL);
			(p + 3)->value.ii = 0;
			(p + 4)->value.ii = 0;
		}
		p = x32_snip_at(n);
		if (p) {
			x32_set_str(p + 1, NULL);
			(p + 2)->value.ii = 0;
			(p + 3)->value.ii = 0;
			(p + 4)->value.ii = 0;
			(p + 5)->value.ii = 0;
			(p + 6)->value.ii = 0;
		}
		x32_scene_path[n][0] = 0;
		x32_snip_path[n][0] = 0;
		x32_scene_listed[n] = 0;
		x32_snip_listed[n] = 0;
	}
	for (n = 0; n < 500; n++) {
		X32command *p = x32_cue_at(n);
		if (!p) break;
		(p + 1)->value.ii = 0;
		x32_set_str(p + 2, NULL);
		(p + 3)->value.ii = 0;
		(p + 4)->value.ii = 0;
		(p + 5)->value.ii = 0;
		(p + 6)->value.ii = 0;
		(p + 7)->value.ii = 0;
		(p + 8)->value.ii = 0;
		(p + 9)->value.ii = 0;
	}
}

static void x32_parse_shw(char *buf) {
	char *p = buf;
	char *t;
	if ((unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB &&
			(unsigned char)buf[2] == 0xBF)
		p += 3;
	while ((t = x32_tok(&p)) != NULL) {
		int idx, nints, i;
		int vals[8];
		int slen = (int)strlen(t);
		if (t[0] == '#' && slen >= 3 && t[slen - 1] == '#') {
			char *name;
			int ints[10];
			if (slen < (int)sizeof x32_shw_tag)
				snprintf(x32_shw_tag, sizeof x32_shw_tag, "%s", t);
			name = x32_tok(&p);
			if (name && x32_casecmp(name, "show") == 0) name = x32_tok(&p);
			if (name) x32_set_str(&Xshow[4], name);
			nints = x32_take_ints(&p, ints, 10);
			for (i = 0; i < nints; i++) Xshow[5 + i].value.ii = ints[i];
			if (x32_next_is_quote(p)) {
				char *ed = x32_tok(&p);
				if (ed) snprintf(x32_shw_edit, sizeof x32_shw_edit, "%s", ed);
			}
			continue;
		}
		if (x32_casecmp(t, "show") == 0) {
			char *name = x32_tok(&p);
			int ints[10];
			if (name) x32_set_str(&Xshow[4], name);
			nints = x32_take_ints(&p, ints, 10);
			for (i = 0; i < nints; i++) Xshow[5 + i].value.ii = ints[i];
			if (x32_next_is_quote(p)) {
				char *ed = x32_tok(&p);
				if (ed) snprintf(x32_shw_edit, sizeof x32_shw_edit, "%s", ed);
			}
			continue;
		}
		if (x32_slot_kind(t, "cue/", &idx) && idx >= 0 && idx < 500) {
			X32command *c = x32_cue_at(idx);
			const char *nm;
			if (!c) continue;
			nints = x32_take_ints(&p, vals, 1);
			if (nints == 1) (c + 1)->value.ii = vals[0];
			nm = x32_take_name(&p);
			x32_set_str(c + 2, nm);
			nints = x32_take_ints(&p, vals, 7);
			for (i = 0; i < nints; i++) (c + 3 + i)->value.ii = vals[i];
			continue;
		}
		if (x32_slot_kind(t, "scene/", &idx) && idx >= 0 && idx <= 99) {
			X32command *c = x32_scene_at(idx);
			const char *nm, *notes;
			char peek[80];
			if (!c) continue;
			nm = x32_take_name(&p);
			notes = x32_take_name(&p);
			x32_set_str(c + 1, nm);
			x32_set_str(c + 2, notes);
			if (x32_preview(p, peek, sizeof peek)) {
				if (peek[0] == '%') {
					x32_tok(&p);
					(c + 3)->value.ii = x32_parse_bits(peek);
				} else if (x32_is_int_tok(peek)) {
					x32_tok(&p);
					(c + 3)->value.ii = atoi(peek);
				}
			}
			nints = x32_take_ints(&p, vals, 1);
			if (nints == 1) (c + 4)->value.ii = vals[0];
			x32_scene_listed[idx] = 1;
			continue;
		}
		if (x32_slot_kind(t, "snippet/", &idx) && idx >= 0 && idx <= 99) {
			X32command *c = x32_snip_at(idx);
			const char *nm;
			if (!c) continue;
			nm = x32_take_name(&p);
			x32_set_str(c + 1, nm);
			nints = x32_take_ints(&p, vals, 5);
			for (i = 0; i < nints && i < 5; i++) (c + 2 + i)->value.ii = vals[i];
			x32_snip_listed[idx] = 1;
			continue;
		}
	}
}

static void x32_find_shw(void) {
	const char *folder;
	char want[X32_PATHMAX];
	char chosen[X32_PATHMAX];
	char chosen_name[X32_PATHMAX];
	char exact[X32_PATHMAX];
	int count = 0;
	folder = strrchr(x32_proj_dir, '/');
	folder = folder ? folder + 1 : x32_proj_dir;
	want[0] = 0;
	{
		int nf = (int)strlen(folder);
		if (nf + 4 < (int)sizeof want) {
			memcpy(want, folder, nf);
			memcpy(want + nf, ".shw", 5);
		}
	}
	chosen[0] = 0;
	chosen_name[0] = 0;
	exact[0] = 0;
#ifdef X32_HAS_DIRENT
	{
		DIR *d = opendir(x32_proj_dir);
		struct dirent *de;
		if (d) {
			while ((de = readdir(d)) != NULL) {
				char path[X32_PATHMAX];
				struct stat st;
				if (de->d_name[0] == '.') continue;
				if (!x32_has_ext(de->d_name, ".shw")) continue;
				if (!x32_join(path, sizeof path, x32_proj_dir, de->d_name)) continue;
				if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
				count++;
				if (x32_casecmp(de->d_name, want) == 0)
					snprintf(exact, sizeof exact, "%s", path);
				if (!chosen_name[0] || x32_casecmp(de->d_name, chosen_name) < 0) {
					snprintf(chosen, sizeof chosen, "%s", path);
					snprintf(chosen_name, sizeof chosen_name, "%s", de->d_name);
				}
			}
			closedir(d);
		}
	}
#endif
	if (exact[0]) snprintf(x32_shw_path, sizeof x32_shw_path, "%s", exact);
	else if (chosen[0]) snprintf(x32_shw_path, sizeof x32_shw_path, "%s", chosen);
	else {
		char base[X32_PATHMAX];
		char file[X32_PATHMAX];
		int nb = (int)strlen(folder);
		if (nb >= (int)sizeof base) nb = (int)sizeof base - 1;
		memcpy(base, folder, nb);
		base[nb] = 0;
		x32_sanitize_file(base);
		if (!base[0]) memcpy(base, "show", 5);
		nb = (int)strlen(base);
		if (nb + 4 < (int)sizeof file) {
			memcpy(file, base, nb);
			memcpy(file + nb, ".shw", 5);
			if (!x32_join(x32_shw_path, sizeof x32_shw_path, x32_proj_dir, file))
				x32_shw_path[0] = 0;
		} else {
			x32_shw_path[0] = 0;
		}
	}
	if (count > 1)
		printf("X32 project: %d show files, using %s\n", count,
			strrchr(x32_shw_path, '/') ? strrchr(x32_shw_path, '/') + 1 : x32_shw_path);
	if (count == 0)
		printf("X32 project: no .shw yet in %s\n", x32_proj_dir);
}

static void x32_stem(const char *base, char *stem, int cap) {
	int n, i;
	n = (int)strlen(base);
	for (i = n - 1; i >= 0; i--) if (base[i] == '.') break;
	if (i < 0) i = n;
	if (i >= cap) i = cap - 1;
	memcpy(stem, base, i);
	stem[i] = 0;
}

static int x32_split_index(const char *stem, char *name, int cap) {
	const char *p = stem;
	const char *q;
	const char *prefs[] = {"scene", "snippet", "scn", "snp", NULL};
	int i, v;
	name[0] = 0;
	if (*p >= '0' && *p <= '9') {
		v = 0;
		q = p;
		while (*q >= '0' && *q <= '9' && v < 100000) {
			v = v * 10 + (*q - '0');
			q++;
		}
		if (*q == 0) return v;
		if (*q == ' ' || *q == '-' || *q == '_' || *q == '.') {
			q++;
			while (*q == ' ') q++;
			snprintf(name, cap, "%s", q);
			return v;
		}
	}
	for (i = 0; prefs[i]; i++) {
		int L = (int)strlen(prefs[i]);
		if (strncmp(p, prefs[i], L) == 0 && p[L] >= '0' && p[L] <= '9') {
			v = 0;
			q = p + L;
			while (*q >= '0' && *q <= '9' && v < 100000) {
				v = v * 10 + (*q - '0');
				q++;
			}
			if (*q == 0 || *q == ' ' || *q == '-' || *q == '_' || *q == '.') {
				if (*q) q++;
				while (*q == ' ') q++;
				snprintf(name, cap, "%s", q);
				return v;
			}
		}
	}
	{
		int n = (int)strlen(stem);
		i = n - 1;
		while (i >= 0 && stem[i] >= '0' && stem[i] <= '9') i--;
		if (i >= 0 && i < n - 1 && (stem[i] == '.' || stem[i] == '-' || stem[i] == '_')) {
			v = atoi(stem + i + 1);
			if (i >= cap) i = cap - 1;
			memcpy(name, stem, i);
			name[i] = 0;
			return v;
		}
	}
	snprintf(name, cap, "%s", stem);
	return -1;
}

static int x32_path_taken(const char *path, int snippet) {
	int i;
	for (i = 0; i < 100; i++) {
		const char *have = snippet ? x32_snip_path[i] : x32_scene_path[i];
		if (have[0] && strcmp(have, path) == 0) return 1;
	}
	return 0;
}

static int x32_assign_body(int snippet, int idx, const char *path) {
	char *dst;
	if (idx < 0 || idx > 99 || !path || !path[0]) return 0;
	if ((int)strlen(path) >= X32_PATHMAX) return 0;
	if (snippet) {
		if (!x32_snip_listed[idx] || x32_snip_path[idx][0]) return 0;
		dst = x32_snip_path[idx];
	} else {
		if (!x32_scene_listed[idx] || x32_scene_path[idx][0]) return 0;
		dst = x32_scene_path[idx];
	}
	if (x32_path_taken(path, snippet)) return 0;
	memcpy(dst, path, strlen(path) + 1);
	return 1;
}

static void x32_scan_dir(const char *dir, const char *ext, int snippet, int pass) {
#ifdef X32_HAS_DIRENT
	DIR *d;
	struct dirent *de;
	d = opendir(dir);
	if (!d) return;
	while ((de = readdir(d)) != NULL) {
		char path[X32_PATHMAX];
		char stem[256], name[256];
		struct stat st;
		int idx;
		if (de->d_name[0] == '.') continue;
		if (!x32_has_ext(de->d_name, ext)) continue;
		if (!x32_join(path, sizeof path, dir, de->d_name)) continue;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
		if (x32_path_taken(path, snippet)) continue;
		x32_stem(de->d_name, stem, sizeof stem);
		idx = x32_split_index(stem, name, sizeof name);
		if (pass == 0) {
			if (idx >= 0 && idx <= 99) x32_assign_body(snippet, idx, path);
		} else if (name[0]) {
			int s;
			for (s = 0; s < 100; s++) {
				X32command *slot;
				const char *slotn;
				if (snippet) {
					if (!x32_snip_listed[s] || x32_snip_path[s][0]) continue;
					slot = x32_snip_at(s);
				} else {
					if (!x32_scene_listed[s] || x32_scene_path[s][0]) continue;
					slot = x32_scene_at(s);
				}
				slotn = (slot && (slot + 1)->value.str) ? (slot + 1)->value.str : "";
				if (slotn[0] && x32_casecmp(slotn, name) == 0) {
					x32_assign_body(snippet, s, path);
					break;
				}
			}
		}
	}
	closedir(d);
#else
	(void)dir;
	(void)ext;
	(void)snippet;
	(void)pass;
#endif
}

static void x32_bind_files(int snippet) {
	const char *subs_scn[] = {"", "scene", "Scene", "scenes", "Scenes", NULL};
	const char *subs_snp[] = {"snippets", "Snippets", "snippet", "Snippet", "", NULL};
	const char **subs = snippet ? subs_snp : subs_scn;
	const char *ext = snippet ? ".snp" : ".scn";
	int pass, si;
	for (pass = 0; pass < 2; pass++) {
		for (si = 0; subs[si]; si++) {
			char dir[X32_PATHMAX];
			if (subs[si][0]) {
				if (!x32_join(dir, sizeof dir, x32_proj_dir, subs[si])) continue;
			} else {
				snprintf(dir, sizeof dir, "%s", x32_proj_dir);
			}
			x32_scan_dir(dir, ext, snippet, pass);
		}
	}
}

static void x32_mark_hasdata(void) {
	int n;
	for (n = 0; n < 100; n++) {
		X32command *p;
		if (!x32_scene_listed[n]) continue;
		p = x32_scene_at(n);
		if (!p) continue;
		if (x32_scene_path[n][0]) (p + 4)->value.ii = 1;
		else (p + 4)->value.ii = 0;
	}
	for (n = 0; n < 100; n++) {
		X32command *p;
		if (!x32_snip_listed[n]) continue;
		p = x32_snip_at(n);
		if (!p) continue;
		if (x32_snip_path[n][0]) (p + 6)->value.ii = 1;
		else (p + 6)->value.ii = 0;
	}
}

static const char *x32_relpath(const char *path) {
	int n;
	if (!path || !path[0]) return "(no file)";
	n = (int)strlen(x32_proj_dir);
	if (n && strncmp(path, x32_proj_dir, n) == 0 && path[n] == '/') return path + n + 1;
	return path;
}

static void x32_log_project(void) {
	int n, ns = 0, np = 0, nc = 0;
	const char *show = Xshow[4].value.str ? Xshow[4].value.str : "";
	printf("X32 project: %s\n", x32_proj_dir);
	printf("show \"%s\" from %s\n", show, x32_relpath(x32_shw_path));
	for (n = 0; n < 500; n++) {
		X32command *p = x32_cue_at(n);
		if (p && x32_cue_used(p)) nc++;
	}
	for (n = 0; n < 100; n++) if (x32_scene_listed[n]) ns++;
	for (n = 0; n < 100; n++) if (x32_snip_listed[n]) np++;
	printf("cues: %d\n", nc);
	for (n = 0; n < 500; n++) {
		X32command *p = x32_cue_at(n);
		if (!p || !x32_cue_used(p)) continue;
		printf("  cue/%03d \"%s\"\n", n, (p + 2)->value.str ? (p + 2)->value.str : "");
	}
	printf("scenes: %d\n", ns);
	for (n = 0; n < 100; n++) {
		X32command *p;
		if (!x32_scene_listed[n]) continue;
		p = x32_scene_at(n);
		printf("  scene/%03d \"%s\" <- %s\n", n,
			(p && (p + 1)->value.str) ? (p + 1)->value.str : "",
			x32_relpath(x32_scene_path[n]));
	}
	printf("snippets: %d\n", np);
	for (n = 0; n < 100; n++) {
		X32command *p;
		if (!x32_snip_listed[n]) continue;
		p = x32_snip_at(n);
		printf("  snippet/%03d \"%s\" <- %s\n", n,
			(p && (p + 1)->value.str) ? (p + 1)->value.str : "",
			x32_relpath(x32_snip_path[n]));
	}
	fflush(stdout);
}

static void x32_load_project(void) {
	char *buf;
	struct stat st;
	/* Drop names restored from .X32res.rc before the show file replaces them. */
	x32_blank_catalog();
	if (stat(x32_proj_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
		printf("X32 project folder not found: %s\n", x32_proj_dir);
		fflush(stdout);
		return;
	}
	x32_find_shw();
	buf = x32_read_all(x32_shw_path);
	if (buf) {
		x32_parse_shw(buf);
		free(buf);
	}
	x32_bind_files(0);
	x32_bind_files(1);
	x32_mark_hasdata();
	x32_log_project();
}

static void x32_save_shw(void) {
	char tmp[X32_PATHMAX];
	FILE *f;
	int n, i;
	char qshow[160], qedit[80], qname[160], qnotes[160], bits[16];
	if (!x32_proj_on || !x32_shw_path[0]) return;
	if ((int)strlen(x32_shw_path) + 5 >= (int)sizeof tmp) return;
	{
		int np = (int)strlen(x32_shw_path);
		memcpy(tmp, x32_shw_path, np);
		memcpy(tmp + np, ".tmp", 5);
	}
	f = fopen(tmp, "w");
	if (!f) {
		printf("X32 project: cannot write %s\n", x32_shw_path);
		return;
	}
	if (!x32_shw_tag[0]) snprintf(x32_shw_tag, sizeof x32_shw_tag, "#4.0#");
	if (!x32_shw_edit[0]) snprintf(x32_shw_edit, sizeof x32_shw_edit, "%s", XVERSION);
	x32_qstr(qshow, sizeof qshow, Xshow[4].value.str);
	x32_qstr(qedit, sizeof qedit, x32_shw_edit);
	fprintf(f, "%s show %s", x32_shw_tag, qshow);
	for (i = 0; i < 10; i++) fprintf(f, " %d", Xshow[5 + i].value.ii);
	fprintf(f, " %s\n", qedit);
	for (n = 0; n < 500; n++) {
		X32command *p = x32_cue_at(n);
		if (!p || !x32_cue_used(p)) continue;
		x32_qstr(qname, sizeof qname, (p + 2)->value.str);
		fprintf(f, "cue/%03d %d %s %d %d %d %d %d %d %d\n",
			n, (p + 1)->value.ii, qname,
			(p + 3)->value.ii, (p + 4)->value.ii, (p + 5)->value.ii,
			(p + 6)->value.ii, (p + 7)->value.ii, (p + 8)->value.ii,
			(p + 9)->value.ii);
	}
	for (n = 0; n < 100; n++) {
		X32command *p = x32_scene_at(n);
		int named;
		if (!p) break;
		named = (p + 1)->value.str && (p + 1)->value.str[0];
		if (!x32_scene_listed[n] && !named && !(p + 4)->value.ii) continue;
		x32_qstr(qname, sizeof qname, (p + 1)->value.str);
		x32_qstr(qnotes, sizeof qnotes, (p + 2)->value.str);
		x32_bits9(bits, (p + 3)->value.ii);
		fprintf(f, "scene/%03d %s %s %s %d\n", n, qname, qnotes, bits, (p + 4)->value.ii);
	}
	for (n = 0; n < 100; n++) {
		X32command *p = x32_snip_at(n);
		int named;
		if (!p) break;
		named = (p + 1)->value.str && (p + 1)->value.str[0];
		if (!x32_snip_listed[n] && !named && !(p + 6)->value.ii) continue;
		x32_qstr(qname, sizeof qname, (p + 1)->value.str);
		fprintf(f, "snippet/%03d %s %d %d %d %d %d\n",
			n, qname, (p + 2)->value.ii, (p + 3)->value.ii, (p + 4)->value.ii,
			(p + 5)->value.ii, (p + 6)->value.ii);
	}
	fclose(f);
	x32_commit(x32_shw_path, tmp);
}

static int x32_param_type(X32command *c) {
	int s, nmax, i;
	if (!c) return I32;
	if (c->format.typ != FX32) return c->format.typ;
	for (s = 0; s < 8; s++) {
		nmax = (s == 0) ? Xfx1_max : ((s < 4) ? Xfx2_max : Xfx5_max);
		if (c >= Xfxset[s] && c < Xfxset[s] + nmax) {
			i = (int)(c - Xfxset[s]);
			return FXc_lookup(Xfxset[s], i);
		}
	}
	return F32;
}

static void x32_apply_one(X32command *c, const char *text) {
	int typ, changed = 0, n = 0;
	char tok[160];
	const char *p = text ? text : "";
	while (*p == ' ' || *p == '\t') p++;
	if (*p == '"') {
		const char *e;
		p++;
		e = strchr(p, '"');
		if (!e) e = p + strlen(p);
		n = (int)(e - p);
		if (n > (int)sizeof tok - 1) n = (int)sizeof tok - 1;
		memcpy(tok, p, n);
		tok[n] = 0;
	} else {
		while (p[n] && p[n] != ' ' && p[n] != '\t' && p[n] != '\n' && p[n] != '\r' &&
				n < (int)sizeof tok - 1) {
			tok[n] = p[n];
			n++;
		}
		tok[n] = 0;
	}
	typ = x32_param_type(c);
	if (typ == S32) {
		const char *cur = c->value.str ? c->value.str : "";
		if (strcmp(cur, tok) != 0) {
			x32_set_str(c, tok);
			changed = 1;
			s_len = Xfprint(s_buf, 0, c->command, 's', c->value.str ? c->value.str : "");
		}
	} else if (typ == F32) {
		float f = 0;
		if (!tok[0]) return;
		if (strcmp(tok, "-oo") == 0 || strcmp(tok, "-inf") == 0) f = 0;
		else sscanf(tok, "%f", &f);
		if (f < c->value.ff - EPSILON || f > c->value.ff + EPSILON) {
			c->value.ff = f;
			changed = 1;
			s_len = Xfprint(s_buf, 0, c->command, 'f', &c->value.ff);
		}
	} else if (typ == E32 && c->node) {
		int j = 0, found = -1;
		if (!tok[0]) return;
		while (c->node[j] && c->node[j][0]) {
			const char *nm = c->node[j];
			while (*nm == ' ') nm++;
			if (x32_casecmp(nm, tok) == 0) {
				found = j;
				break;
			}
			j++;
		}
		if (found >= 0 && c->value.ii != found) {
			c->value.ii = found;
			changed = 1;
			s_len = Xfprint(s_buf, 0, c->command, 'i', &c->value.ii);
		}
	} else {
		int v = 0;
		if (!tok[0]) return;
		if (tok[0] == '%') v = x32_parse_bits(tok);
		else sscanf(tok, "%d", &v);
		if (c->value.ii != v) {
			c->value.ii = v;
			changed = 1;
			s_len = Xfprint(s_buf, 0, c->command, 'i', &c->value.ii);
		}
	}
	if (changed) Xsend(S_REM);
}

static void x32_slash_line(const char *line) {
	char rb[BSIZE];
	char sb[BSIZE];
	int rl = r_len, sl = s_len;
	memcpy(rb, r_buf, BSIZE);
	memcpy(sb, s_buf, BSIZE);
	memset(r_buf, 0, BSIZE);
	r_len = Xsprint(r_buf, 0, 's', "/");
	r_len = Xsprint(r_buf, r_len, 's', ",s");
	r_len = Xsprint(r_buf, r_len, 's', (char *)line);
	function_slash();
	memcpy(r_buf, rb, BSIZE);
	memcpy(s_buf, sb, BSIZE);
	r_len = rl;
	s_len = sl;
}

static void x32_apply_console_line(char *line, int safes, int idx) {
	char path[192];
	char *p = line;
	int n = 0;
	X32command *c;
	while (*p == ' ' || *p == '\t') p++;
	if (*p == 0 || *p == '#' || *p == '\n' || *p == '\r') return;
	if (*p != '/') return;
	while (p[n] && p[n] != ' ' && p[n] != '\t' && p[n] != '\n' && p[n] != '\r' &&
			n < (int)sizeof path - 1) {
		path[n] = p[n];
		n++;
	}
	path[n] = 0;
	if (safes >= 0 && x32_scene_skip(path, safes, idx)) return;
	c = x32_find_param(path);
	if (c && !(c->flags & F_FND)) {
		x32_apply_one(c, p + n);
		return;
	}
	{
		int L = (int)strlen(p);
		while (L > 0 && (p[L - 1] == '\n' || p[L - 1] == '\r')) p[--L] = 0;
		/* Leave room for the OSC address and type tag function_slash expects. */
		if (L <= 0 || L >= BSIZE - 64) return;
		x32_slash_line(p);
	}
}

static int x32_apply_console_file(const char *path, int safes, int idx) {
	FILE *f;
	char line[8192];
	if (!path || !path[0]) return 0;
	f = fopen(path, "r");
	if (!f) return 0;
	while (fgets(line, sizeof line, f)) x32_apply_console_line(line, safes, idx);
	fclose(f);
	return 1;
}

static int x32_write_grouped(FILE *f, X32command *node) {
	char rb[BSIZE], sb[BSIZE], arg[192];
	int rl = r_len, sl = s_len, ok = 0;
	const char *path = node->command ? node->command : "";
	if (path[0] == '/') path++;
	if ((int)strlen(path) >= (int)sizeof arg) return 0;
	memcpy(rb, r_buf, BSIZE);
	memcpy(sb, s_buf, BSIZE);
	snprintf(arg, sizeof arg, "%s", path);
	memset(r_buf, 0, BSIZE);
	r_len = Xsprint(r_buf, 0, 's', "/node");
	r_len = Xsprint(r_buf, r_len, 's', ",s");
	r_len = Xsprint(r_buf, r_len, 's', arg);
	s_len = 0;
	s_buf[0] = 0;
	if (function_node() == S_SND && s_len > 12 && s_len < BSIZE && s_buf[12] == '/') {
		char *text = s_buf + 12;
		int L = (int)strlen(text);
		while (L > 0 && (text[L - 1] == '\n' || text[L - 1] == '\r')) text[--L] = 0;
		/* A line that will not fit back through function_slash is not written,
		 * so the caller falls back to one parameter per line. */
		if (L > 0 && text[0] == '/' && L < BSIZE - 64) {
			fprintf(f, "%s\n", text);
			ok = 1;
		}
	}
	memcpy(r_buf, rb, BSIZE);
	memcpy(s_buf, sb, BSIZE);
	r_len = rl;
	s_len = sl;
	return ok;
}

static void x32_write_one_line(FILE *f, X32command *c) {
	int typ = x32_param_type(c);
	fprintf(f, "%s", c->command);
	if (typ == S32) {
		char q[160];
		x32_qstr(q, sizeof q, c->value.str);
		fprintf(f, " %s\n", q);
		return;
	}
	if (typ == E32 && c->node && c->value.ii >= 0 && c->node[c->value.ii] && c->node[c->value.ii][0]) {
		const char *nm = c->node[c->value.ii];
		while (*nm == ' ') nm++;
		fprintf(f, " %s\n", nm);
		return;
	}
	if (typ == F32) {
		fprintf(f, " %.8g\n", c->value.ff);
		return;
	}
	fprintf(f, " %d\n", c->value.ii);
}

typedef struct {
	FILE *f;
	int snippet;
	int ev, ch, ax, mg;
	int lines;
} X32cons;

static void x32_console_arr(X32command *a, int n, void *v) {
	X32cons *w = (X32cons *)v;
	int i = 0;
	while (i < n) {
		int j, k, kids, any, all;
		if (!(a[i].flags & F_FND) || (i + 1 < n && (a[i + 1].flags & F_FND))) {
			i++;
			continue;
		}
		j = i + 1;
		while (j < n && !(a[j].flags & F_FND)) j++;
		kids = j - (i + 1);
		any = 0;
		all = 1;
		if (w->snippet) {
			for (k = i + 1; k < j; k++) {
				if (x32_snip_keep(a[k].command, w->ev, w->ch, w->ax, w->mg)) any = 1;
				else all = 0;
			}
			if (!any) {
				i = j;
				continue;
			}
		} else if (kids <= 0) {
			i = j;
			continue;
		}
		if (kids > 0 && all &&
				(int)strlen(a[i].command) + kids * 32 < BSIZE - 64 &&
				x32_write_grouped(w->f, &a[i])) {
			w->lines++;
			i = j;
			continue;
		}
		for (k = i + 1; k < j; k++) {
			if (w->snippet && !x32_snip_keep(a[k].command, w->ev, w->ch, w->ax, w->mg))
				continue;
			x32_write_one_line(w->f, &a[k]);
			w->lines++;
		}
		i = j;
	}
}

static int x32_write_console_scene(int idx) {
	X32command *p = x32_scene_at(idx);
	X32cons w;
	char tmp[X32_PATHMAX];
	FILE *f;
	char qn[160], qt[160], bits[16];
	if (!p) return 0;
	if (!x32_scene_path[idx][0]) {
		char nm[64];
		int okp;
		x32_copy_clean(nm, (p + 1)->value.str, 60);
		x32_sanitize_file(nm);
		if (nm[0])
			okp = x32_numbered(x32_scene_path[idx], X32_PATHMAX, x32_proj_dir, "%03d %.60s.scn", idx, nm);
		else
			okp = x32_numbered(x32_scene_path[idx], X32_PATHMAX, x32_proj_dir, "%03d.scn", idx, "");
		if (!okp) return 0;
	}
	if ((int)strlen(x32_scene_path[idx]) + 5 >= (int)sizeof tmp) return 0;
	{
		int np = (int)strlen(x32_scene_path[idx]);
		memcpy(tmp, x32_scene_path[idx], np);
		memcpy(tmp + np, ".tmp", 5);
	}
	f = fopen(tmp, "w");
	if (!f) {
		printf("X32 project: cannot write %s\n", x32_scene_path[idx]);
		return 0;
	}
	if (!x32_shw_tag[0]) snprintf(x32_shw_tag, sizeof x32_shw_tag, "#4.0#");
	x32_qstr(qn, sizeof qn, (p + 1)->value.str);
	x32_qstr(qt, sizeof qt, (p + 2)->value.str);
	x32_bits9(bits, (p + 3)->value.ii);
	fprintf(f, "%s %s %s %s 1\n", x32_shw_tag, qn, qt, bits);
	w.f = f;
	w.snippet = 0;
	w.ev = w.ch = w.ax = w.mg = 0;
	w.lines = 0;
	x32_for_engine(x32_console_arr, &w);
	fclose(f);
	x32_commit(x32_scene_path[idx], tmp);
	return 1;
}

static int x32_write_console_snip(int idx) {
	X32command *p = x32_snip_at(idx);
	X32cons w;
	char tmp[X32_PATHMAX];
	char sub[X32_PATHMAX];
	FILE *f;
	char qn[160];
	if (!p) return 0;
	if (!x32_snip_path[idx][0]) {
		char nm[64];
		int okp;
		if (!x32_join(sub, sizeof sub, x32_proj_dir, "snippets")) return 0;
		x32_mkdir(sub);
		x32_copy_clean(nm, (p + 1)->value.str, 60);
		x32_sanitize_file(nm);
		if (nm[0])
			okp = x32_numbered(x32_snip_path[idx], X32_PATHMAX, sub, "%03d %.60s.snp", idx, nm);
		else
			okp = x32_numbered(x32_snip_path[idx], X32_PATHMAX, sub, "%03d.snp", idx, "");
		if (!okp) return 0;
	}
	if ((int)strlen(x32_snip_path[idx]) + 5 >= (int)sizeof tmp) return 0;
	{
		int np = (int)strlen(x32_snip_path[idx]);
		memcpy(tmp, x32_snip_path[idx], np);
		memcpy(tmp + np, ".tmp", 5);
	}
	f = fopen(tmp, "w");
	if (!f) {
		printf("X32 project: cannot write %s\n", x32_snip_path[idx]);
		return 0;
	}
	if (!x32_shw_tag[0]) snprintf(x32_shw_tag, sizeof x32_shw_tag, "#4.0#");
	x32_qstr(qn, sizeof qn, (p + 1)->value.str);
	fprintf(f, "%s %s %d %d %d %d 1\n", x32_shw_tag, qn,
		(p + 2)->value.ii, (p + 3)->value.ii, (p + 4)->value.ii, (p + 5)->value.ii);
	w.f = f;
	w.snippet = 1;
	w.ev = (p + 2)->value.ii;
	w.ch = (p + 3)->value.ii;
	w.ax = (p + 4)->value.ii;
	w.mg = (p + 5)->value.ii;
	w.lines = 0;
	x32_for_engine(x32_console_arr, &w);
	fclose(f);
	x32_commit(x32_snip_path[idx], tmp);
	return 1;
}

void x32_data_init(void) {
	int n, k;
	if (!x32_proj_on) {
		x32_mkdir(X32DATA);
		x32_mkdir(X32DATA "/scene");
		x32_mkdir(X32DATA "/snippet");
		x32_mkdir(X32DATA "/libch");
		x32_mkdir(X32DATA "/libfx");
		x32_mkdir(X32DATA "/librt");
	}
	if (strcmp(Xscene[2].command, "/-show/showfile/scene/000/name") != 0)
		printf("X32 show layout: scene 000 name is not where save expects it\n");
	if (strcmp(Xsnippet[2].command, "/-show/showfile/snippet/000/name") != 0)
		printf("X32 show layout: snippet 000 name is not where save expects it\n");
	if (strcmp(Xshow[0].command, "/-show/prepos/current") != 0 ||
		strcmp(Xshow[4].command, "/-show/showfile/show/name") != 0)
		printf("X32 show layout: show header is not where save expects it\n");
	if (16 + 500 * 10 > Xshow_max)
		printf("X32 show layout: cue table is shorter than 500 slots\n");
	else if (strcmp(Xshow[16].command, "/-show/showfile/cue/000") != 0)
		printf("X32 show layout: cue 000 is not at the expected index\n");
	if (strcmp(Xlibsc[4].command, "/-libs/ch/001/name") != 0)
		printf("X32 show layout: channel library name is not where save expects it\n");
	if (strcmp(Xlibsf[3].command, "/-libs/fx/001/name") != 0)
		printf("X32 show layout: effect library name is not where save expects it\n");
	if (strcmp(Xlibsr[3].command, "/-libs/r/001/name") != 0)
		printf("X32 show layout: routing library name is not where save expects it\n");
	for (k = 0; k < 3; k++) {
		for (n = 0; n < 100; n++) {
			X32command *p = x32_lib_at(k, n);
			if (p && (p + 1)->value.ii == 0) (p + 1)->value.ii = n + 1;
		}
	}
	if (x32_proj_on) {
		x32_load_project();
		return;
	}
	x32_load_show();
	x32_load_scenes_idx();
	x32_load_snips_idx();
	x32_load_cues_idx();
	x32_load_libs_idx();
	x32_load_recall();
	x32_sync_lists();
	printf("X32 show/scene/snippet data in ./" X32DATA "\n");
	fflush(stdout);
}

void x32_after_show_set(void) {
	if (strcmp(r_buf, "/-show/prepos/current") == 0) {
		int sc = x32_show_control();
		if (sc == 1) x32_scene_idx = Xshow[0].value.ii;
		else if (sc == 2) x32_snippet_idx = Xshow[0].value.ii;
		else x32_cue_idx = Xshow[0].value.ii;
	}
	x32_sync_lists();
}

int x32_stat_index(void) {
	int *slot;
	int sc;
	if (strcmp(r_buf, "/-stat/snippet/index") == 0) slot = &x32_snippet_idx;
	else slot = &x32_scene_idx;
	if (x32_osc_has_arg()) {
		if (!x32_osc_prepare()) return 0;
		*slot = x32_take_int();
		if (*slot < 0) *slot = 0;
		if (*slot > 99) *slot = 99;
		sc = x32_show_control();
		if ((sc == 1 && slot == &x32_scene_idx) || (sc == 2 && slot == &x32_snippet_idx)) {
			if (Xshow[0].value.ii != *slot) {
				Xshow[0].value.ii = *slot;
				x32_publish(&Xshow[0]);
			}
		}
		x32_save_recall();
	}
	s_len = Xfprint(s_buf, 0, r_buf, 'i', slot);
	return S_SND;
}

void x32_action_hook(void) {
	int v;
	if (!x32_osc_has_arg()) return;
	if (!x32_osc_prepare()) return;
	v = x32_take_int();
	if (strcmp(r_buf, "/-action/goscene") == 0) x32_recall_scene(v);
	else if (strcmp(r_buf, "/-action/gosnippet") == 0) x32_recall_snip(v);
	else if (strcmp(r_buf, "/-action/gocue") == 0) x32_go_cue(v);
	else if (strcmp(r_buf, "/-action/initshow") == 0 && v == 1) x32_init_show();
	else if (strcmp(r_buf, "/-action/initlib") == 0 && v == 1) x32_init_libs();
	else if (strcmp(r_buf, "/-action/savestate") == 0 && v == 1) {
		x32_sync_lists();
		x32_shutdown_quit = 0;
		X32Shutdown();
		x32_shutdown_quit = 1;
	}
}

int function_copy(void) {
	int kind, src, dst, mask, nint, smax, dmax, nmax;
	char *typ;
	X32command *ch_src, *ch_dst;
	if (!x32_osc_prepare()) return 0;
	typ = x32_take_str();
	kind = x32_kind_of(typ);
	nint = x32_ntags('i');
	if (nint < 2) return x32_reply_si("/copy", typ, 0);
	src = x32_take_int();
	dst = x32_take_int();
	if (kind == 0 && nint >= 3) {
		mask = x32_take_int();
		if (src < 0 || src > 31 || dst < 0 || dst > 31) return x32_reply_si("/copy", "libchan", 0);
		ch_src = Xchannelset[src];
		ch_dst = Xchannelset[dst];
		smax = (src == 0) ? Xchannel01_max : Xchannel02_max;
		dmax = (dst == 0) ? Xchannel01_max : Xchannel02_max;
		nmax = (smax < dmax) ? smax : dmax;
		if (mask & C_CONFIG) x32_copy_range(ch_dst, ch_src, 0, 8, nmax);
		if (mask & C_HA) x32_copy_range(ch_dst, ch_src, 8, 20, nmax);
		if (mask & C_GATE) x32_copy_range(ch_dst, ch_src, 20, 33, nmax);
		if (mask & C_DYN) x32_copy_range(ch_dst, ch_src, 33, 53, nmax);
		if (mask & C_EQ) x32_copy_range(ch_dst, ch_src, 53, 75, nmax);
		if (mask & C_SEND) x32_copy_range(ch_dst, ch_src, 75, 146, nmax);
		return x32_reply_si("/copy", "libchan", 1);
	}
	if (kind == 10) return x32_reply_si("/copy", "scene", x32_copy_scene(src, dst));
	if (kind == 0 || kind == 1 || kind == 2)
		return x32_reply_si("/copy", typ, x32_copy_lib(kind, src, dst));
	return x32_reply_si("/copy", typ, 0);
}

int function_add(void) {
	char *typ;
	int numb;
	char *name;
	if (!x32_osc_prepare()) return 0;
	typ = x32_take_str();
	if (strcmp(typ, "cue") != 0) return x32_reply_si("/add", typ, 0);
	numb = x32_take_int();
	name = x32_take_str();
	return x32_reply_si("/add", "cue", x32_add_cue(numb, name));
}

int function_load(void) {
	char *typ;
	int kind, idx, dest = 0, mask = 0x3f, nint, ok = 0;
	if (!x32_osc_prepare()) return 0;
	typ = x32_take_str();
	kind = x32_kind_of(typ);
	if (x32_ntags('i') < 1) return x32_reply_si("/load", typ, 0);
	idx = x32_take_int();
	nint = x32_ntags('i');
	if (kind == 10) ok = x32_recall_scene(idx);
	else if (kind == 11) ok = x32_recall_snip(idx);
	else if (kind == 0 || kind == 1 || kind == 2) {
		if (nint >= 2) dest = x32_take_int();
		if (nint >= 3) mask = x32_take_int();
		ok = x32_recall_lib(kind, idx, dest, nint >= 2, mask);
	}
	return x32_reply_si("/load", typ, ok);
}

int function_save(void) {
	char *typ, *name, *notes;
	int kind, idx, extra, nint, ok = 0;
	if (!x32_osc_prepare()) return 0;
	typ = x32_take_str();
	kind = x32_kind_of(typ);
	if (x32_ntags('i') < 1 || x32_ntags('s') < 2) return x32_reply_si("/save", typ, 0);
	idx = x32_take_int();
	name = x32_take_str();
	nint = x32_ntags('i');
	if (kind == 10) {
		notes = (x32_ntags('s') >= 3) ? x32_take_str() : "";
		extra = 0;
		if (nint >= 2) extra = x32_take_int();
		ok = x32_save_scene(idx, name, notes, extra, nint >= 2);
	} else if (kind == 11) {
		ok = x32_save_snip(idx, name);
	} else if (kind == 0 || kind == 1) {
		extra = 0;
		if (nint >= 2) extra = x32_take_int();
		ok = x32_save_lib(kind, idx, name, extra);
	} else if (kind == 2) {
		ok = x32_save_lib(2, idx, name, 0);
	}
	return x32_reply_si("/save", typ, ok);
}

int function_delete(void) {
	char *typ;
	int kind, idx, ok = 0;
	if (!x32_osc_prepare()) return 0;
	typ = x32_take_str();
	kind = x32_kind_of(typ);
	if (x32_ntags('i') < 1) return x32_reply_si("/delete", typ, 0);
	idx = x32_take_int();
	if (kind == 10 && x32_scene_at(idx)) {
		x32_clear_scene(idx);
		x32_save_scenes_idx();
		ok = 1;
	} else if (kind == 11 && x32_snip_at(idx)) {
		x32_clear_snip(idx);
		x32_save_snips_idx();
		ok = 1;
	} else if ((kind == 0 || kind == 1 || kind == 2) && x32_lib_at(kind, idx)) {
		x32_clear_lib(kind, idx);
		x32_save_libs_idx();
		ok = 1;
	}
	return x32_reply_si("/delete", typ, ok);
}

int function_libs(void) {
	X32command *a;
	int n, i, setting, r;
	setting = x32_osc_has_arg();
	if (strncmp(r_buf, "/-libs/fx", 9) == 0) {
		a = Xlibsf;
		n = Xlibsf_max;
	} else if (strncmp(r_buf, "/-libs/r", 8) == 0) {
		a = Xlibsr;
		n = Xlibsr_max;
	} else {
		a = Xlibsc;
		n = Xlibsc_max;
	}
	for (i = 0; i < n; i++) {
		if (strcmp(r_buf, a[i].command) == 0) {
			r = funct_params(a, i);
			if (setting && r) x32_save_libs_idx();
			return r;
		}
	}
	return 0;
}

int function_showdump(void) {
	char line[480];
	char qn[80], qt[160], bits[16];
	int n;
	const char *shown = Xshow[4].value.str ? Xshow[4].value.str : "";
	x32_qstr(qn, 80, shown);
	sprintf(line, "/-show/showfile/show %s %d %d %d %d %d %d %d %d %d %d \"%s\"\n",
		qn,
		Xshow[5].value.ii, Xshow[6].value.ii, Xshow[7].value.ii, Xshow[8].value.ii,
		Xshow[9].value.ii, Xshow[10].value.ii, Xshow[11].value.ii, Xshow[12].value.ii,
		Xshow[13].value.ii, Xshow[14].value.ii, XVERSION);
	x32_send_node(line);
	for (n = 0; n < 500; n++) {
		X32command *p = x32_cue_at(n);
		if (!p || !x32_cue_used(p)) continue;
		x32_qstr(qn, 80, (p + 2)->value.str);
		sprintf(line, "/-show/showfile/cue/%03d %d %s %d %d %d %d %d %d %d\n",
			n, (p + 1)->value.ii, qn, (p + 3)->value.ii, (p + 4)->value.ii,
			(p + 5)->value.ii, (p + 6)->value.ii, (p + 7)->value.ii,
			(p + 8)->value.ii, (p + 9)->value.ii);
		x32_send_node(line);
	}
	for (n = 0; n < 100; n++) {
		X32command *p = x32_scene_at(n);
		if (!p) break;
		if (!(p + 4)->value.ii && !((p + 1)->value.str && (p + 1)->value.str[0])) continue;
		x32_qstr(qn, 80, (p + 1)->value.str);
		x32_qstr(qt, 160, (p + 2)->value.str);
		x32_bits9(bits, (p + 3)->value.ii);
		sprintf(line, "/-show/showfile/scene/%03d %s %s %s %d\n", n, qn, qt, bits, (p + 4)->value.ii);
		x32_send_node(line);
	}
	for (n = 0; n < 100; n++) {
		X32command *p = x32_snip_at(n);
		if (!p) break;
		if (!(p + 6)->value.ii && !((p + 1)->value.str && (p + 1)->value.str[0])) continue;
		x32_qstr(qn, 80, (p + 1)->value.str);
		sprintf(line, "/-show/showfile/snippet/%03d %s %d %d %d %d %d\n",
			n, qn, (p + 2)->value.ii, (p + 3)->value.ii, (p + 4)->value.ii,
			(p + 5)->value.ii, (p + 6)->value.ii);
		x32_send_node(line);
	}
	s_len = 0;
	return 0;
}
