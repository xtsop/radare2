/* radare - LGPL - Copyright 2009-2013 - pancake */

static int preludecnt = 0;
static int searchflags = 0;
static int searchshow = 0;
static int searchhits = 0;
static const char *cmdhit = NULL;
static const char *searchprefix = NULL;
static unsigned int searchcount = 0;

static int __prelude_cb_hit(RSearchKeyword *kw, void *user, ut64 addr) {
	RCore *core = (RCore *)user;
	int depth = r_config_get_i (core->config, "anal.depth");
	//eprintf ("ap: Found function prelude %d at 0x%08"PFMT64x"\n", preludecnt, addr);
	searchhits ++; //= kw->count+1;
	r_core_anal_fcn (core, addr, -1, R_ANAL_REF_TYPE_NULL, depth);
	preludecnt++;
	return R_TRUE;
}

R_API int r_core_search_prelude(RCore *core, ut64 from, ut64 to, const ut8 *buf, int blen, const ut8 *mask, int mlen) {
	int ret;
	ut64 at;
	ut8 *b = (ut8 *)malloc (core->blocksize);
// TODO: handle sections ?
	r_search_reset (core->search, R_SEARCH_KEYWORD);
	r_search_kw_add (core->search,
		r_search_keyword_new (buf, blen, mask, mlen, NULL));
	r_search_begin (core->search);
	r_search_set_callback (core->search, &__prelude_cb_hit, core);
	preludecnt = 0;
	for (at = from; at < to; at += core->blocksize) {
		if (r_cons_singleton ()->breaked)
			break;
		ret = r_io_read_at (core->io, at, b, core->blocksize);
		if (ret != core->blocksize)
			break;
		if (r_search_update (core->search, &at, b, ret) == -1) {
			eprintf ("search: update read error at 0x%08"PFMT64x"\n", at);
			break;
		}
	}
	eprintf ("Analized %d functions based on preludes\n", preludecnt);
	free (b);
	return preludecnt;
}

R_API int r_core_search_preludes(RCore *core) {
	int ret = -1;
	const char *prelude = r_config_get (core->config, "anal.prelude");
	const char *arch = r_config_get (core->config, "asm.arch");
	int bits = r_config_get_i (core->config, "asm.bits");
	ut64 from = core->offset;
	ut64 to = core->offset+0xffffff; // hacky!
	// TODO: this is x86 only
	if (prelude && *prelude) {
		ut8 *kw = malloc (strlen (prelude));
		int kwlen = r_hex_str2bin (prelude, kw);
		ret = r_core_search_prelude (core, from, to, kw, kwlen, NULL, 0);
		free (kw);
	} else
	if (strstr (arch, "mips")) {
		ret = r_core_search_prelude (core, from, to,
			(const ut8 *)"\x27\xbd\x00", 3, NULL, 0);
	} else
	if (strstr (arch, "x86")) {
		switch (bits) {
		case 32:
			ret = r_core_search_prelude (core, from, to,
				(const ut8 *)"\x55\x89\xe5", 3, NULL, 0);
			break;
		case 64:
			ret = r_core_search_prelude (core, from, to,
				(const ut8 *)"\x55\x48\x89\xe5", 3, NULL, 0);
			//r_core_cmd0 (core, "./x 554989e5");
			break;
		default:
			eprintf ("ap: Unsupported bits: %d\n", bits);
		}
	} else eprintf ("ap: Unsupported asm.arch and asm.bits\n");
	return ret;
}

