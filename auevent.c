/*-
 * xnumon - monitor macOS for malicious activity
 * https://www.roe.ch/xnumon
 *
 * Copyright (c) 2017-2018, Daniel Roethlisberger <daniel@roe.ch>.
 * All rights reserved.
 *
 * Licensed under the Open Software License version 3.0.
 */

#include "auevent.h"

#include "aupipe.h"
#include "minmax.h"
#include "sys.h"
#include "aev.h"
#include "logutl.h"

#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <limits.h>
#include <arpa/inet.h>

#include <bsm/libbsm.h>
#include <bsm/audit_kevents.h>
#include <bsm/audit_domain.h>
#include <bsm/audit_socket_type.h>

/*
 * Record token structs in libbsm:
 * https://github.com/openbsm/openbsm/blob/master/bsm/libbsm.h
 *
 * AUT_* token constants in kernel:
 * https://github.com/apple/darwin-xnu/blob/master/bsd/bsm/audit_record.h
 *
 * AUE_* event constants in kernel:
 * https://github.com/apple/darwin-xnu/blob/master/bsd/bsm/audit_kevents.h
 */

static dev_t devnull;

int
auevent_init(void) {
	devnull = sys_devbypath("/dev/null");
	if (devnull == (dev_t)-1)
		return -1;
	return 0;
}

#define SET_DEV(DST_DEV, SRC_TID) \
	DST_DEV = ((dev_t)(SRC_TID).port) == devnull ? -1 : (SRC_TID).port;

#define SET_ADDR(DST_ADDR, SRC_TID) \
	if ((SRC_TID).addr != 0) { \
		(DST_ADDR).family = AF_INET; \
		(DST_ADDR).ev_addr = (SRC_TID).addr; \
	}

#define SET_ADDR_EX(DST_ADDR, SRC_TID) \
	if ((SRC_TID).type == AU_IPv4) { \
		if ((SRC_TID).addr[0] != 0) { \
			(DST_ADDR).family = AF_INET; \
			(DST_ADDR).ev_addr = (SRC_TID).addr[0]; \
		} \
	} else if ((SRC_TID).type == AU_IPv6) { \
		(DST_ADDR).family = AF_INET6; \
		(DST_ADDR).ev6_addr[0] = (SRC_TID).addr[0]; \
		(DST_ADDR).ev6_addr[1] = (SRC_TID).addr[1]; \
		(DST_ADDR).ev6_addr[2] = (SRC_TID).addr[2]; \
		(DST_ADDR).ev6_addr[3] = (SRC_TID).addr[3]; \
	}

/*
 * While this functionality is still present, it is not currently being used
 * by xnumon, so the linear search is not an issue.
 */
static bool
auevent_type_in_typelist(const uint16_t type, const uint16_t typelist[]) {
	int i = 0;
	if (!typelist)
		return true;
	while (typelist[i]) {
		if (type == typelist[i])
			return true;
		i++;
	}
	return false;
}

/*
 * ev must be created using auevent_create before every call to
 * auevent_fread and destroyed after using the results.
 *
 * returns 0 to indicate that a record was skipped
 * returns 1 to indicate that a record was read into ev
 * returns -1 on errors
 */
