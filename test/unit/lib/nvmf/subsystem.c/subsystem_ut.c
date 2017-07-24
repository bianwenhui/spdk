/*-
 *   BSD LICENSE
 *
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

#include "spdk_cunit.h"
#include "subsystem.h"

const struct spdk_nvmf_ctrlr_ops spdk_nvmf_bdev_ctrlr_ops;
const struct spdk_nvmf_ctrlr_ops spdk_nvmf_discovery_ctrlr_ops;

#include "subsystem.c"

SPDK_LOG_REGISTER_TRACE_FLAG("nvmf", SPDK_TRACE_NVMF)

struct spdk_nvmf_tgt g_nvmf_tgt;

struct spdk_nvmf_listen_addr *
spdk_nvmf_listen_addr_create(struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_listen_addr *listen_addr;

	listen_addr = calloc(1, sizeof(*listen_addr));
	if (!listen_addr) {
		return NULL;
	}

	listen_addr->trid = *trid;

	return listen_addr;
}

void
spdk_nvmf_listen_addr_destroy(struct spdk_nvmf_listen_addr *addr)
{
	free(addr);
}

static int
test_transport1_listen_addr_add(struct spdk_nvmf_transport *transport,
				struct spdk_nvmf_listen_addr *listen_addr)
{
	return 0;
}

static void
test_transport1_listen_addr_discover(struct spdk_nvmf_transport *transport,
				     struct spdk_nvmf_listen_addr *listen_addr,
				     struct spdk_nvmf_discovery_log_page_entry *entry)
{
	entry->trtype = 42;
}

static const struct spdk_nvmf_transport_ops test_transport1_ops = {
	.listen_addr_add = test_transport1_listen_addr_add,
	.listen_addr_discover = test_transport1_listen_addr_discover,
};

static struct spdk_nvmf_transport test_transport1 = {
	.ops = &test_transport1_ops,
};

struct spdk_nvmf_transport *
spdk_nvmf_transport_create(struct spdk_nvmf_tgt *tgt,
			   enum spdk_nvme_transport_type type)
{
	if (type == SPDK_NVME_TRANSPORT_RDMA) {
		test_transport1.tgt = tgt;
		return &test_transport1;
	}

	return NULL;
}

struct spdk_nvmf_transport *
spdk_nvmf_tgt_get_transport(struct spdk_nvmf_tgt *tgt, enum spdk_nvme_transport_type trtype)
{
	if (trtype == SPDK_NVME_TRANSPORT_RDMA) {
		test_transport1.tgt = tgt;
		return &test_transport1;
	}

	return NULL;
}

int
spdk_nvme_transport_id_parse_trtype(enum spdk_nvme_transport_type *trtype, const char *str)
{
	if (trtype == NULL || str == NULL) {
		return -EINVAL;
	}

	if (strcasecmp(str, "PCIe") == 0) {
		*trtype = SPDK_NVME_TRANSPORT_PCIE;
	} else if (strcasecmp(str, "RDMA") == 0) {
		*trtype = SPDK_NVME_TRANSPORT_RDMA;
	} else {
		return -ENOENT;
	}
	return 0;
}

int
spdk_nvme_transport_id_compare(const struct spdk_nvme_transport_id *trid1,
			       const struct spdk_nvme_transport_id *trid2)
{
	return 0;
}

int32_t
spdk_nvme_ctrlr_process_admin_completions(struct spdk_nvme_ctrlr *ctrlr)
{
	return -1;
}

int32_t
spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	return -1;
}

int
spdk_nvme_detach(struct spdk_nvme_ctrlr *ctrlr)
{
	return -1;
}

void
spdk_nvmf_ctrlr_destruct(struct spdk_nvmf_ctrlr *ctrlr)
{
}

int
spdk_nvmf_ctrlr_poll(struct spdk_nvmf_ctrlr *ctrlr)
{
	return -1;
}

int
spdk_bdev_open(struct spdk_bdev *bdev, bool write, spdk_bdev_remove_cb_t remove_cb,
	       void *remove_ctx, struct spdk_bdev_desc **desc)
{
	return 0;
}

const char *
spdk_bdev_get_name(const struct spdk_bdev *bdev)
{
	return "test";
}

static void
test_spdk_nvmf_tgt_listen(void)
{
	struct spdk_nvmf_listen_addr *listen_addr;
	struct spdk_nvme_transport_id trid = {};

	/* Invalid trtype */
	trid.trtype = 55;
	trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
	snprintf(trid.traddr, sizeof(trid.traddr), "192.168.100.1");
	snprintf(trid.trsvcid, sizeof(trid.trsvcid), "4420");
	CU_ASSERT(spdk_nvmf_tgt_listen(&trid) == NULL);

	trid.trtype = SPDK_NVME_TRANSPORT_RDMA;
	trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
	snprintf(trid.traddr, sizeof(trid.traddr), "192.168.100.1");
	snprintf(trid.trsvcid, sizeof(trid.trsvcid), "4420");
	listen_addr = spdk_nvmf_tgt_listen(&trid);
	SPDK_CU_ASSERT_FATAL(listen_addr != NULL);
	spdk_nvmf_listen_addr_destroy(listen_addr);

}

