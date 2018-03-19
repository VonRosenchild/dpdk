/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright(c) 2018 Advanced Micro Devices, Inc. All rights reserved.
 */

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <unistd.h>

#include <rte_hexdump.h>
#include <rte_memzone.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_spinlock.h>
#include <rte_string_fns.h>
#include <rte_cryptodev_pmd.h>

#include "ccp_dev.h"
#include "ccp_crypto.h"
#include "ccp_pci.h"
#include "ccp_pmd_private.h"

static enum ccp_cmd_order
ccp_get_cmd_id(const struct rte_crypto_sym_xform *xform)
{
	enum ccp_cmd_order res = CCP_CMD_NOT_SUPPORTED;

	if (xform == NULL)
		return res;
	if (xform->type == RTE_CRYPTO_SYM_XFORM_AUTH) {
		if (xform->next == NULL)
			return CCP_CMD_AUTH;
		else if (xform->next->type == RTE_CRYPTO_SYM_XFORM_CIPHER)
			return CCP_CMD_HASH_CIPHER;
	}
	if (xform->type == RTE_CRYPTO_SYM_XFORM_CIPHER) {
		if (xform->next == NULL)
			return CCP_CMD_CIPHER;
		else if (xform->next->type == RTE_CRYPTO_SYM_XFORM_AUTH)
			return CCP_CMD_CIPHER_HASH;
	}
	if (xform->type == RTE_CRYPTO_SYM_XFORM_AEAD)
		return CCP_CMD_COMBINED;
	return res;
}

/* configure session */
static int
ccp_configure_session_cipher(struct ccp_session *sess,
			     const struct rte_crypto_sym_xform *xform)
{
	const struct rte_crypto_cipher_xform *cipher_xform = NULL;

	cipher_xform = &xform->cipher;

	/* set cipher direction */
	if (cipher_xform->op ==  RTE_CRYPTO_CIPHER_OP_ENCRYPT)
		sess->cipher.dir = CCP_CIPHER_DIR_ENCRYPT;
	else
		sess->cipher.dir = CCP_CIPHER_DIR_DECRYPT;

	/* set cipher key */
	sess->cipher.key_length = cipher_xform->key.length;
	rte_memcpy(sess->cipher.key, cipher_xform->key.data,
		   cipher_xform->key.length);

	/* set iv parameters */
	sess->iv.offset = cipher_xform->iv.offset;
	sess->iv.length = cipher_xform->iv.length;

	switch (cipher_xform->algo) {
	default:
		CCP_LOG_ERR("Unsupported cipher algo");
		return -1;
	}


	switch (sess->cipher.engine) {
	default:
		CCP_LOG_ERR("Invalid CCP Engine");
		return -ENOTSUP;
	}
	return 0;
}

static int
ccp_configure_session_auth(struct ccp_session *sess,
			   const struct rte_crypto_sym_xform *xform)
{
	const struct rte_crypto_auth_xform *auth_xform = NULL;

	auth_xform = &xform->auth;

	sess->auth.digest_length = auth_xform->digest_length;
	if (auth_xform->op ==  RTE_CRYPTO_AUTH_OP_GENERATE)
		sess->auth.op = CCP_AUTH_OP_GENERATE;
	else
		sess->auth.op = CCP_AUTH_OP_VERIFY;
	switch (auth_xform->algo) {
	default:
		CCP_LOG_ERR("Unsupported hash algo");
		return -ENOTSUP;
	}
	return 0;
}

static int
ccp_configure_session_aead(struct ccp_session *sess,
			   const struct rte_crypto_sym_xform *xform)
{
	const struct rte_crypto_aead_xform *aead_xform = NULL;

	aead_xform = &xform->aead;

	sess->cipher.key_length = aead_xform->key.length;
	rte_memcpy(sess->cipher.key, aead_xform->key.data,
		   aead_xform->key.length);

	if (aead_xform->op == RTE_CRYPTO_AEAD_OP_ENCRYPT) {
		sess->cipher.dir = CCP_CIPHER_DIR_ENCRYPT;
		sess->auth.op = CCP_AUTH_OP_GENERATE;
	} else {
		sess->cipher.dir = CCP_CIPHER_DIR_DECRYPT;
		sess->auth.op = CCP_AUTH_OP_VERIFY;
	}
	sess->auth.aad_length = aead_xform->aad_length;
	sess->auth.digest_length = aead_xform->digest_length;

	/* set iv parameters */
	sess->iv.offset = aead_xform->iv.offset;
	sess->iv.length = aead_xform->iv.length;

	switch (aead_xform->algo) {
	default:
		CCP_LOG_ERR("Unsupported aead algo");
		return -ENOTSUP;
	}
	return 0;
}