static int __cb_hit(RSearchKeyword *kw, void *user, ut64 addr) {
	RCore *core = (RCore *)user;
	searchhits ++ ;///= kw->count+1;
	if (searchcount) {
		if (!--searchcount) {
			//eprintf ("\nsearch stop: search.count reached\n");
			return R_FALSE;
		}
	}
	if (searchshow) {
		int len, i;
		ut8 buf[64];
		char str[128], *p;
		switch (kw->type) {
		case R_SEARCH_KEYWORD_TYPE_STRING:
			len = sizeof (str);
			r_core_read_at (core, addr, (ut8*)str+1, len-2);
			*str = '"';
			r_str_filter_zeroline (str, len);
			strcpy (str+strlen (str), "\"");
			break;
		default:
			len = kw->keyword_length + 8; // 8 byte context
			if (len>=sizeof (str)) len = sizeof (str)-1;
			r_core_read_at (core, addr, buf, sizeof (buf));
			for (i=0, p=str; i<len; i++) {
				sprintf (p, "%02x", buf[i]);
				p += 2;
			}
			*p = 0;
			break;
		}
		r_cons_printf ("0x%08"PFMT64x" %s%d_%d %s\n",
			addr, searchprefix, kw->kwidx, kw->count, str);
	} else {
		if (searchflags)
			r_cons_printf ("%s%d_%d\n", searchprefix, kw->kwidx, kw->count);
		else r_cons_printf ("f %s%d_%d %d 0x%08"PFMT64x"\n", searchprefix,
				kw->kwidx, kw->count, kw->keyword_length, addr);
	}
	if (searchflags) {
		char flag[64];
		snprintf (flag, sizeof (flag), "%s%d_%d", searchprefix, kw->kwidx, kw->count);
		r_flag_set (core->flags, flag, addr, kw->keyword_length, 1);
	}
	if (!strnull (cmdhit)) {
		ut64 here = core->offset;
		r_core_seek (core, addr, R_FALSE);
		r_core_cmd (core, cmdhit, 0);
		r_core_seek (core, here, R_TRUE);
	}
	return R_TRUE;
}


static int c = 0;
static inline void print_search_progress(ut64 at, ut64 to, int n) {
	if ((++c%64))
		return;
	if (r_cons_singleton()->columns<50)
		eprintf ("\r[  ]  0x%08"PFMT64x"  hits = %d   \r%s",
				at, n, (c%2)?"[ #]":"[# ]");
	else eprintf ("\r[  ]  0x%08"PFMT64x" < 0x%08"PFMT64x"  hits = %d   \r%s",
			at, to, n, (c%2)?"[ #]":"[# ]");
}

R_API void r_core_get_boundaries (RCore *core, const char *mode, ut64 *from, ut64 *to) {
	if (!strcmp (mode, "block")) {
		*from = core->offset;
		*to = core->offset + core->blocksize;
	} else
	if (!strcmp (mode, "file")) {
		if (core->io->va) {
			RListIter *iter;
			RIOSection *s;
			*from = *to = core->offset;
			r_list_foreach (core->io->sections, iter, s) {
				if (((s->vaddr) < *from) && s->vaddr)
					*from = s->vaddr;
				if ((s->vaddr+s->size) > *to && *from>=s->vaddr)
					*to = s->vaddr+s->size;
			}
			if (*to == 0LL || *to == UT64_MAX || *to == UT32_MAX)
				*to = r_io_size (core->io);
		} else {
			RIOMap *map = r_io_map_get (core->io, core->offset);
			*from = core->offset;
			*to = r_io_size (core->io) + (map? map->to:0);
		}
	} else
	if (!strcmp (mode, "section")) {
		if (core->io->va) {
			RListIter *iter;
			RIOSection *s;
			*from = *to = core->offset;
			r_list_foreach (core->io->sections, iter, s) {
				if (*from >= s->vaddr && *from < (s->vaddr+s->size)) {
					*to = s->vaddr+s->size;
					break;
				}
			}
		} else {
			*from = core->offset;
			*to = r_io_size (core->io);
		}
	} else {
		//if (!strcmp (mode, "raw")) {
		/* obey temporary seek if defined '/x 8080 @ addr:len' */
		if (core->tmpseek) {
			*from = core->offset;
			*to = core->offset + core->blocksize;
		} else {
			// TODO: repeat last search doesnt works for /a
			*from = r_config_get_i (core->config, "search.from");
			if (*from == UT64_MAX)
				*from = core->offset;
			*to = r_config_get_i (core->config, "search.to");
			if (*to == UT64_MAX) {
				if (core->io->va) {
					/* TODO: section size? */
				} else {
					*to = core->file->size;
				}
			}
		}
	}
}

