/*-
 * Copyright (c) 2026, Samsung Electronics Co., Ltd.
 * Written by Jaeyoon Choi
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/conf.h>
#include <sys/ioccom.h>
#include <sys/malloc.h>
#include <sys/systm.h>

#include "ufshci_private.h"
#include "ufshci_ioctl.h"
#include "ufshci_reg.h"

/* UFS 4.1, section 10.6.2: the EHS length counts 32 byte units. */
#define UFSHCI_EHS_UNIT_SIZE 32

static d_ioctl_t ufshci_ctrlr_ioctl;

static struct cdevsw ufshci_ctrlr_cdevsw = {
	.d_version = D_VERSION,
	.d_ioctl = ufshci_ctrlr_ioctl,
	.d_name = "ufshci",
};

/*
 * Work out how much of the UPIU the controller has to read and how much
 * it may write back. A query request carries its data segment inside the
 * UPIU, so its size covers that segment already.
 */
static int
ufshci_ctrlr_passthrough_upiu_sizes(const struct ufshci_pt_command *pt,
    size_t *req_size, size_t *resp_size, bool *is_admin)
{
	switch (pt->req_upiu.header.trans_code) {
	case UFSHCI_UPIU_TRANSACTION_CODE_QUERY_REQUEST:
		/* Only a command UPIU has room for an EHS. */
		if (pt->req_upiu.header.ehs_length != 0)
			return (EINVAL);
		*req_size = sizeof(struct ufshci_query_request_upiu);
		*resp_size = sizeof(struct ufshci_query_response_upiu);
		*is_admin = true;
		return (0);
	case UFSHCI_UPIU_TRANSACTION_CODE_NOP_OUT:
		if (pt->req_upiu.header.ehs_length != 0)
			return (EINVAL);
		*req_size = sizeof(struct ufshci_nop_out_upiu);
		*resp_size = sizeof(struct ufshci_nop_in_upiu);
		*is_admin = true;
		return (0);
	case UFSHCI_UPIU_TRANSACTION_CODE_COMMAND: {
		/*
		 * The EHS follows the command UPIU. UFS 4.1 section 10.6.2
		 * counts the total EHS length in units of 32 bytes, not in
		 * dwords.
		 */
		size_t ehs_bytes = pt->req_upiu.header.ehs_length *
		    UFSHCI_EHS_UNIT_SIZE;

		*req_size = sizeof(struct ufshci_cmd_command_upiu) + ehs_bytes;
		/*
		 * The device answers with an EHS of its own, ahead of the
		 * sense data. Advanced RPMB carries its MAC there, so the
		 * response has to be sized for it too.
		 */
		*resp_size = sizeof(struct ufshci_cmd_response_upiu) +
		    ehs_bytes;
		*is_admin = false;
		return (0);
	}
	default:
		return (EINVAL);
	}
}

static int
ufshci_ctrlr_passthrough_cmd(struct ufshci_controller *ctrlr,
    struct ufshci_pt_command *pt)
{
	struct ufshci_completion_poll_status status;
	struct ufshci_request *req;
	size_t req_size, resp_size;
	void *buf = NULL;
	bool is_admin;
	int error;

	/*
	 * Zero the whole status. A request that is aborted or reset away is
	 * completed by hand with a zero cpl.size, and the poll callback
	 * copies that many bytes, so response_upiu would keep whatever the
	 * stack held and go out to userland.
	 */
	memset(&status, 0, sizeof(status));

	/*
	 * There is no per request timeout to honor yet. Refuse a value
	 * rather than accept one and ignore it.
	 */
	if (pt->timeout_ms != 0)
		return (EINVAL);

	/*
	 * Clear the response up front. The completion holds less than a
	 * whole UPIU, so the copy below leaves a tail, and that tail is the
	 * kernel's copy of what the caller passed in.
	 */
	memset(&pt->resp_upiu, 0, sizeof(pt->resp_upiu));