int
ccp_set_session_parameters(struct ccp_session *sess,
			   const struct rte_crypto_sym_xform *xform)
{
	const struct rte_crypto_sym_xform *cipher_xform = NULL;
	const struct rte_crypto_sym_xform *auth_xform = NULL;
	const struct rte_crypto_sym_xform *aead_xform = NULL;
	int ret = 0;

	sess->cmd_id = ccp_get_cmd_id(xform);

	switch (sess->cmd_id) {
	case CCP_CMD_CIPHER:
		cipher_xform = xform;
		break;
	case CCP_CMD_AUTH:
		auth_xform = xform;
		break;
	case CCP_CMD_CIPHER_HASH:
		cipher_xform = xform;
		auth_xform = xform->next;
		break;
	case CCP_CMD_HASH_CIPHER:
		auth_xform = xform;
		cipher_xform = xform->next;
		break;
	case CCP_CMD_COMBINED:
		aead_xform = xform;
		break;
	default:
		CCP_LOG_ERR("Unsupported cmd_id");
		return -1;
	}

	/* Default IV length = 0 */
	sess->iv.length = 0;
	if (cipher_xform) {
		ret = ccp_configure_session_cipher(sess, cipher_xform);
		if (ret != 0) {
			CCP_LOG_ERR("Invalid/unsupported cipher parameters");
			return ret;
		}
	}
	if (auth_xform) {
		ret = ccp_configure_session_auth(sess, auth_xform);
		if (ret != 0) {
			CCP_LOG_ERR("Invalid/unsupported auth parameters");
			return ret;
		}
	}
	if (aead_xform) {
		ret = ccp_configure_session_aead(sess, aead_xform);
		if (ret != 0) {
			CCP_LOG_ERR("Invalid/unsupported aead parameters");
			return ret;
		}
	}
	return ret;
}

/* calculate CCP descriptors requirement */
static inline int
ccp_cipher_slot(struct ccp_session *session)
{
	int count = 0;

	switch (session->cipher.algo) {
	default:
		CCP_LOG_ERR("Unsupported cipher algo %d",
			    session->cipher.algo);
	}
	return count;
}

static inline int
ccp_auth_slot(struct ccp_session *session)
{
	int count = 0;

	switch (session->auth.algo) {
	default:
		CCP_LOG_ERR("Unsupported auth algo %d",
			    session->auth.algo);
	}

	return count;
}

static int
ccp_aead_slot(struct ccp_session *session)
{
	int count = 0;

	switch (session->aead_algo) {
	default:
		CCP_LOG_ERR("Unsupported aead algo %d",
			    session->aead_algo);
	}
	return count;
}

int
ccp_compute_slot_count(struct ccp_session *session)
{
	int count = 0;

	switch (session->cmd_id) {
	case CCP_CMD_CIPHER:
		count = ccp_cipher_slot(session);
		break;
	case CCP_CMD_AUTH:
		count = ccp_auth_slot(session);
		break;
	case CCP_CMD_CIPHER_HASH:
	case CCP_CMD_HASH_CIPHER:
		count = ccp_cipher_slot(session);
		count += ccp_auth_slot(session);
		break;
	case CCP_CMD_COMBINED:
		count = ccp_aead_slot(session);
		break;
	default:
		CCP_LOG_ERR("Unsupported cmd_id");

	}

	return count;
}

static inline int
ccp_crypto_cipher(struct rte_crypto_op *op,
		  struct ccp_queue *cmd_q __rte_unused,
		  struct ccp_batch_info *b_info __rte_unused)
{
	int result = 0;
	struct ccp_session *session;

	session = (struct ccp_session *)get_session_private_data(
					 op->sym->session,
					 ccp_cryptodev_driver_id);

	switch (session->cipher.algo) {
	default:
		CCP_LOG_ERR("Unsupported cipher algo %d",
			    session->cipher.algo);
		return -ENOTSUP;
	}
	return result;
}

static inline int
ccp_crypto_auth(struct rte_crypto_op *op,
		struct ccp_queue *cmd_q __rte_unused,
		struct ccp_batch_info *b_info __rte_unused)
{

	int result = 0;
	struct ccp_session *session;

	session = (struct ccp_session *)get_session_private_data(
					 op->sym->session,
					ccp_cryptodev_driver_id);