ssize_t
auevent_fread(audit_event_t *ev, const uint16_t aues[], int flags, FILE *f) {
	int rv;
	int reclen;
	tokenstr_t tok;
	size_t textc;
	size_t pathc;

	assert(ev);

	/*
	 * https://github.com/openbsm/openbsm/blob/master/libbsm/bsm_io.c
	 *
	 * au_read_rec always reads a whole record.  On read errors or short
	 * reads due to non-blocking I/O, it returns an error and leaves the
	 * file pointer dangling where it was without returning the partially
	 * read buffer.  While using blocking file descriptors on a sane
	 * kernel, this should work for us and read exactly one event from
	 * the file descriptor per call.
	 */
	reclen = au_read_rec(f, &ev->recbuf);
	if (reclen == -1) {
		fprintf(stderr, "au_read_rec(): %s (%i)\n",
		                strerror(errno), errno);
		return -1;
	}
	if (reclen == 0)
		goto skip_rec;

	textc = 0;
	pathc = 0;
	for (int recpos = 0; recpos < reclen;) {
		rv = au_fetch_tok(&tok, ev->recbuf+recpos, reclen-recpos);
		if (rv == -1) {
			/* partial record; libbsm's current implementation
			 * of au_read_rec never reads a partial record.
			 * If it would, there would be a need for handling
			 * partial records gracefully (praudit does not). */
			fprintf(stderr, "au_fetch_tok() returns error,"
			                " skipping partial record\n");
			goto skip_rec;
		}

		/*
		 * XNU reports subjects and processes not attached to any TTY
		 * with tty device /dev/null and tty addr 0.0.0.0.
		 * Translate those here to no device represented by (dev_t)-1
		 * and no addr represented by address family 0, respectively.
		 *
		 * The timestamp in the headers is nanotime() shortly before
		 * the syscall returns to the calling userspace process.
		 */

		switch (tok.id) {
		/* record header and trailer */
		case AUT_HEADER32:
			ev->type = tok.tt.hdr32.e_type;
			if (aues && !auevent_type_in_typelist(ev->type, aues))
				goto skip_rec;
			ev->mod = tok.tt.hdr32.e_mod;
			ev->tv.tv_sec = (time_t)tok.tt.hdr32.s;
			ev->tv.tv_nsec = (long)tok.tt.hdr32.ms*1000000;
			/* size, version */
			break;
		case AUT_HEADER32_EX:
			ev->type = tok.tt.hdr32_ex.e_type;
			if (aues && !auevent_type_in_typelist(ev->type, aues))
				goto skip_rec;
			ev->mod = tok.tt.hdr32_ex.e_mod;
			ev->tv.tv_sec = (time_t)tok.tt.hdr32_ex.s;
			ev->tv.tv_nsec = (long)tok.tt.hdr32_ex.ms*1000000;
			/* size, version */
			break;
		case AUT_HEADER64:
			ev->type = tok.tt.hdr64.e_type;
			if (aues && !auevent_type_in_typelist(ev->type, aues))
				goto skip_rec;
			ev->mod = tok.tt.hdr64.e_mod;
			ev->tv.tv_sec = (time_t)tok.tt.hdr64.s;
			ev->tv.tv_nsec = (long)tok.tt.hdr64.ms;
			/* size, version */
			break;
		case AUT_HEADER64_EX:
			ev->type = tok.tt.hdr64_ex.e_type;
			if (aues && !auevent_type_in_typelist(ev->type, aues))
				goto skip_rec;
			ev->mod = tok.tt.hdr64_ex.e_mod;
			ev->tv.tv_sec = (time_t)tok.tt.hdr64_ex.s;
			ev->tv.tv_nsec = (long)tok.tt.hdr64_ex.ms;
			/* size, version */
			break;
		case AUT_TRAILER:
			/* ignore */
			break;
		/* subject */
		case AUT_SUBJECT32:
			assert(!ev->subject_present);
			ev->subject_present = true;
			ev->subject.auid = tok.tt.subj32.auid;
			ev->subject.euid = tok.tt.subj32.euid;
			ev->subject.egid = tok.tt.subj32.egid;
			ev->subject.ruid = tok.tt.subj32.ruid;
			ev->subject.rgid = tok.tt.subj32.rgid;
			ev->subject.pid = tok.tt.subj32.pid;
			ev->subject.sid = tok.tt.subj32.sid;
			SET_DEV(ev->subject.dev, tok.tt.subj32.tid);
			SET_ADDR(ev->subject.addr, tok.tt.subj32.tid);
			break;
		case AUT_SUBJECT32_EX:
			assert(!ev->subject_present);
			ev->subject_present = true;
			ev->subject.auid = tok.tt.subj32_ex.auid;
			ev->subject.euid = tok.tt.subj32_ex.euid;
			ev->subject.egid = tok.tt.subj32_ex.egid;
			ev->subject.ruid = tok.tt.subj32_ex.ruid;
			ev->subject.rgid = tok.tt.subj32_ex.rgid;
			ev->subject.pid = tok.tt.subj32_ex.pid;
			ev->subject.sid = tok.tt.subj32_ex.sid;
			SET_DEV(ev->subject.dev, tok.tt.subj32_ex.tid);
			SET_ADDR_EX(ev->subject.addr, tok.tt.subj32_ex.tid);
			break;
		case AUT_SUBJECT64:
			assert(!ev->subject_present);
			ev->subject_present = true;
			ev->subject.auid = tok.tt.subj64.auid;
			ev->subject.euid = tok.tt.subj64.euid;
			ev->subject.egid = tok.tt.subj64.egid;
			ev->subject.ruid = tok.tt.subj64.ruid;
			ev->subject.rgid = tok.tt.subj64.rgid;
			ev->subject.pid = tok.tt.subj64.pid;
			ev->subject.sid = tok.tt.subj64.sid;
			SET_DEV(ev->subject.dev, tok.tt.subj64.tid);
			SET_ADDR(ev->subject.addr, tok.tt.subj64.tid);
			break;
		case AUT_SUBJECT64_EX:
			assert(!ev->subject_present);
			ev->subject_present = true;
			ev->subject.auid = tok.tt.subj64_ex.auid;
			ev->subject.euid = tok.tt.subj64_ex.euid;
			ev->subject.egid = tok.tt.subj64_ex.egid;
			ev->subject.ruid = tok.tt.subj64_ex.ruid;
			ev->subject.rgid = tok.tt.subj64_ex.rgid;
			ev->subject.pid = tok.tt.subj64_ex.pid;
			ev->subject.sid = tok.tt.subj64_ex.sid;
			SET_DEV(ev->subject.dev, tok.tt.subj64_ex.tid);
			SET_ADDR_EX(ev->subject.addr, tok.tt.subj64_ex.tid);
			break;
		/* process (as object, other than subject) */
		case AUT_PROCESS32:
			assert(!ev->process_present);
			ev->process_present = true;
			ev->process.auid = tok.tt.proc32.auid;
			ev->process.euid = tok.tt.proc32.euid;
			ev->process.egid = tok.tt.proc32.egid;
			ev->process.ruid = tok.tt.proc32.ruid;
			ev->process.rgid = tok.tt.proc32.rgid;
			ev->process.pid = tok.tt.proc32.pid;
			ev->process.sid = tok.tt.proc32.sid;
			SET_DEV(ev->process.dev, tok.tt.proc32.tid);
			SET_ADDR(ev->process.addr, tok.tt.proc32.tid);
			break;
		case AUT_PROCESS32_EX:
			assert(!ev->process_present);
			ev->process_present = true;
			ev->process.auid = tok.tt.proc32_ex.auid;
			ev->process.euid = tok.tt.proc32_ex.euid;
			ev->process.egid = tok.tt.proc32_ex.egid;
			ev->process.ruid = tok.tt.proc32_ex.ruid;
			ev->process.rgid = tok.tt.proc32_ex.rgid;
			ev->process.pid = tok.tt.proc32_ex.pid;
			ev->process.sid = tok.tt.proc32_ex.sid;
			SET_DEV(ev->process.dev, tok.tt.proc32_ex.tid);
			SET_ADDR_EX(ev->process.addr, tok.tt.proc32_ex.tid);
			break;
		case AUT_PROCESS64:
			assert(!ev->process_present);
			ev->process_present = true;
			ev->process.auid = tok.tt.proc64.auid;
			ev->process.euid = tok.tt.proc64.euid;
			ev->process.egid = tok.tt.proc64.egid;
			ev->process.ruid = tok.tt.proc64.ruid;
			ev->process.rgid = tok.tt.proc64.rgid;
			ev->process.pid = tok.tt.proc64.pid;
			ev->process.sid = tok.tt.proc64.sid;
			SET_DEV(ev->process.dev, tok.tt.proc64.tid);
			SET_ADDR(ev->process.addr, tok.tt.proc64.tid);
			break;
		case AUT_PROCESS64_EX:
			assert(!ev->process_present);
			ev->process_present = true;
			ev->process.auid = tok.tt.proc64_ex.auid;
			ev->process.euid = tok.tt.proc64_ex.euid;
			ev->process.egid = tok.tt.proc64_ex.egid;
			ev->process.ruid = tok.tt.proc64_ex.ruid;
			ev->process.rgid = tok.tt.proc64_ex.rgid;
			ev->process.pid = tok.tt.proc64_ex.pid;
			ev->process.sid = tok.tt.proc64_ex.sid;
			SET_DEV(ev->process.dev, tok.tt.proc64_ex.tid);
			SET_ADDR_EX(ev->process.addr, tok.tt.proc64_ex.tid);
			break;
		/* syscall arguments */
		case AUT_ARG32:
			/* tok.tt.arg32.no is zero-based */
			assert(!ev->args[tok.tt.arg32.no].present);
			ev->args[tok.tt.arg32.no].present = true;
			ev->args[tok.tt.arg32.no].value = tok.tt.arg32.val;
#ifdef DEBUG_AUDITPIPE
			ev->args[tok.tt.arg32.no].text =
				strdup(tok.tt.arg32.text);
			if (!ev->args[tok.tt.arg32.no].text)
				ev->flags |= AEFLAG_ENOMEM;
#endif /* DEBUG_AUDITPIPE */
			ev->args_count = max(ev->args_count,
			                     (size_t)tok.tt.arg32.no + 1);
			break;
		case AUT_ARG64:
			/* tok.tt.arg64.no is zero-based */
			assert(!ev->args[tok.tt.arg64.no].present);
			ev->args[tok.tt.arg64.no].present = true;
			ev->args[tok.tt.arg64.no].value = tok.tt.arg64.val;
#ifdef DEBUG_AUDITPIPE
			ev->args[tok.tt.arg64.no].text =
				strdup(tok.tt.arg64.text);
			if (!ev->args[tok.tt.arg64.no].text)
				ev->flags |= AEFLAG_ENOMEM;
#endif /* DEBUG_AUDITPIPE */
			ev->args_count = max(ev->args_count,
			                     (size_t)tok.tt.arg64.no + 1);
			break;
		/* syscall return value */
		case AUT_RETURN32:
			assert(!ev->return_present);
			ev->return_present = true;
			ev->return_error = tok.tt.ret32.status;
			ev->return_value = tok.tt.ret32.ret;
			break;
		case AUT_RETURN64:
			assert(!ev->return_present);
			ev->return_present = true;
			ev->return_error = tok.tt.ret64.err;
			ev->return_value = tok.tt.ret64.val;
			break;
		/* symlink text */
		case AUT_TEXT:
			if (!(textc < sizeof(ev->text)/sizeof(ev->text[0]))) {
				fprintf(stderr, "Too many text tokens, "
				                "skipping record\n");
				goto skip_rec;
			}
			ev->text[textc] = tok.tt.text.text;
			textc++;
			break;
		/* path */
		case AUT_PATH:
			/*
			 * Historically, on other BSM implementations, records
			 * for syscalls with a single path argument had only
			 * had a single path token.  However, macOS includes an
			 * unresolved and a resolved version of each token, as
			 * confirmed by Apple in radar 39267988 on 2018-06-13.
			 * Since there are syscalls with two path arguments, we
			 * store a maximum of four path arguments.
			 */
			if (!(pathc < sizeof(ev->path)/sizeof(ev->path[0]))) {
				fprintf(stderr, "Too many path tokens, "
				                "skipping record\n");
				goto skip_rec;
			}
			ev->path[pathc] = tok.tt.path.path;
			pathc++;
			break;
		/* attr */
		case AUT_ATTR32:
			if (!(ev->attr_count <
			      sizeof(ev->attr)/sizeof(ev->attr[0]))) {
				fprintf(stderr, "Too many attr tokens, "
				                "skipping record\n");
				goto skip_rec;
			}
			ev->attr[ev->attr_count].mode = tok.tt.attr32.mode;
			ev->attr[ev->attr_count].uid  = tok.tt.attr32.uid;
			ev->attr[ev->attr_count].gid  = tok.tt.attr32.gid;
			ev->attr[ev->attr_count].dev  = tok.tt.attr32.fsid;
			ev->attr[ev->attr_count].ino  = tok.tt.attr32.nid;
#if 0
			ev->attr[ev->attr_count].rdev = tok.tt.attr32.dev;
#endif
			ev->attr_count++;
			break;
		case AUT_ATTR64:
			if (!(ev->attr_count <
			      sizeof(ev->attr)/sizeof(ev->attr[0]))) {
				fprintf(stderr, "Too many attr tokens, "
				                "skipping record\n");
				goto skip_rec;
			}
			ev->attr[ev->attr_count].mode = tok.tt.attr64.mode;
			ev->attr[ev->attr_count].uid  = tok.tt.attr64.uid;
			ev->attr[ev->attr_count].gid  = tok.tt.attr64.gid;
			ev->attr[ev->attr_count].dev  = tok.tt.attr64.fsid;
			ev->attr[ev->attr_count].ino  = tok.tt.attr64.nid;
#if 0
			ev->attr[ev->attr_count].rdev = tok.tt.attr64.dev;
#endif
			ev->attr_count++;
			break;
		/* exec argv */
		case AUT_EXEC_ARGS:
			assert(ev->execarg == NULL);
			if (ev->execarg)
				free(ev->execarg);
			ev->execarg = aev_new(tok.tt.execarg.count,
			                      tok.tt.execarg.text);
			if (!ev->execarg)
				ev->flags |= AEFLAG_ENOMEM;
			break;
		/* exec env */
		case AUT_EXEC_ENV:
			if (!(flags & (AUEVENT_FLAG_ENV_DYLD |
			               AUEVENT_FLAG_ENV_FULL)))
				break;
			assert(ev->execenv == NULL);
			if (ev->execenv)
				free(ev->execenv);
			if (flags & AUEVENT_FLAG_ENV_DYLD) {
				ev->execenv = aev_new_prefix(
				              tok.tt.execenv.count,
				              tok.tt.execenv.text,
				              "DYLD_");
			} else {
				assert(flags & AUEVENT_FLAG_ENV_FULL);
				ev->execenv = aev_new(tok.tt.execenv.count,
				                      tok.tt.execenv.text);
			}
			if (!ev->execenv && errno == ENOMEM)
				ev->flags |= AEFLAG_ENOMEM;
			break;
		/* process exit status */
		case AUT_EXIT:
			assert(!ev->exit_present);
			ev->exit_present = true;
			ev->exit_status = tok.tt.exit.status;
			ev->exit_return = tok.tt.exit.ret;
			break;
		case AUT_SOCKINET32: /* Darwin */
			if (tok.tt.sockinet_ex32.family != BSM_PF_INET)
				break;
			ev->sockinet_addr.family = AF_INET;
			ev->sockinet_addr.ev_addr =
				tok.tt.sockinet_ex32.addr[0];
			ev->sockinet_port = ntohs(tok.tt.sockinet_ex32.port);
			break;
		case AUT_SOCKINET128: /* Darwin */
			if (tok.tt.sockinet_ex32.family != BSM_PF_INET6)
				break;
			ev->sockinet_addr.family = AF_INET6;
			ev->sockinet_addr.ev6_addr[0] =
				tok.tt.sockinet_ex32.addr[0];
			ev->sockinet_addr.ev6_addr[1] =
				tok.tt.sockinet_ex32.addr[1];
			ev->sockinet_addr.ev6_addr[2] =
				tok.tt.sockinet_ex32.addr[2];
			ev->sockinet_addr.ev6_addr[3] =
				tok.tt.sockinet_ex32.addr[3];
			/* AUT_SOCKINET128 has ports in host byte order.
			 * Reported to Apple as radar 43063872 on 2018-08-08.
			 * Need to differentiate here based on record version
			 * or macOS version once a fix is out. */
#ifdef RADAR43063872_FIXED
			if (radar_43063872_present) {
#endif
				ev->sockinet_port = tok.tt.sockinet_ex32.port;
#ifdef RADAR43063872_FIXED
			} else {
				ev->sockinet_port =
					ntohs(tok.tt.sockinet_ex32.port);
			}
#endif
			break;
		case AUT_SOCKUNIX: /* Darwin */
			/* ignore for now */
			break;
		/* unhandled tokens */
		default:
			for (int i = 0; i < 256; i++) {
				if (ev->unk_tokids[i] == tok.id)
					break;
				if (ev->unk_tokids[i] == 0) {
					ev->unk_tokids[i] = tok.id;
					break;
				}
			}
			break;
		}

#ifdef DEBUG_AUDITPIPE
		au_print_flags_tok(stderr, &tok, ":", AU_OFLAG_NONE);
		fprintf(stderr, "\n");
#endif
		recpos += tok.len;
	}

	return (ev->flags & AEFLAG_ENOMEM) ? -1 : 1;

skip_rec:
	return 0;
}

