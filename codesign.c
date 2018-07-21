/*-
 * xnumon - monitor macOS for malicious activity
 * https://www.roe.ch/xnumon
 *
 * Copyright (c) 2017-2018, Daniel Roethlisberger <daniel@roe.ch>.
 * All rights reserved.
 *
 * Licensed under the Open Software License version 3.0.
 */

#include "codesign.h"

#include "cf.h"

#include <stdio.h>
#include <stdlib.h>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

typedef struct {
	int origin;
	SecRequirementRef req;
} origin_req_tuple_t;

origin_req_tuple_t reqs[] = {
	{CODESIGN_ORIGIN_APPLE_SYSTEM, NULL},
	{CODESIGN_ORIGIN_MAC_APP_STORE, NULL},
	{CODESIGN_ORIGIN_DEVELOPER_ID, NULL},
	{CODESIGN_ORIGIN_APPLE_GENERIC, NULL},
};

#define CREATE_REQ(REQ, REQSTR) \
{ \
	REQ = NULL; \
	if (SecRequirementCreateWithString(CFSTR(REQSTR), \
	                                   kSecCSDefaultFlags, \
	                                   &REQ) != errSecSuccess || !REQ) \
		return -1; \
}

int
codesign_init() {
	CREATE_REQ(reqs[0].req, "anchor apple");
	CREATE_REQ(reqs[1].req, "anchor apple generic and "
		"certificate leaf[field.1.2.840.113635.100.6.1.9] exists");
	CREATE_REQ(reqs[2].req, "anchor apple generic and "
		"certificate 1[field.1.2.840.113635.100.6.2.6] exists and "
		"certificate leaf[field.1.2.840.113635.100.6.1.13] exists");
	CREATE_REQ(reqs[3].req, "anchor apple generic");
	return 0;
}

void
codesign_fini() {
	for (size_t i = 0; i < sizeof(reqs)/sizeof(origin_req_tuple_t); i++) {
		if (reqs[i].req) {
			CFRelease(reqs[i].req);
			reqs[i].req = NULL;
		}
	}
}

#undef CREATE_REQ

void
codesign_free(codesign_t *cs) {
	if (cs->ident)
		free(cs->ident);
	if (cs->cdhash)
		free(cs->cdhash);
	if (cs->teamid)
		free(cs->teamid);
	if (cs->devid)
		free(cs->devid);
	free(cs);
}

codesign_t *
codesign_dup(const codesign_t *other) {
	codesign_t *cs;

	cs = malloc(sizeof(codesign_t));
	if (!cs)
		return NULL;
	bzero(cs, sizeof(codesign_t));

	cs->result = other->result;
	cs->origin = other->origin;
	cs->error = other->error;
	if (other->ident) {
		cs->ident = strdup(other->ident);
		if (!cs->ident)
			goto errout;
	}
	if (other->cdhash) {
		cs->cdhashsz = other->cdhashsz;
		cs->cdhash = malloc(cs->cdhashsz);
		if (!cs->cdhash)
			goto errout;
		memcpy(cs->cdhash, other->cdhash, cs->cdhashsz);
	}
	if (other->teamid) {
		cs->teamid = strdup(other->teamid);
		if (!cs->teamid)
			goto errout;
	}
	if (other->devid) {
		cs->devid = strdup(other->devid);
		if (!cs->devid)
			goto errout;
	}
	return cs;
errout:
	codesign_free(cs);
	return NULL;
}