static void
test_spdk_nvmf_subsystem_add_ns(void)
{
	struct spdk_nvmf_subsystem subsystem = {
		.dev.max_nsid = 0,
		.dev.ns_list = {},
	};
	struct spdk_bdev bdev1 = {}, bdev2 = {};
	uint32_t nsid;

	/* Allow NSID to be assigned automatically */
	nsid = spdk_nvmf_subsystem_add_ns(&subsystem, &bdev1, 0);
	/* NSID 1 is the first unused ID */
	CU_ASSERT(nsid == 1);
	CU_ASSERT(subsystem.dev.max_nsid == 1);
	CU_ASSERT(subsystem.dev.ns_list[nsid - 1] == &bdev1);

	/* Request a specific NSID */
	nsid = spdk_nvmf_subsystem_add_ns(&subsystem, &bdev2, 5);
	CU_ASSERT(nsid == 5);
	CU_ASSERT(subsystem.dev.max_nsid == 5);
	CU_ASSERT(subsystem.dev.ns_list[nsid - 1] == &bdev2);

	/* Request an NSID that is already in use */
	nsid = spdk_nvmf_subsystem_add_ns(&subsystem, &bdev2, 5);
	CU_ASSERT(nsid == 0);
	CU_ASSERT(subsystem.dev.max_nsid == 5);
}

static void
nvmf_test_create_subsystem(void)
{
	char nqn[256];
	struct spdk_nvmf_subsystem *subsystem;
	TAILQ_INIT(&g_nvmf_tgt.subsystems);

	strncpy(nqn, "nqn.2016-06.io.spdk:subsystem1", sizeof(nqn));
	subsystem = spdk_nvmf_create_subsystem(nqn, SPDK_NVMF_SUBTYPE_NVME,
					       NULL, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);
	CU_ASSERT_STRING_EQUAL(subsystem->subnqn, nqn);
	spdk_nvmf_delete_subsystem(subsystem);

	/* Longest valid name */
	strncpy(nqn, "nqn.2016-06.io.spdk:", sizeof(nqn));
	memset(nqn + strlen(nqn), 'a', 223 - strlen(nqn));
	nqn[223] = '\0';
	CU_ASSERT(strlen(nqn) == 223);
	subsystem = spdk_nvmf_create_subsystem(nqn, SPDK_NVMF_SUBTYPE_NVME,
					       NULL, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);
	CU_ASSERT_STRING_EQUAL(subsystem->subnqn, nqn);
	spdk_nvmf_delete_subsystem(subsystem);

	/* Name that is one byte longer than allowed */
	strncpy(nqn, "nqn.2016-06.io.spdk:", sizeof(nqn));
	memset(nqn + strlen(nqn), 'a', 224 - strlen(nqn));
	nqn[224] = '\0';
	CU_ASSERT(strlen(nqn) == 224);
	subsystem = spdk_nvmf_create_subsystem(nqn, SPDK_NVMF_SUBTYPE_NVME,
					       NULL, NULL, NULL);
	CU_ASSERT(subsystem == NULL);
}

static void
nvmf_test_find_subsystem(void)
{
	CU_ASSERT_PTR_NULL(spdk_nvmf_find_subsystem(NULL));
	CU_ASSERT_PTR_NULL(spdk_nvmf_find_subsystem("fake"));
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("nvmf", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "create_subsystem", nvmf_test_create_subsystem) == NULL ||
		CU_add_test(suite, "nvmf_tgt_listen", test_spdk_nvmf_tgt_listen) == NULL ||
		CU_add_test(suite, "nvmf_subsystem_add_ns", test_spdk_nvmf_subsystem_add_ns) == NULL ||
		CU_add_test(suite, "find_subsystem", nvmf_test_find_subsystem) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
