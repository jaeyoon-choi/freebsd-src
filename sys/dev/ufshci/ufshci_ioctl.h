/*-
 * Copyright (c) 2026, Samsung Electronics Co., Ltd.
 * Written by Jaeyoon Choi
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef __UFSHCI_IOCTL_H__
#define __UFSHCI_IOCTL_H__

#include <sys/ioccom.h>

#include <dev/ufshci/ufshci.h>

#define UFSHCI_PT_FLAG_DATA_IN	0x01 /* device -> host */
#define UFSHCI_PT_FLAG_DATA_OUT	0x02 /* host -> device */

/* Largest data buffer a single passthrough request may carry. */
#define UFSHCI_PT_MAX_XFER	(1024 * 1024)

/*
 * req_upiu and resp_upiu carry the UPIU whole, including its EHS and its
 * data segment. A query request keeps its descriptor bytes in that data
 * segment, so such a request leaves buf NULL and len zero.
 *
 * buf is the separate buffer a command UPIU moves through the PRDT. Only
 * a command UPIU uses it.
 */
struct ufshci_pt_command {
	struct ufshci_upiu req_upiu;  /* [in] */
	struct ufshci_upiu resp_upiu; /* [out] */
	void *buf;		      /* [in] PRDT payload, may be NULL */
	uint32_t len;		      /* [in] length of buf */
	uint32_t flags;		      /* [in] UFSHCI_PT_FLAG_* */
	uint32_t timeout_ms;	      /* [in] 0 means the driver default */
	uint32_t xfer_len;	      /* [out] bytes actually moved */
	uint8_t ocs;		      /* [out] overall command status */
	uint8_t reserved[7];
};

struct ufshci_pt_uic_command {
	struct ufshci_uic_cmd cmd; /* [in/out] opcode and argument1..3 */
	uint32_t timeout_ms;	   /* [in] */
	uint32_t result;	   /* [out] UICCMDARG2 result code */
};

/*
 * Command group 'u'. ttycom.h claims _IO('u', n) for UIOCCMD, but that
 * encoding carries IOC_VOID and a zero length, so it cannot collide with
 * these.
 */
#define UFSHCI_PASSTHROUGH_CMD	_IOWR('u', 0, struct ufshci_pt_command)
#define UFSHCI_PASSTHROUGH_UIC	_IOWR('u', 1, struct ufshci_pt_uic_command)

#endif /* __UFSHCI_IOCTL_H__ */