	switch (session->auth.algo) {
	default:
		CCP_LOG_ERR("Unsupported auth algo %d",
			    session->auth.algo);
		return -ENOTSUP;
	}

	return result;
}

static inline int
ccp_crypto_aead(struct rte_crypto_op *op,
		struct ccp_queue *cmd_q __rte_unused,
		struct ccp_batch_info *b_info __rte_unused)
{
	int result = 0;
	struct ccp_session *session;

	session = (struct ccp_session *)get_session_private_data(
					 op->sym->session,
					ccp_cryptodev_driver_id);

	switch (session->aead_algo) {
	default:
		CCP_LOG_ERR("Unsupported aead algo %d",
			    session->aead_algo);
		return -ENOTSUP;
	}
	return result;
}

int
process_ops_to_enqueue(const struct ccp_qp *qp,
		       struct rte_crypto_op **op,
		       struct ccp_queue *cmd_q,
		       uint16_t nb_ops,
		       int slots_req)
{
	int i, result = 0;
	struct ccp_batch_info *b_info;
	struct ccp_session *session;

	if (rte_mempool_get(qp->batch_mp, (void **)&b_info)) {
		CCP_LOG_ERR("batch info allocation failed");
		return 0;
	}
	/* populate batch info necessary for dequeue */
	b_info->op_idx = 0;
	b_info->lsb_buf_idx = 0;
	b_info->desccnt = 0;
	b_info->cmd_q = cmd_q;
	b_info->lsb_buf_phys =
		(phys_addr_t)rte_mem_virt2phy((void *)b_info->lsb_buf);
	rte_atomic64_sub(&b_info->cmd_q->free_slots, slots_req);

	b_info->head_offset = (uint32_t)(cmd_q->qbase_phys_addr + cmd_q->qidx *
					 Q_DESC_SIZE);
	for (i = 0; i < nb_ops; i++) {
		session = (struct ccp_session *)get_session_private_data(
						 op[i]->sym->session,
						 ccp_cryptodev_driver_id);
		switch (session->cmd_id) {
		case CCP_CMD_CIPHER:
			result = ccp_crypto_cipher(op[i], cmd_q, b_info);
			break;
		case CCP_CMD_AUTH:
			result = ccp_crypto_auth(op[i], cmd_q, b_info);
			break;
		case CCP_CMD_CIPHER_HASH:
			result = ccp_crypto_cipher(op[i], cmd_q, b_info);
			if (result)
				break;
			result = ccp_crypto_auth(op[i], cmd_q, b_info);
			break;
		case CCP_CMD_HASH_CIPHER:
			result = ccp_crypto_auth(op[i], cmd_q, b_info);
			if (result)
				break;
			result = ccp_crypto_cipher(op[i], cmd_q, b_info);
			break;
		case CCP_CMD_COMBINED:
			result = ccp_crypto_aead(op[i], cmd_q, b_info);
			break;
		default:
			CCP_LOG_ERR("Unsupported cmd_id");
			result = -1;
		}
		if (unlikely(result < 0)) {
			rte_atomic64_add(&b_info->cmd_q->free_slots,
					 (slots_req - b_info->desccnt));
			break;
		}
		b_info->op[i] = op[i];
	}

	b_info->opcnt = i;
	b_info->tail_offset = (uint32_t)(cmd_q->qbase_phys_addr + cmd_q->qidx *
					 Q_DESC_SIZE);

	rte_wmb();
	/* Write the new tail address back to the queue register */
	CCP_WRITE_REG(cmd_q->reg_base, CMD_Q_TAIL_LO_BASE,
			      b_info->tail_offset);
	/* Turn the queue back on using our cached control register */
	CCP_WRITE_REG(cmd_q->reg_base, CMD_Q_CONTROL_BASE,
			      cmd_q->qcontrol | CMD_Q_RUN);

	rte_ring_enqueue(qp->processed_pkts, (void *)b_info);

	return i;
}