void
auevent_fprint(FILE *f, audit_event_t *ev) {
	struct au_event_ent *aue_ent;

	assert(ev);
	logutl_fwrite_timespec(f, &ev->tv);
	aue_ent = getauevnum(ev->type);
	fprintf(f, " %s [%i:%i]", aue_ent->ae_name, ev->type, ev->mod);
	if (ev->subject_present) {
		fprintf(f,
		        " subject_pid=%i"
		        " subject_sid=%"PRIu32
		        " subject_tid=/dev/%s[%s]"
		        " subject_auid=%u"
		        " subject_euid=%u"
		        " subject_egid=%u"
		        " subject_ruid=%u"
		        " subject_rgid=%u",
		        ev->subject.pid,
		        ev->subject.sid,
		        ev->subject.dev == -1 ? "-"
		                              : sys_ttydevname(ev->subject.dev),
		        ipaddrtoa(&ev->subject.addr, "-"),
		        ev->subject.auid,
		        ev->subject.euid,
		        ev->subject.egid,
		        ev->subject.ruid,
		        ev->subject.rgid);
	}
	if (ev->process_present) {
		fprintf(f,
		        " process_pid=%i"
		        " process_sid=%"PRIu32
		        " process_tid=/dev/%s[%s]"
		        " process_auid=%u"
		        " process_euid=%u"
		        " process_egid=%u"
		        " process_ruid=%u"
		        " process_rgid=%u",
		        ev->process.pid,
		        ev->process.sid,
		        ev->subject.dev == -1 ? "-"
		                              : sys_ttydevname(ev->process.dev),
		        ipaddrtoa(&ev->process.addr, "-"),
		        ev->process.auid,
		        ev->process.euid,
		        ev->process.egid,
		        ev->process.ruid,
		        ev->process.rgid);
	}
	for (size_t i = 0; i < ev->args_count; i++) {
		if (ev->args[i].present) {
#ifdef DEBUG_AUDITPIPE
			fprintf(f, " args[%zu:%s]=%"PRIu64, i,
			        ev->args[i].text,
			        ev->args[i].value);
#else
			fprintf(f, " args[%zu]=%"PRIu64, i,
			        ev->args[i].value);
#endif
		}
	}
	if (ev->return_present) {
		fprintf(f, " return_error=%u return_value=%"PRIu32,
		        ev->return_error, ev->return_value);
	}
	if (ev->exit_present) {
		fprintf(f, " exit_status=%"PRIu32" exit_return=%"PRIu32,
		        ev->exit_status, ev->exit_return);
	}
	for (size_t i = 0; i < sizeof(ev->text)/sizeof(ev->text[0]); i++) {
		if (ev->text[i]) {
			fprintf(f, " text[%zu]=%s", i, ev->text[i]);
		}
	}
	for (size_t i = 0; i < sizeof(ev->path)/sizeof(ev->path[0]); i++) {
		if (ev->path[i]) {
			fprintf(f, " path[%zu]='%s'", i, ev->path[i]);
		}
	}
	for (size_t i = 0; i < ev->attr_count; i++) {
		fprintf(f, " attr[%zu] mode=%o uid=%u gid=%u",
		        i,
		        ev->attr[i].mode,
		        ev->attr[i].uid,
		        ev->attr[i].gid);
	}
	if (ev->execarg) {
		fprintf(f, " execarg");
		for (size_t i = 0; ev->execarg[i]; i++) {
			fprintf(f, "%s'%s'", i ? " ": "=",
			        ev->execarg[i]);
		}
	}
	if (ev->execenv) {
		fprintf(f, " execenv");
		for (size_t i = 0; ev->execenv[i]; i++) {
			fprintf(f, "%s'%s'", i ? " ": "=",
			        ev->execenv[i]);
		}
	}
	if (ev->sockinet_present) {
		fprintf(f, " sockinet=%s:%i",
		        ipaddrtoa(&ev->sockinet_addr, "-"),
		        ev->sockinet_port);
	}
	if (ev->unk_tokids[0]) {
		fprintf(f, " unk_tokids");
		for (int i = 0; i < 256; i++) {
			if (ev->unk_tokids[i] == 0)
				break;
			fprintf(f, "%s0x%02x", i ? "," : "=",
			        ev->unk_tokids[i]);
		}
	}
	fprintf(f, "\n");
}

