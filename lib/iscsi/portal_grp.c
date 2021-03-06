/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"

#include "spdk/conf.h"
#include "spdk/net.h"
#include "spdk/event.h"
#include "spdk/string.h"

#include "spdk_internal/log.h"

#include "iscsi/iscsi.h"
#include "iscsi/conn.h"
#include "iscsi/portal_grp.h"
#include "iscsi/acceptor.h"

#define PORTNUMSTRLEN 32

static int
spdk_iscsi_portal_grp_open(struct spdk_iscsi_portal_grp *pg);

static struct spdk_iscsi_portal *
spdk_iscsi_portal_find_by_addr(const char *host, const char *port)
{
	struct spdk_iscsi_portal *p;

	TAILQ_FOREACH(p, &g_spdk_iscsi.portal_head, g_tailq) {
		if (!strcmp(p->host, host) && !strcmp(p->port, port)) {
			return p;
		}
	}

	return NULL;
}

/* Assumes caller allocated host and port strings on the heap */
struct spdk_iscsi_portal *
spdk_iscsi_portal_create(const char *host, const char *port, const char *cpumask)
{
	struct spdk_iscsi_portal *p = NULL;
	struct spdk_cpuset *core_mask = NULL;
	int rc;

	assert(host != NULL);
	assert(port != NULL);

	p = spdk_iscsi_portal_find_by_addr(host, port);
	if (p != NULL) {
		SPDK_ERRLOG("portal (%s, %s) already exists\n", host, port);
		return NULL;
	}

	p = calloc(1, sizeof(*p));
	if (!p) {
		SPDK_ERRLOG("calloc() failed for portal\n");
		return NULL;
	}

	/* check and overwrite abbreviation of wildcard */
	if (strcasecmp(host, "[*]") == 0) {
		SPDK_WARNLOG("Please use \"[::]\" as IPv6 wildcard\n");
		SPDK_WARNLOG("Convert \"[*]\" to \"[::]\" automatically\n");
		SPDK_WARNLOG("(Use of \"[*]\" will be deprecated in a future release)");
		p->host = strdup("[::]");
	} else if (strcasecmp(host, "*") == 0) {
		SPDK_WARNLOG("Please use \"0.0.0.0\" as IPv4 wildcard\n");
		SPDK_WARNLOG("Convert \"*\" to \"0.0.0.0\" automatically\n");
		SPDK_WARNLOG("(Use of \"[*]\" will be deprecated in a future release)");
		p->host = strdup("0.0.0.0");
	} else {
		p->host = strdup(host);
	}
	if (!p->host) {
		SPDK_ERRLOG("strdup() failed for host\n");
		goto error_out;
	}

	p->port = strdup(port);
	if (!p->port) {
		SPDK_ERRLOG("strdup() failed for host\n");
		goto error_out;
	}

	core_mask = spdk_cpuset_alloc();
	if (!core_mask) {
		SPDK_ERRLOG("spdk_cpuset_alloc() failed for host\n");
		goto error_out;
	}

	if (cpumask != NULL) {
		rc = spdk_app_parse_core_mask(cpumask, core_mask);
		if (rc < 0) {
			SPDK_ERRLOG("cpumask (%s) is invalid\n", cpumask);
			goto error_out;
		}
		if (spdk_cpuset_count(core_mask) == 0) {
			SPDK_ERRLOG("cpumask (%s) does not contain core mask (0x%s)\n",
				    cpumask, spdk_cpuset_fmt(spdk_app_get_core_mask()));
			goto error_out;
		}
	} else {
		spdk_cpuset_copy(core_mask, spdk_app_get_core_mask());
	}

	p->cpumask = core_mask;

	p->sock = -1;
	p->group = NULL; /* set at a later time by caller */
	p->acceptor_poller = NULL;

	TAILQ_INSERT_TAIL(&g_spdk_iscsi.portal_head, p, g_tailq);

	return p;

error_out:
	spdk_cpuset_free(core_mask);
	free(p->port);
	free(p->host);
	free(p);

	return NULL;
}

void
spdk_iscsi_portal_destroy(struct spdk_iscsi_portal *p)
{
	assert(p != NULL);

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "spdk_iscsi_portal_destroy\n");
	TAILQ_REMOVE(&g_spdk_iscsi.portal_head, p, g_tailq);
	free(p->host);
	free(p->port);
	spdk_cpuset_free(p->cpumask);
	free(p);
}

static int
spdk_iscsi_portal_open(struct spdk_iscsi_portal *p)
{
	int port, sock;

	if (p->sock >= 0) {
		SPDK_ERRLOG("portal (%s, %s) is already opened\n",
			    p->host, p->port);
		return -1;
	}

	port = (int)strtol(p->port, NULL, 0);
	sock = spdk_sock_listen(p->host, port);
	if (sock < 0) {
		SPDK_ERRLOG("listen error %.64s.%d\n", p->host, port);
		return -1;
	}

	p->sock = sock;

	spdk_iscsi_acceptor_start(p);

	return 0;
}