	/*
	 * UFSHCI_PT_MAX_XFER is what the ABI promises. What a controller can
	 * actually map comes from its page size, so check that too.
	 */
	if (pt->len > UFSHCI_PT_MAX_XFER || pt->len > ctrlr->max_xfer_size)
		return (EINVAL);
	if (pt->len != 0 && pt->buf == NULL)
		return (EINVAL);

	/* A buffer with no direction would build a PRDT nothing reads. */
	if (pt->len != 0 && (pt->flags &
	    (UFSHCI_PT_FLAG_DATA_IN | UFSHCI_PT_FLAG_DATA_OUT)) == 0)
		return (EINVAL);

	/* A request moves data one way or the other, never both. */
	if ((pt->flags & (UFSHCI_PT_FLAG_DATA_IN | UFSHCI_PT_FLAG_DATA_OUT)) ==
	    (UFSHCI_PT_FLAG_DATA_IN | UFSHCI_PT_FLAG_DATA_OUT))
		return (EINVAL);

	error = ufshci_ctrlr_passthrough_upiu_sizes(pt, &req_size, &resp_size,
	    &is_admin);
	if (error)
		return (error);
	if (req_size > sizeof(struct ufshci_upiu))
		return (EINVAL);
	/*
	 * The completion carries the response in a union that is smaller
	 * than the hardware buffer, and the queue code copies response_size
	 * bytes into it. An EHS length the caller chose must not push the
	 * response past that union.
	 */
	if (resp_size > sizeof(status.cpl.response_upiu))
		return (EINVAL);
	/*
	 * Only the transfer request descriptor tells the controller how long
	 * the EHS is. Without that field it sends the command UPIU alone, and
	 * the device answers a request it never saw the rest of. Refuse
	 * rather than let that read as success.
	 */
	if (pt->req_upiu.header.ehs_length != 0 &&
	    UFSHCIV(UFSHCI_CAP_REG_EHSLUTRDS, ctrlr->cap) == 0)
		return (EOPNOTSUPP);

	if (pt->len != 0) {
		buf = malloc(pt->len, M_UFSHCI, M_WAITOK | M_ZERO);
		if (pt->flags & UFSHCI_PT_FLAG_DATA_OUT) {
			error = copyin(pt->buf, buf, pt->len);
			if (error)
				goto out;
		}
	}

	req = ufshci_allocate_request_vaddr(buf, pt->len, M_WAITOK,
	    ufshci_completion_poll_cb, &status);

	memcpy(&req->request_upiu, &pt->req_upiu, sizeof(req->request_upiu));
	req->request_size = req_size;
	req->response_size = resp_size;
	req->is_admin = is_admin;

	if (pt->flags & UFSHCI_PT_FLAG_DATA_OUT)
		req->data_direction = UFSHCI_DATA_DIRECTION_FROM_SYS_TO_TGT;
	else if (pt->flags & UFSHCI_PT_FLAG_DATA_IN)
		req->data_direction = UFSHCI_DATA_DIRECTION_FROM_TGT_TO_SYS;
	else
		req->data_direction = UFSHCI_DATA_DIRECTION_NO_DATA_TRANSFER;

	error = ufshci_ctrlr_submit_transfer_request(ctrlr, req);
	if (error) {
		ufshci_free_request(req);
		goto out;
	}

	ufshci_completion_poll(&status);

	memcpy(&pt->resp_upiu, &status.cpl.response_upiu,
	    min(sizeof(pt->resp_upiu), sizeof(status.cpl.response_upiu)));
	pt->xfer_len = pt->len;
	/* ocs is reserved. Produce the zero the ABI promises. */
	pt->ocs = 0;

	/*
	 * A device that refuses a command still answers, and the reason is
	 * in the response field. Report that as a success: the ioctl layer
	 * copies output back only when we return zero, so an error here
	 * would throw the answer away. The driver fills the same field in
	 * for a request it aborted itself, so that case comes back this way
	 * too.
	 *
	 * The controller can also flag a transfer bad while the response
	 * reads clean, or before any response was written. A caller would
	 * take the zeroes for a clean result. Report that as an error.
	 */
	if (status.error && pt->resp_upiu.header.response == 0) {
		error = EIO;
		goto out;
	}