// TODO: handle more than one?
static ut64 findprevopsz(RCore *core, ut64 addr) {
	int i;
	RAnalOp aop;
	ut8 buf[132];
	ut64 base = addr - 120;
	ut64 newaddr = UT64_MAX;
	r_io_read_at (core->io, base, buf, sizeof (buf));
	for (i=0; i<16; i++) {
		if (r_anal_op (core->anal, &aop, addr-i, buf+120-i, 16+i)) {
			if (aop.length<1) break;
			if (i == aop.length)
				return addr-i;
		}
	}
	return newaddr;
}

static int r_core_search_rop (RCore *core, ut64 from, ut64 to, int opt) {
	ut8 *buf;
	ut64 prev;
	RAsmOp asmop;
	RAnalOp aop;
	int i, delta = to-from;
	if (delta<1) {
		return R_FALSE;
	}
	buf = malloc (delta);
	r_io_read_at (core->io, from, buf, delta);
	for (i=0; i<delta; i++) {
		if (r_anal_op (core->anal, &aop, from+i, buf+i, delta-i)) {
			int ret = r_asm_disassemble (core->assembler, &asmop, buf+i, delta-i);
			switch (aop.type) {
			case R_ANAL_OP_TYPE_TRAP:
			case R_ANAL_OP_TYPE_RET:
			case R_ANAL_OP_TYPE_UCALL:
			case R_ANAL_OP_TYPE_UJMP:
				r_cons_printf ("0x%08"PFMT64x"  %s\n", from+i, asmop.buf_asm);
				prev = findprevopsz (core, from+i);
				if (prev != UT64_MAX) {
					ut64 prev2 = findprevopsz (core, prev);
					if (prev2 != UT64_MAX)
						r_core_cmdf (core, "pd 3 @ 0x%"PFMT64x, prev2);
					else r_core_cmdf (core, "pd 2 @ 0x%"PFMT64x, prev);
				} else r_core_cmdf (core, "pd 1 @ 0x%"PFMT64x, from+i);
				// show this and prev opcode
				break;
			}
			//eprintf ("%lld\n", aop.type);
		}
	}
	return R_TRUE;
}