static void
spdk_iscsi_portal_close(struct spdk_iscsi_portal *p)
{
	if (p->sock >= 0) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "close portal (%s, %s)\n",
			      p->host, p->port);
		spdk_iscsi_acceptor_stop(p);
		spdk_sock_close(p->sock);
		p->sock = -1;
	}
}

static int
spdk_iscsi_portal_create_from_configline(const char *portalstring,
		struct spdk_iscsi_portal **ip,
		int dry_run)
{
	char *host = NULL, *port = NULL, *cpumask = NULL;
	int len, rc = -1;
	const char *p, *q;

	if (portalstring == NULL) {
		SPDK_ERRLOG("portal error\n");
		goto error_out;
	}

	/* IP address */
	if (portalstring[0] == '[') {
		/* IPv6 */
		p = strchr(portalstring + 1, ']');
		if (p == NULL) {
			SPDK_ERRLOG("portal error\n");
			goto error_out;
		}
		p++;
	} else {
		/* IPv4 */
		p = strchr(portalstring, ':');
		if (p == NULL) {
			p = portalstring + strlen(portalstring);
		}
	}

	if (!dry_run) {
		len = p - portalstring;
		host = malloc(len + 1);
		if (host == NULL) {
			SPDK_ERRLOG("malloc() failed for host\n");
			goto error_out;
		}
		strncpy(host, portalstring, len);
		host[len] = '\0';
	}

	/* Port number (IPv4 and IPv6 are the same) */
	if (p[0] == '\0') {
		if (!dry_run) {
			port = malloc(PORTNUMSTRLEN);
			if (!port) {
				SPDK_ERRLOG("malloc() failed for port\n");
				goto error_out;
			}
			snprintf(port, PORTNUMSTRLEN, "%d", DEFAULT_PORT);
		}
	} else {
		if (p[0] != ':') {
			SPDK_ERRLOG("portal error\n");
			goto error_out;
		}
		q = strchr(portalstring, '@');
		if (q == NULL) {
			q = portalstring + strlen(portalstring);
		}
		if (q == p) {
			SPDK_ERRLOG("no port specified\n");
			goto error_out;
		}

		if (!dry_run) {
			len = q - p - 1;
			port = malloc(len + 1);
			if (port == NULL) {
				SPDK_ERRLOG("malloc() failed for port\n");
				goto error_out;
			}
			strncpy(port, p + 1, len);
			port[len] = '\0';
		}
	}

	/* Cpumask (IPv4 and IPv6 are the same) */
	p = strchr(portalstring, '@');
	if (p != NULL) {
		q = portalstring + strlen(portalstring);
		if (q == p) {
			SPDK_ERRLOG("no cpumask specified\n");
			goto error_out;
		}
		if (!dry_run) {
			len = q - p - 1;
			cpumask = malloc(len + 1);
			if (cpumask == NULL) {
				SPDK_ERRLOG("malloc() failed for cpumask\n");
				goto error_out;
			}
			strncpy(cpumask, p + 1, len);
			cpumask[len] = '\0';
		}
	}

	if (!dry_run) {
		*ip = spdk_iscsi_portal_create(host, port, cpumask);
		if (!*ip) {
			goto error_out;
		}
	}

	rc = 0;
error_out:
	free(host);
	free(port);
	free(cpumask);

	return rc;
}

static struct spdk_iscsi_portal_grp *
spdk_iscsi_portal_grp_create(int tag)
{
	struct spdk_iscsi_portal_grp *pg = malloc(sizeof(*pg));

	if (!pg) {
		SPDK_ERRLOG("malloc() failed for portal group\n");
		return NULL;
	}

	/* Make sure there are no duplicate portal group tags */
	if (spdk_iscsi_portal_grp_find_by_tag(tag)) {
		SPDK_ERRLOG("portal group creation failed.  duplicate portal group tag (%d)\n", tag);
		free(pg);
		return NULL;
	}

	pg->ref = 0;
	pg->tag = tag;

	TAILQ_INIT(&pg->head);

	return pg;
}

static void
spdk_iscsi_portal_grp_destroy(struct spdk_iscsi_portal_grp *pg)
{
	struct spdk_iscsi_portal	*p;

	assert(pg != NULL);

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "spdk_iscsi_portal_grp_destroy\n");
	while (!TAILQ_EMPTY(&pg->head)) {
		p = TAILQ_FIRST(&pg->head);
		TAILQ_REMOVE(&pg->head, p, per_pg_tailq);
		spdk_iscsi_portal_destroy(p);
	}
	free(pg);
}