static inline void ccp_auth_dq_prepare(struct rte_crypto_op *op)
{
	struct ccp_session *session;
	uint8_t *digest_data, *addr;
	struct rte_mbuf *m_last;
	int offset, digest_offset;
	uint8_t digest_le[64];

	session = (struct ccp_session *)get_session_private_data(
					 op->sym->session,
					ccp_cryptodev_driver_id);

	if (session->cmd_id == CCP_CMD_COMBINED) {
		digest_data = op->sym->aead.digest.data;
		digest_offset = op->sym->aead.data.offset +
					op->sym->aead.data.length;
	} else {
		digest_data = op->sym->auth.digest.data;
		digest_offset = op->sym->auth.data.offset +
					op->sym->auth.data.length;
	}
	m_last = rte_pktmbuf_lastseg(op->sym->m_src);
	addr = (uint8_t *)((char *)m_last->buf_addr + m_last->data_off +
			   m_last->data_len - session->auth.ctx_len);

	rte_mb();
	offset = session->auth.offset;

	if (session->auth.engine == CCP_ENGINE_SHA)
		if ((session->auth.ut.sha_type != CCP_SHA_TYPE_1) &&
		    (session->auth.ut.sha_type != CCP_SHA_TYPE_224) &&
		    (session->auth.ut.sha_type != CCP_SHA_TYPE_256)) {
			/* All other algorithms require byte
			 * swap done by host
			 */
			unsigned int i;

			offset = session->auth.ctx_len -
				session->auth.offset - 1;
			for (i = 0; i < session->auth.digest_length; i++)
				digest_le[i] = addr[offset - i];
			offset = 0;
			addr = digest_le;
		}

	op->status = RTE_CRYPTO_OP_STATUS_SUCCESS;
	if (session->auth.op == CCP_AUTH_OP_VERIFY) {
		if (memcmp(addr + offset, digest_data,
			   session->auth.digest_length) != 0)
			op->status = RTE_CRYPTO_OP_STATUS_AUTH_FAILED;

	} else {
		if (unlikely(digest_data == 0))
			digest_data = rte_pktmbuf_mtod_offset(
					op->sym->m_dst, uint8_t *,
					digest_offset);
		rte_memcpy(digest_data, addr + offset,
			   session->auth.digest_length);
	}
	/* Trim area used for digest from mbuf. */
	rte_pktmbuf_trim(op->sym->m_src,
			 session->auth.ctx_len);
}

static int
ccp_prepare_ops(struct rte_crypto_op **op_d,
		struct ccp_batch_info *b_info,
		uint16_t nb_ops)
{
	int i, min_ops;
	struct ccp_session *session;

	min_ops = RTE_MIN(nb_ops, b_info->opcnt);

	for (i = 0; i < min_ops; i++) {
		op_d[i] = b_info->op[b_info->op_idx++];
		session = (struct ccp_session *)get_session_private_data(
						 op_d[i]->sym->session,
						ccp_cryptodev_driver_id);
		switch (session->cmd_id) {
		case CCP_CMD_CIPHER:
			op_d[i]->status = RTE_CRYPTO_OP_STATUS_SUCCESS;
			break;
		case CCP_CMD_AUTH:
		case CCP_CMD_CIPHER_HASH:
		case CCP_CMD_HASH_CIPHER:
		case CCP_CMD_COMBINED:
			ccp_auth_dq_prepare(op_d[i]);
			break;
		default:
			CCP_LOG_ERR("Unsupported cmd_id");
		}
	}

	b_info->opcnt -= min_ops;
	return min_ops;
}

int
process_ops_to_dequeue(struct ccp_qp *qp,
		       struct rte_crypto_op **op,
		       uint16_t nb_ops)
{
	struct ccp_batch_info *b_info;
	uint32_t cur_head_offset;

	if (qp->b_info != NULL) {
		b_info = qp->b_info;
		if (unlikely(b_info->op_idx > 0))
			goto success;
	} else if (rte_ring_dequeue(qp->processed_pkts,
				    (void **)&b_info))
		return 0;
	cur_head_offset = CCP_READ_REG(b_info->cmd_q->reg_base,
				       CMD_Q_HEAD_LO_BASE);

	if (b_info->head_offset < b_info->tail_offset) {
		if ((cur_head_offset >= b_info->head_offset) &&
		    (cur_head_offset < b_info->tail_offset)) {
			qp->b_info = b_info;
			return 0;
		}
	} else {
		if ((cur_head_offset >= b_info->head_offset) ||
		    (cur_head_offset < b_info->tail_offset)) {
			qp->b_info = b_info;
			return 0;
		}
	}


success:
	nb_ops = ccp_prepare_ops(op, b_info, nb_ops);
	rte_atomic64_add(&b_info->cmd_q->free_slots, b_info->desccnt);
	b_info->desccnt = 0;
	if (b_info->opcnt > 0) {
		qp->b_info = b_info;
	} else {
		rte_mempool_put(qp->batch_mp, (void *)b_info);
		qp->b_info = NULL;
	}

	return nb_ops;
}