static int cmd_search(void *data, const char *input) {
	int i, len, ret, dosearch = R_FALSE;
	RCore *core = (RCore *)data;
	int aes_search = R_FALSE;
	int ignorecase = R_FALSE;
	int inverse = R_FALSE;
	ut64 at, from, to;
	const char *mode;
	char *inp;
	ut64 n64, __from, __to;
	ut32 n32;
	ut16 n16;
	ut8 *buf;

c = 0;
	__from = r_config_get_i (core->config, "search.from");
	__to = r_config_get_i (core->config, "search.to");

	searchshow = r_config_get_i (core->config, "search.show");
	mode = r_config_get (core->config, "search.in");
	r_core_get_boundaries (core, mode, &from, &to);

	if (__from != UT64_MAX) from = __from;
	if (__to != UT64_MAX) to = __to;
	if (__to < __from) {
		eprintf ("Invalid search range. Check 'e search.{from|to}'\n");
		return R_FALSE;
	}

	core->search->align = r_config_get_i (core->config, "search.align");
	searchflags = r_config_get_i (core->config, "search.flags");
	//TODO: handle section ranges if from&&to==0
/*
	section = r_io_section_get (core->io, core->offset);
	if (section) {
		from += section->vaddr;
		//fin = ini + s->size;
	}
*/
	searchprefix = r_config_get (core->config, "search.prefix");
	// TODO: get ranges from current IO section
	/* XXX: Think how to get the section ranges here */
	if (from == 0LL) from = core->offset;
	if (to == 0LL) to = UT32_MAX; // XXX?

	reread:
	switch (*input) {
	case '!':
		input++;
		inverse = R_TRUE;
		goto reread;
		break;
	case 'R':
		{
		eprintf ("Search for ROP gadgets...\n");
		r_core_search_rop (core, from, to, 0);
		}
		return R_TRUE;
	case 'r':
		if (input[1]==' ')
			r_core_anal_search (core, from, to, r_num_math (core->num, input+2));
		else r_core_anal_search (core, from, to, core->offset);
		break;
	case 'a': {
		char *kwd;
		if (!(kwd = r_core_asm_search (core, input+2, from, to)))
			return R_FALSE;
		r_search_reset (core->search, R_SEARCH_KEYWORD);
		r_search_set_distance (core->search, (int)
				r_config_get_i (core->config, "search.distance"));
		r_search_kw_add (core->search,
				r_search_keyword_new_hexmask (kwd, NULL));
		r_search_begin (core->search);
		free (kwd);
		dosearch = R_TRUE;
		} break;
	case 'A':
		dosearch = aes_search = R_TRUE;
		break;
	case '/':
		r_search_begin (core->search);
		dosearch = R_TRUE;
		break;
	case 'm':
		dosearch = R_FALSE;
		if (input[1]==' ' || input[1]=='\0') {
			const char *file = input[1]? input+2: NULL;
			ut64 addr = from;
			r_cons_break (NULL, NULL);
			for (; addr<to; addr++) {
				if (r_cons_singleton ()->breaked)
					break;
				if (r_core_magic_at (core, file, addr, 99, R_FALSE) == -1) {
					// something went terribly wrong.
					break;
				}
			}
			r_cons_break_end ();
		} else eprintf ("Usage: /m [file]\n");
		break;
	case 'p':
		{
			int ps = atoi (input+1);
			if (ps>1) {
				r_search_pattern_size (core->search, ps);
				r_search_pattern (core->search, from, to);
			} else eprintf ("Invalid pattern size (must be >0)\n");
		}
		break;
	case 'v':
		r_search_reset (core->search, R_SEARCH_KEYWORD);
		r_search_set_distance (core->search, (int)
			r_config_get_i (core->config, "search.distance"));
		switch (input[1]) {
		case '?':
			eprintf ("Usage: /v[2|4|8] [value]\n");
			return R_TRUE;
		case '8':
			n64 = r_num_math (core->num, input+2);
			r_search_kw_add (core->search,
				r_search_keyword_new ((const ut8*)&n64, 8, NULL, 0, NULL));
			break;
		case '2':
			n16 = (ut16)r_num_math (core->num, input+2);
			r_search_kw_add (core->search,
				r_search_keyword_new ((const ut8*)&n16, 2, NULL, 0, NULL));
			break;
		default: // default size
		case '4':
			n32 = (ut32)r_num_math (core->num, input+1);
			r_search_kw_add (core->search,
				r_search_keyword_new ((const ut8*)&n32, 4, NULL, 0, NULL));
			break;
		}
// TODO: Add support for /v4 /v8 /v2
		r_search_begin (core->search);
		dosearch = R_TRUE;
		break;
	case 'w': /* search wide string */
		if (input[1]==' ') {
			int len = strlen (input+2);
			const char *p2;
			char *p, *str = malloc ((len+1)*2);
			for (p2=input+2, p=str; *p2; p+=2, p2++) {
				p[0] = *p2;
				p[1] = 0;
			}
			r_search_reset (core->search, R_SEARCH_KEYWORD);
			r_search_set_distance (core->search, (int)
				r_config_get_i (core->config, "search.distance"));
			r_search_kw_add (core->search,
				r_search_keyword_new ((const ut8*)str, len*2, NULL, 0, NULL));
			r_search_begin (core->search);
			dosearch = R_TRUE;
		}
		break;
	case 'i':
		if (input[1]!= ' ') {
			eprintf ("Missing ' ' after /i\n");
			return R_FALSE;
		}
		ignorecase = R_TRUE;
	case ' ': /* search string */
		inp = strdup (input+1+ignorecase);
		if (ignorecase)
			for (i=1; inp[i]; i++)
				inp[i] = tolower (inp[i]);
		len = r_str_escape (inp);
		eprintf ("Searching %d bytes from 0x%08"PFMT64x" to 0x%08"PFMT64x": ", len, from, to);
		for (i=0; i<len; i++) eprintf ("%02x ", (ut8)inp[i]);
		eprintf ("\n");
		r_search_reset (core->search, R_SEARCH_KEYWORD);
		r_search_set_distance (core->search, (int)
			r_config_get_i (core->config, "search.distance"));
		{
		RSearchKeyword *skw;
		skw = r_search_keyword_new ((const ut8*)inp, len, NULL, 0, NULL);
		if (skw) {
			skw->icase = ignorecase;
			skw->type = R_SEARCH_KEYWORD_TYPE_STRING;
			r_search_kw_add (core->search, skw);
		} else {
			eprintf ("Invalid keyword\n");
		}
		}
		r_search_begin (core->search);
		dosearch = R_TRUE;
		break;
	case 'e': /* match regexp */
		{
		char *inp = strdup (input+2);
		char *res = (char *)r_str_lchr (inp+1, inp[0]);
		char *opt = NULL;
		if (res > inp) {
			opt = strdup (res+1);
			res[1]='\0';
		}
		r_search_reset (core->search, R_SEARCH_REGEXP);
		r_search_set_distance (core->search, (int)
			r_config_get_i (core->config, "search.distance"));
		r_search_kw_add (core->search,
			r_search_keyword_new_str (inp, opt, NULL, 0));
		r_search_begin (core->search);
		dosearch = R_TRUE;
		free (inp);
		free (opt);
		}
		break;
	case 'd': /* search delta key */
		r_search_reset (core->search, R_SEARCH_DELTAKEY);
		r_search_kw_add (core->search,
			r_search_keyword_new_hexmask (input+2, NULL));
		r_search_begin (core->search);
		dosearch = R_TRUE;
		break;
	case 'x': /* search hex */
		r_search_reset (core->search, R_SEARCH_KEYWORD);
		r_search_set_distance (core->search, (int)
			r_config_get_i (core->config, "search.distance"));
		// TODO: add support for binmask here
		{
			char *s, *p = strdup (input+2);
			s = strchr (p, ' ');
			if (!s) s = strchr (p, ':');
			if (s) {
				*s++ = 0;
				r_search_kw_add (core->search,
						r_search_keyword_new_hex (p, s, NULL));
			} else {
				r_search_kw_add (core->search,
						r_search_keyword_new_hexmask (input+2, NULL));
			}
		}
		r_search_begin (core->search);
		dosearch = R_TRUE;
		break;
	case 'c': /* search asm */
		{
		RCoreAsmHit *hit;
		RListIter *iter;
		int count = 0;
		RList *hits;
		if ((hits = r_core_asm_strsearch (core, input+2, from, to))) {
			r_list_foreach (hits, iter, hit) {
				r_cons_printf ("f %s_%i @ 0x%08"PFMT64x"   # %i: %s\n",
					searchprefix, count, hit->addr, hit->len, hit->code);
				count++;
			}
			r_list_destroy (hits);
		}
		dosearch = 0;
		}
		break;
	case 'z': /* search asm */
		{
		char *p;
		ut32 min, max;
		if (!input[1]) {
			eprintf ("Usage: /z min max\n");
			break;
		}
		if ((p = strchr (input+2, ' '))) {
			*p = 0;
			max = r_num_math (core->num, p+1);
		} else {
			eprintf ("Usage: /z min max\n");
			break;
		}
		min = r_num_math (core->num, input+2);
		if (!r_search_set_string_limits (core->search, min, max)) {
			eprintf ("Error: min must be lower than max\n");
			break;
		}
		r_search_reset (core->search, R_SEARCH_STRING);
		r_search_set_distance (core->search, (int)
				r_config_get_i (core->config, "search.distance"));
		r_search_kw_add (core->search,
			r_search_keyword_new_hexmask ("00", NULL)); //XXX
		r_search_begin (core->search);
		dosearch = R_TRUE;
		}
		break;
	default:
		r_cons_printf (
		"Usage: /[amx/] [arg]\n"
		" / foo\\x00       ; search for string 'foo\\0'\n"
		" /w foo          ; search for wide string 'f\\0o\\0o\\0'\n"
		" /! ff           ; search for first occurrence not matching\n"
		" /i foo          ; search for string 'foo' ignoring case\n"
		" /e /E.F/i       ; match regular expression\n"
		" /x ff0033       ; search for hex string\n"
		" /x ff..33       ; search for hex string ignoring some nibbles\n"
		" /x ff43 ffd0    ; search for hexpair with mask\n"
		" /d 101112       ; search for a deltified sequence of bytes\n"
		" /!x 00          ; inverse hexa search (find first byte != 0x00)\n"
		" /c jmp [esp]    ; search for asm code (see search.asmstr)\n"
		" /a jmp eax      ; assemble opcode and search its bytes\n"
		" /A              ; search for AES expanded keys\n"
		" /r sym.printf   ; analyze opcode reference an offset\n"
		" /R              ; search for ROP gadgets\n"
		" /m magicfile    ; search for matching magic file (use blocksize)\n"
		" /p patternsize  ; search for pattern of given size\n"
		" /z min max      ; search for strings of given size\n"
		" /v[?248] num    ; look for a asm.bigendian 32bit value\n"
		" //              ; repeat last search\n"
		" ./ hello        ; search 'hello string' and import flags\n"
		"Configuration:\n"
		" e cmd.hit = x         ; command to execute on every search hit\n"
		" e search.distance = 0 ; search string distance\n"
		" e search.in = [foo]   ; boundaries to raw, block, file, section)\n"
		" e search.align = 4    ; only catch aligned search hits\n"
		" e search.from = 0     ; start address\n"
		" e search.to = 0       ; end address\n"
		" e search.asmstr = 0   ; search string instead of assembly\n"
		" e search.flags = true ; if enabled store flags on keyword hits\n");
		break;
	}
	searchhits = 0;
	r_config_set_i (core->config, "search.kwidx", core->search->n_kws);
	if (dosearch) {
		if (!searchflags)
			r_cons_printf ("fs hits\n");
		core->search->inverse = inverse;
		searchcount = r_config_get_i (core->config, "search.count");
		if (searchcount)
			searchcount++;
		if (core->search->n_kws>0 || aes_search) {
			RSearchKeyword aeskw;
			if (aes_search) {
				memset (&aeskw, 0, sizeof (aeskw));
				aeskw.keyword_length = 31;
			}
			/* set callback */
			/* TODO: handle last block of data */
			/* TODO: handle ^C */
			/* TODO: launch search in background support */
			// REMOVE OLD FLAGS r_core_cmdf (core, "f-%s*", r_config_get (core->config, "search.prefix"));
			buf = (ut8 *)malloc (core->blocksize);
			r_search_set_callback (core->search, &__cb_hit, core);
			cmdhit = r_config_get (core->config, "cmd.hit");
			r_cons_break (NULL, NULL);
			// XXX required? imho nor_io_set_fd (core->io, core->file->fd);
			for (at = from; at < to; at += core->blocksize) {
				print_search_progress (at, to, searchhits);
				if (r_cons_singleton ()->breaked) {
					eprintf ("\n\n");
					break;
				}
				//ret = r_core_read_at (core, at, buf, core->blocksize);
			//	ret = r_io_read_at (core->io, at, buf, core->blocksize); 
				r_io_seek (core->io, at, R_IO_SEEK_SET);
				ret = r_io_read (core->io, buf, core->blocksize);
/*
				if (ignorecase) {
					int i;
					for (i=0; i<core->blocksize; i++)
						buf[i] = tolower (buf[i]);
				}
*/
				if (ret <1)
					break;
				if (aes_search) {
					int delta = r_search_aes_update (core->search, at, buf, ret);
					if (delta != -1) {
						if (!r_search_hit_new (core->search, &aeskw, at+delta)) {
							break;
						}
						aeskw.count++;
					}
				} else
				if (r_search_update (core->search, &at, buf, ret) == -1) {
					//eprintf ("search: update read error at 0x%08"PFMT64x"\n", at);
					break;
				}
			}
			print_search_progress (at, to, searchhits);
			r_cons_break_end ();
			free (buf);
			//r_cons_clear_line ();
			if (searchflags && searchcount>0) {
				eprintf ("hits: %d  %s%d_0 .. %s%d_%d\n",
					searchhits,
					searchprefix, core->search->n_kws-1,
					searchprefix, core->search->n_kws-1, searchcount-1);
			} else eprintf ("hits: %d\n", searchhits);
		} else eprintf ("No keywords defined\n");
	}
	return R_TRUE;
}