void
auevent_create(audit_event_t *ev) {
	assert(ev);
	bzero(ev, sizeof(audit_event_t));
}

void
auevent_destroy(audit_event_t *ev) {
	/* free raw event memory */
	if (ev->recbuf) {
		free(ev->recbuf);
		ev->recbuf = NULL;
	}
	if (ev->execarg) {
		free(ev->execarg);
		ev->execarg = NULL;
	}
	if (ev->execenv) {
		free(ev->execenv);
		ev->execenv = NULL;
	}
#ifdef DEBUG_AUDITPIPE
	for (size_t i = 0; i < ev->args_count; i++) {
		if (ev->args[i].present && ev->args[i].text) {
			free(ev->args[i].text);
			ev->args[i].text = NULL;
		}
	}
#endif /* DEBUG_AUDITPIPE */
}

/*
 * BSM uses domain/PF/AF and socket type constants derived from Solaris, which
 * unfortunately differ from BSD.  Hence the need to map them back into BSD
 * constants.  Cannot do this automatically because the constants are emitted
 * as generic arg tokens.
 */

int
auevent_sock_domain(int bsmdomain) {
	switch (bsmdomain) {
	case BSM_PF_UNSPEC:             /*   0 */
		return PF_UNSPEC;       /*   0 */
	case BSM_PF_LOCAL:              /*   1 */
		return PF_UNIX;         /*   1 */
	case BSM_PF_INET:               /*   2 */
		return PF_INET;         /*   2 */
	case BSM_PF_ROUTE:              /*  24 */
		return PF_ROUTE;        /*  17 */
	case BSM_PF_KEY:                /*  27 */
		return PF_KEY;          /*  29 */
	case BSM_PF_INET6:              /*  26 */
		return PF_INET6;        /*  30 */
	/* ... */
	case BSM_PF_UNKNOWN:            /* 700 */
	default:
		return -1;
	}
}

int
auevent_sock_type(int bsmtype) {
	switch (bsmtype) {
	case BSM_SOCK_DGRAM:            /*   1 */
		return SOCK_DGRAM;      /*   2 */
	case BSM_SOCK_STREAM:           /*   2 */
		return SOCK_STREAM;     /*   1 */
	case BSM_SOCK_RAW:              /*   4 */
		return SOCK_RAW;        /*   3 */
	case BSM_SOCK_RDM:              /*   5 */
		return SOCK_RDM;        /*   4 */
	case BSM_SOCK_SEQPACKET:        /*   6 */
		return SOCK_SEQPACKET;  /*   5 */
	case BSM_SOCK_UNKNOWN:          /* 500 */
	default:
		return -1;
	}
}