	if (pt->len != 0 && (pt->flags & UFSHCI_PT_FLAG_DATA_IN))
		error = copyout(buf, pt->buf, pt->len);

out:
	free(buf, M_UFSHCI);
	return (error);
}

static int
ufshci_ctrlr_passthrough_uic_cmd(struct ufshci_controller *ctrlr,
    struct ufshci_pt_uic_command *pt)
{
	uint32_t return_value = 0;
	int error;

	if (pt->timeout_ms != 0)
		return (EINVAL);

	/*
	 * Only the four attribute commands. The rest can drop the link or
	 * power the device off, and nothing in userland needs them.
	 */
	switch (pt->cmd.opcode) {
	case UFSHCI_DME_GET:
	case UFSHCI_DME_SET:
	case UFSHCI_DME_PEER_GET:
	case UFSHCI_DME_PEER_SET:
		break;
	default:
		return (EINVAL);
	}

	/*
	 * The device reports its result in the low byte of argument2, and
	 * the caller owns the rest of that word. On a path that never
	 * reaches the register read, a value left there by the caller would
	 * read back as a device answer, so clear the field first.
	 */
	pt->cmd.argument2 &= ~(UFSHCI_UICCMDARG2_REG_ERROR_CODE_MASK <<
	    UFSHCI_UICCMDARG2_REG_ERROR_CODE_SHIFT);

	error = ufshci_uic_send_cmd(ctrlr, &pt->cmd, &return_value);

	pt->result = UFSHCIV(UFSHCI_UICCMDARG2_REG_ERROR_CODE,
	    pt->cmd.argument2);
	if (pt->result != 0) {
		/*
		 * The device answered and refused. That is a result, not a
		 * transport failure, so hand the code to the caller. There is
		 * no attribute value to go with it, and leaving the caller's
		 * own input in argument3 would read like one.
		 */
		pt->cmd.argument3 = 0;
		return (0);
	}
	if (error)
		return (error);

	pt->cmd.argument3 = return_value;

	return (0);
}

static int
ufshci_ctrlr_ioctl(struct cdev *cdev, u_long cmd, caddr_t arg, int flag,
    struct thread *td)
{
	struct ufshci_controller *ctrlr = cdev->si_drv1;

	switch (cmd) {
	case UFSHCI_PASSTHROUGH_CMD:
		return (ufshci_ctrlr_passthrough_cmd(ctrlr,
		    (struct ufshci_pt_command *)arg));
	case UFSHCI_PASSTHROUGH_UIC:
		return (ufshci_ctrlr_passthrough_uic_cmd(ctrlr,
		    (struct ufshci_pt_uic_command *)arg));
	default:
		return (ENOTTY);
	}
}

int
ufshci_ctrlr_ioctl_construct(struct ufshci_controller *ctrlr, device_t dev)
{
	struct make_dev_args md_args;
	int error;

	/*
	 * Set si_drv1 as the node is created. A separate assignment after
	 * make_dev leaves a window where an open that races the attach finds
	 * it unset.
	 */
	make_dev_args_init(&md_args);
	md_args.mda_devsw = &ufshci_ctrlr_cdevsw;
	md_args.mda_uid = UID_ROOT;
	md_args.mda_gid = GID_WHEEL;
	md_args.mda_mode = 0600;
	md_args.mda_unit = device_get_unit(dev);
	md_args.mda_si_drv1 = ctrlr;

	error = make_dev_s(&md_args, &ctrlr->cdev, "ufshci%d",
	    device_get_unit(dev));
	if (error != 0)
		return (error);

	return (0);
}

void
ufshci_ctrlr_ioctl_destruct(struct ufshci_controller *ctrlr)
{
	if (ctrlr->cdev != NULL) {
		destroy_dev(ctrlr->cdev);
		ctrlr->cdev = NULL;
	}
}