static void
spdk_iscsi_portal_grp_register(struct spdk_iscsi_portal_grp *pg)
{
	assert(pg != NULL);
	assert(!TAILQ_EMPTY(&pg->head));

	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	TAILQ_INSERT_TAIL(&g_spdk_iscsi.pg_head, pg, tailq);
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
}

static void
spdk_iscsi_portal_grp_add_portal(struct spdk_iscsi_portal_grp *pg,
				 struct spdk_iscsi_portal *p)
{
	assert(pg != NULL);
	assert(p != NULL);

	p->group = pg;
	TAILQ_INSERT_TAIL(&pg->head, p, per_pg_tailq);
}

/**
 * If all portals are valid, this function will take their ownership.
 */
int
spdk_iscsi_portal_grp_create_from_portal_list(int tag,
		struct spdk_iscsi_portal **portal_list,
		int num_portals)
{
	int i = 0, rc = 0;
	struct spdk_iscsi_portal_grp *pg;

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "add portal group (from portal list) %d\n", tag);

	if (num_portals > MAX_PORTAL) {
		SPDK_ERRLOG("%d > MAX_PORTAL\n", num_portals);
		return -1;
	}

	pg = spdk_iscsi_portal_grp_create(tag);
	if (!pg) {
		SPDK_ERRLOG("portal group creation error (%d)\n", tag);
		return -1;
	}

	for (i = 0; i < num_portals; i++) {
		struct spdk_iscsi_portal *p = portal_list[i];

		SPDK_DEBUGLOG(SPDK_LOG_ISCSI,
			      "RIndex=%d, Host=%s, Port=%s, Tag=%d\n",
			      i, p->host, p->port, tag);
		rc = spdk_iscsi_portal_open(p);
		if (rc < 0) {
			/* if listening failed on any port, do not register the portal group
			 * and close any previously opened. */
			for (--i; i >= 0; --i) {
				spdk_iscsi_portal_close(portal_list[i]);
			}

			break;
		}
	}

	if (rc < 0) {
		spdk_iscsi_portal_grp_destroy(pg);
	} else {
		/* Add portals to portal group */
		for (i = 0; i < num_portals; i++) {
			spdk_iscsi_portal_grp_add_portal(pg, portal_list[i]);
		}

		/* Add portal group to the end of the pg list */
		spdk_iscsi_portal_grp_register(pg);
	}

	return rc;
}

static int
spdk_iscsi_portal_grp_create_from_configfile(struct spdk_conf_section *sp)
{
	struct spdk_iscsi_portal_grp *pg;
	struct spdk_iscsi_portal	*p;
	const char *val;
	char *label, *portal;
	int portals = 0, i = 0, rc = 0;

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "add portal group (from config file) %d\n",
		      spdk_conf_section_get_num(sp));

	val = spdk_conf_section_get_val(sp, "Comment");
	if (val != NULL) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "Comment %s\n", val);
	}

	/* counts number of definitions */
	for (i = 0; ; i++) {
		/*
		 * label is no longer used, but we keep it in the config
		 *  file definition so that we do not break existing config
		 *  files.
		 */
		label = spdk_conf_section_get_nmval(sp, "Portal", i, 0);
		portal = spdk_conf_section_get_nmval(sp, "Portal", i, 1);
		if (label == NULL || portal == NULL) {
			break;
		}
		rc = spdk_iscsi_portal_create_from_configline(portal, &p, 1);
		if (rc < 0) {
			SPDK_ERRLOG("parse portal error (%s)\n", portal);
			goto error_out;
		}
	}

	portals = i;
	if (portals > MAX_PORTAL) {
		SPDK_ERRLOG("%d > MAX_PORTAL\n", portals);
		goto error_out;
	}

	pg = spdk_iscsi_portal_grp_create(spdk_conf_section_get_num(sp));
	if (!pg) {
		SPDK_ERRLOG("portal group malloc error (%s)\n", spdk_conf_section_get_name(sp));
		goto error_out;
	}

	for (i = 0; i < portals; i++) {
		label = spdk_conf_section_get_nmval(sp, "Portal", i, 0);
		portal = spdk_conf_section_get_nmval(sp, "Portal", i, 1);
		if (label == NULL || portal == NULL) {
			spdk_iscsi_portal_grp_destroy(pg);
			SPDK_ERRLOG("portal error\n");
			goto error_out;
		}

		rc = spdk_iscsi_portal_create_from_configline(portal, &p, 0);
		if (rc < 0) {
			spdk_iscsi_portal_grp_destroy(pg);
			SPDK_ERRLOG("parse portal error (%s)\n", portal);
			goto error_out;
		}

		SPDK_DEBUGLOG(SPDK_LOG_ISCSI,
			      "RIndex=%d, Host=%s, Port=%s, Tag=%d\n",
			      i, p->host, p->port, spdk_conf_section_get_num(sp));

		spdk_iscsi_portal_grp_add_portal(pg, p);
	}

	/* Add portal group to the end of the pg list */
	spdk_iscsi_portal_grp_register(pg);

	return 0;