codesign_t *
codesign_new(const char *cpath) {
	codesign_t *cs;
	OSStatus rv;

	assert(cpath);

	cs = malloc(sizeof(codesign_t));
	if (!cs)
		goto enomemout;
	bzero(cs, sizeof(codesign_t));

	CFURLRef url = cf_url(cpath);
	if (!url)
		goto enomemout;

	SecStaticCodeRef scode = NULL;
	rv = SecStaticCodeCreateWithPath(url, kSecCSDefaultFlags, &scode);
	CFRelease(url);
	if (rv != errSecSuccess) {
		cs->error = rv;
		cs->result = CODESIGN_RESULT_ERROR;
		return cs;
	}

	/* verify signature using embedded designated requirement */
	SecRequirementRef designated_req = NULL;
	rv = SecCodeCopyDesignatedRequirement(scode, kSecCSDefaultFlags,
	                                      &designated_req);
	switch (rv) {
	case errSecSuccess:
		break;
	case errSecCSUnsigned:
		cs->result = CODESIGN_RESULT_UNSIGNED;
		CFRelease(scode);
		return cs;
	default:
		cs->error = rv;
		cs->result = CODESIGN_RESULT_ERROR;
		CFRelease(scode);
		return cs;
	}
	rv = SecStaticCodeCheckValidity(scode,
	                                kSecCSDefaultFlags|
	                                kSecCSCheckAllArchitectures|
	                                kSecCSStrictValidate|
	                                kSecCSCheckNestedCode|
	                                kSecCSEnforceRevocationChecks|
	                                kSecCSConsiderExpiration,
	                                designated_req);
	CFRelease(designated_req);
	if (rv != errSecSuccess) {
		cs->result = CODESIGN_RESULT_BAD;
		CFRelease(scode);
		return cs;
	}

	/* retrieve information from signature */
	CFDictionaryRef dict = NULL;
	rv = SecCodeCopySigningInformation(scode,
	                                   kSecCSSigningInformation|
	                                   kSecCSInternalInformation|
	                                   kSecCSRequirementInformation,
	                                   &dict);
	if (rv != errSecSuccess || !dict) {
		CFRelease(scode);
		cs->error = rv;
		cs->result = CODESIGN_RESULT_ERROR;
		return cs;
	}

	/* copy ident string; signed implies ident string is present */
	CFStringRef ident = CFDictionaryGetValue(dict, kSecCodeInfoIdentifier);
	if (ident && cf_is_string(ident)) {
		cs->ident = cf_cstr(ident);
		if (!cs->ident) {
			CFRelease(scode);
			CFRelease(dict);
			goto enomemout;
		}
	} else {
		CFRelease(scode);
		CFRelease(dict);
		cs->result = CODESIGN_RESULT_BAD;
		return cs;
	}
	assert(ident && cs->ident);

	/* reduced set of flags, we are only checking requirements here */
	SecCSFlags csflags = kSecCSDefaultFlags|
	                     kSecCSCheckAllArchitectures|
	                     kSecCSStrictValidate;
	for (size_t i = 0; i < sizeof(reqs)/sizeof(origin_req_tuple_t); i++) {
		rv = SecStaticCodeCheckValidity(scode, csflags, reqs[i].req);
		if (rv == errSecSuccess) {
			cs->origin = reqs[i].origin;
			break;
		}
	}
	CFRelease(scode);
	if (rv != errSecSuccess) {
		/* we are treating ad-hoc signatures as bad signatures;
		 * might want to change this at some point */
		CFRelease(dict);
		free(cs->ident);
		cs->ident = NULL;
		cs->result = CODESIGN_RESULT_BAD;
		return cs;
	}

	/* extract CDHash */
	CFDataRef cdhash = CFDictionaryGetValue(dict, kSecCodeInfoUnique);
	if (cdhash && cf_is_data(cdhash)) {
		cs->cdhashsz = CFDataGetLength(cdhash);
		cs->cdhash = malloc(cs->cdhashsz);
		if (!cs->cdhash) {
			CFRelease(dict);
			goto enomemout;
		}
		memcpy(cs->cdhash, CFDataGetBytePtr(cdhash), cs->cdhashsz);
	}

	/* skip Team ID and Developer ID extraction for Apple System sigs */
	if (cs->origin == CODESIGN_ORIGIN_APPLE_SYSTEM)
		goto out;

	/* extract Team ID associated with the signing Developer ID */
	CFStringRef teamid = CFDictionaryGetValue(dict,
	                                          kSecCodeInfoTeamIdentifier);
	if (teamid && cf_is_string(teamid)) {
		cs->teamid = cf_cstr(teamid);
		if (!cs->teamid) {
			CFRelease(dict);
			goto enomemout;
		}
	}

	/* skip Developer ID extraction unless sig origin is Developer ID */
	if (cs->origin != CODESIGN_ORIGIN_DEVELOPER_ID)
		goto out;

	/* extract Developer ID from CN of first certificate in chain */
	CFArrayRef chain = CFDictionaryGetValue(dict,
	                                        kSecCodeInfoCertificates);
	if (chain && cf_is_array(chain) && CFArrayGetCount(chain) >= 1) {
		SecCertificateRef crt =
		        (SecCertificateRef)CFArrayGetValueAtIndex(chain, 0);
		if (crt && cf_is_cert(crt)) {
			CFStringRef s = SecCertificateCopySubjectSummary(crt);
			if (!s) {
				CFRelease(dict);
				goto enomemout;
			}
			cs->devid = cf_cstr(s);
			CFRelease(s);
			if (!cs->devid) {
				CFRelease(dict);
				goto enomemout;
			}
		}
	}

out:
	CFRelease(dict);
	cs->result = CODESIGN_RESULT_GOOD;
	return cs;

enomemout:
	if (cs)
		codesign_free(cs);
	errno = ENOMEM;
	return NULL;
}

const char *
codesign_result_s(codesign_t *cs) {
	switch (cs->result) {
	case CODESIGN_RESULT_UNSIGNED:
		return "unsigned";
	case CODESIGN_RESULT_GOOD:
		return "good";
	case CODESIGN_RESULT_BAD:
		return "bad";
	case CODESIGN_RESULT_ERROR:
		return "error";
	default:
		/* this should never happen */
		return "undefined";
	}
}

const char *
codesign_origin_s(codesign_t *cs) {
	switch (cs->origin) {
	case CODESIGN_ORIGIN_APPLE_SYSTEM:
		return "system";
	case CODESIGN_ORIGIN_MAC_APP_STORE:
		return "appstore";
	case CODESIGN_ORIGIN_DEVELOPER_ID:
		return "devid";
	case CODESIGN_ORIGIN_APPLE_GENERIC:
		return "generic";
	default:
		/* this should never happen if a signature is present */
		return "undefined";
	}
}

void
codesign_fprint(FILE *f, codesign_t *cs) {
	fprintf(f, "signature: %s\n", codesign_result_s(cs));
	if (cs->origin)
		fprintf(f, "origin: %s\n", codesign_origin_s(cs));
	if (cs->error)
		fprintf(f, "error: %lu\n", cs->error);
	if (cs->ident)
		fprintf(f, "ident: %s\n", cs->ident);
	if (cs->cdhash) {
		fprintf(f, "cdhash: ");
		for (size_t i = 0; i < cs->cdhashsz; i++) {
			fprintf(f, "%02x", cs->cdhash[i]);
		}
		fprintf(f, "\n");
	}
	if (cs->teamid)
		fprintf(f, "teamid: %s\n", cs->teamid);
	if (cs->devid)
		fprintf(f, "devid: %s\n", cs->devid);
}