error_out:
	return -1;
}

struct spdk_iscsi_portal_grp *
spdk_iscsi_portal_grp_find_by_tag(int tag)
{
	struct spdk_iscsi_portal_grp *pg;

	TAILQ_FOREACH(pg, &g_spdk_iscsi.pg_head, tailq) {
		if (pg->tag == tag) {
			return pg;
		}
	}

	return NULL;
}

int
spdk_iscsi_portal_grp_array_create(void)
{
	int rc = 0;
	struct spdk_conf_section *sp;

	TAILQ_INIT(&g_spdk_iscsi.portal_head);
	TAILQ_INIT(&g_spdk_iscsi.pg_head);
	sp = spdk_conf_first_section(NULL);
	while (sp != NULL) {
		if (spdk_conf_section_match_prefix(sp, "PortalGroup")) {
			if (spdk_conf_section_get_num(sp) == 0) {
				SPDK_ERRLOG("Group 0 is invalid\n");
				return -1;
			}

			/* Build portal group from cfg section PortalGroup */
			rc = spdk_iscsi_portal_grp_create_from_configfile(sp);
			if (rc < 0) {
				SPDK_ERRLOG("parse_portal_group() failed\n");
				return -1;
			}
		}
		sp = spdk_conf_next_section(sp);
	}
	return 0;
}

void
spdk_iscsi_portal_grp_array_destroy(void)
{
	struct spdk_iscsi_portal_grp *pg, *tmp;

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "spdk_iscsi_portal_grp_array_destroy\n");
	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	TAILQ_FOREACH_SAFE(pg, &g_spdk_iscsi.pg_head, tailq, tmp) {
		TAILQ_REMOVE(&g_spdk_iscsi.pg_head, pg, tailq);
		spdk_iscsi_portal_grp_destroy(pg);
	}
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
}

static int
spdk_iscsi_portal_grp_open(struct spdk_iscsi_portal_grp *pg)
{
	struct spdk_iscsi_portal *p;
	int rc;

	TAILQ_FOREACH(p, &pg->head, per_pg_tailq) {
		rc = spdk_iscsi_portal_open(p);
		if (rc < 0) {
			return rc;
		}
	}
	return 0;
}

int
spdk_iscsi_portal_grp_open_all(void)
{
	struct spdk_iscsi_portal_grp *pg;
	int rc;

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "spdk_iscsi_portal_grp_open_all\n");
	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	TAILQ_FOREACH(pg, &g_spdk_iscsi.pg_head, tailq) {
		rc = spdk_iscsi_portal_grp_open(pg);
		if (rc < 0) {
			pthread_mutex_unlock(&g_spdk_iscsi.mutex);
			return -1;
		}
	}
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
	return 0;
}

static void
spdk_iscsi_portal_grp_close(struct spdk_iscsi_portal_grp *pg)
{
	struct spdk_iscsi_portal *p;

	TAILQ_FOREACH(p, &pg->head, per_pg_tailq) {
		spdk_iscsi_portal_close(p);
	}
}

void
spdk_iscsi_portal_grp_close_all(void)
{
	struct spdk_iscsi_portal_grp *pg;

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "spdk_iscsi_portal_grp_close_all\n");
	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	TAILQ_FOREACH(pg, &g_spdk_iscsi.pg_head, tailq) {
		spdk_iscsi_portal_grp_close(pg);
	}
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
}

static inline void
spdk_iscsi_portal_grp_unregister(struct spdk_iscsi_portal_grp *pg)
{
	struct spdk_iscsi_portal_grp *portal_group;
	struct spdk_iscsi_portal_grp *portal_group_tmp;

	assert(pg != NULL);
	assert(!TAILQ_EMPTY(&pg->head));

	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	TAILQ_FOREACH_SAFE(portal_group, &g_spdk_iscsi.pg_head, tailq, portal_group_tmp) {
		if (portal_group->tag == pg->tag) {
			TAILQ_REMOVE(&g_spdk_iscsi.pg_head, portal_group, tailq);
		}
	}
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
}

void
spdk_iscsi_portal_grp_release(struct spdk_iscsi_portal_grp *pg)
{
	spdk_iscsi_portal_grp_close(pg);
	spdk_iscsi_portal_grp_unregister(pg);
	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	spdk_iscsi_portal_grp_destroy(pg);
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
}
