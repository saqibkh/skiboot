/* Copyright 2013-2014 IBM Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define pr_fmt(fmt) "IPMI: " fmt
#include <skiboot.h>
#include <stdlib.h>
#include <string.h>
#include <ipmi.h>
#include <device.h>
#include <opal.h>
#include <lock.h>
#include <errorlog.h>
#include <pel.h>
#include <opal-msg.h>

/* OEM SEL fields */
#define SEL_OEM_ID_0		0x55
#define SEL_OEM_ID_1		0x55
#define SEL_RECORD_TYPE_OEM	0xC0
#define SEL_RECORD_TYPE_EVENT	0x02

#define SEL_NETFN_IBM		0x3a

/* OEM SEL Commands */
#define CMD_AMI_POWER		0x04
#define CMD_AMI_PNOR_ACCESS	0x07
#define CMD_AMI_OCC_RESET	0x0e

#define SOFT_OFF	        0x00
#define SOFT_REBOOT	        0x01

#define RELEASE_PNOR		0x00
#define REQUEST_PNOR		0x01

/* 32.1 SEL Event Records type */
#define SEL_REC_TYPE_SYS_EVENT	0x02
#define SEL_REC_TYPE_AMI_ESEL	0xDF

/* OEM SEL generator ID for AMI */
#define SEL_GENERATOR_ID_AMI	0x2000

/* IPMI SEL version */
#define SEL_EVM_VER_1		0x03
#define SEL_EVM_VER_2		0x04

/*
 * Sensor type for System events
 *
 * Sensor information (type, number, etc) is passed to us via
 * device tree. Currently we are using System Event type to
 * log OPAL events.
 */
#define SENSOR_TYPE_SYS_EVENT	0x12

/*
 * 42.1 Event/Reading Type Codes
 *
 * Note that device hotplug and availability related events
 * are not defined as we are not using those events type.
 */
#define SEL_EVENT_DIR_TYPE_UNSPECIFIED	0x00
#define SEL_EVENT_DIR_TYPE_THRESHOLD	0x01
#define SEL_EVENT_DIR_TYPE_STATE	0x03
#define SEL_EVENT_DIR_TYPE_PREDICTIVE	0x04
#define SEL_EVENT_DIR_TYPE_LIMIT	0x05
#define SEL_EVENT_DIR_TYPE_PERFORMANCE	0x06
#define SEL_EVENT_DIR_TYPE_TRANSITION	0x07
#define SEL_EVENT_DIR_TYPE_OEM		0x70

/*
 * 42.1 Event/Reading Type Codes
 */
#define SEL_DATA1_AMI			0xAA
#define SEL_DATA1_DEASSERTED		0x00
#define SEL_DATA1_ASSERTED		0x01
#define SEL_DATA1_OK			0x00
#define SEL_DATA1_NON_CRIT_FROM_OK	0x01
#define SEL_DATA1_CRIT_FROM_LESS_SEV	0x02
#define SEL_DATA1_NON_REC_FROM_LESS_SEV	0x03
#define SEL_DATA1_NON_CRIT		0x04
#define SEL_DATA1_CRITICAL		0x05
#define SEL_DATA1_NON_RECOVERABLE	0X06
#define SEL_DATA1_MONITOR		0x07
#define SEL_DATA1_INFORMATIONAL		0x08

/* SEL Record Entry */
struct sel_record {
	le16		record_id;
	uint8_t		record_type;
	le32		timestamp;
	le16		generator_id;
	uint8_t		evm_ver;
	uint8_t		sensor_type;
	uint8_t		sensor_number;
	uint8_t		event_dir_type;
	uint8_t		event_data1;
	uint8_t		event_data2;
	uint8_t		event_data3;
} __packed;

static struct sel_record sel_record;

struct oem_sel {
	/* SEL header */
	uint8_t id[2];
	uint8_t type;
	uint8_t manuf_id[3];
	uint8_t timestamp[4];
	/* OEM SEL data (6 bytes) follows */
	uint8_t netfun;
	uint8_t cmd;
	uint8_t data[4];
};

/* As far as I can tell the size of PEL record is unbounded (due to
 * the possible presence of the user defined section). We chose this
 * size because it's what hostboot also uses and most of the OPAL logs
 * are few hundred bytes.
 */
#define MAX_PEL_SIZE 0x800

#define ESEL_HDR_SIZE 7


/* Initialize eSEL record */
static void ipmi_init_esel_record(void)
{
	memset(&sel_record, 0, sizeof(struct sel_record));
	sel_record.record_type = SEL_REC_TYPE_AMI_ESEL;
	sel_record.generator_id = SEL_GENERATOR_ID_AMI;
	sel_record.evm_ver = SEL_EVM_VER_2;
	sel_record.sensor_type	= SENSOR_TYPE_SYS_EVENT;
	sel_record.sensor_number =
		ipmi_get_sensor_number(SENSOR_TYPE_SYS_EVENT);
	sel_record.event_dir_type = SEL_EVENT_DIR_TYPE_OEM;
	sel_record.event_data1 = SEL_DATA1_AMI;
}

static void ipmi_elog_error(struct ipmi_msg *msg)
{
	if (msg->cc == IPMI_LOST_ARBITRATION_ERR)
		/* Retry due to SEL erase */
		ipmi_queue_msg(msg);
	else {
		opal_elog_complete(msg->user_data, false);
		ipmi_free_msg(msg);
	}
}

/* Goes through the required steps to add a complete eSEL:
 *
 *  1. Get a reservation
 *  2. Add eSEL header
 *  3. Partially add data to the SEL
 *
 * Because a reservation is needed we need to ensure eSEL's are added
 * as a single transaction as concurrent/interleaved adds would cancel
 * the reservation. We guarantee this by always adding our messages to
 * the head of the transmission queue, blocking any other messages
 * being sent until we have completed sending this message.
 *
 * There is still a very small chance that we will accidentally
 * interleave a message if there is another one waiting at the head of
 * the ipmi queue and another cpu calls the ipmi poller before we
 * complete. However this should just cause a resevation cancelled
 * error which we have to deal with anyway (eg. because there may be a
 * SEL erase in progress) so it shouldn't cause any problems.
 */
static void ipmi_elog_poll(struct ipmi_msg *msg)
{
	static bool first = false;
	static char pel_buf[MAX_PEL_SIZE];
	static size_t pel_size;
	static size_t esel_size;
	static int esel_index = 0;
	int pel_index;
	static unsigned int reservation_id = 0;
	static unsigned int record_id = 0;
	struct errorlog *elog_buf = (struct errorlog *) msg->user_data;
	size_t req_size;

	ipmi_init_esel_record();

	if (msg->cmd == IPMI_CMD(IPMI_RESERVE_SEL)) {
		first = true;
		reservation_id = msg->data[0];
		reservation_id |= msg->data[1] << 8;
		if (!reservation_id) {
			/* According to specification we should never
			 * get here, but just in case we do we cancel
			 * sending the message. */
			prerror("Invalid reservation id");
			opal_elog_complete(elog_buf, false);
			ipmi_free_msg(msg);
			return;
		}

		pel_size = create_pel_log(elog_buf, pel_buf, MAX_PEL_SIZE);
		esel_size = pel_size + sizeof(struct sel_record);
		esel_index = 0;
		record_id = 0;
	} else {
		record_id = msg->data[0];
		record_id |= msg->data[1] << 8;
	}

	/* Start or continue the IPMI_PARTIAL_ADD_SEL */
	if (esel_index >= esel_size) {
		/* We're all done. Invalidate the resevation id to
		 * ensure we get an error if we cut in on another eSEL
		 * message. */
		reservation_id = 0;
		esel_index = 0;
		opal_elog_complete(elog_buf, true);
		ipmi_free_msg(msg);
		return;
	}

	if ((esel_size - esel_index) < (IPMI_MAX_REQ_SIZE - ESEL_HDR_SIZE)) {
		/* Last data to send */
		msg->data[6] = 1;
		req_size = esel_size - esel_index + ESEL_HDR_SIZE;
	} else {
		msg->data[6] = 0;
		req_size = IPMI_MAX_REQ_SIZE;
	}

	ipmi_init_msg(msg, IPMI_DEFAULT_INTERFACE, IPMI_PARTIAL_ADD_ESEL,
		      ipmi_elog_poll, elog_buf, req_size, 2);

	msg->data[0] = reservation_id & 0xff;
	msg->data[1] = (reservation_id >> 8) & 0xff;
	msg->data[2] = record_id & 0xff;
	msg->data[3] = (record_id >> 8) & 0xff;
	msg->data[4] = esel_index & 0xff;
	msg->data[5] = (esel_index >> 8) & 0xff;

	if (first) {
		first = false;
		memcpy(&msg->data[ESEL_HDR_SIZE],
		       &sel_record, sizeof(struct sel_record));
		esel_index = sizeof(struct sel_record);
		msg->req_size = esel_index + ESEL_HDR_SIZE;
	} else {
		pel_index = esel_index - sizeof(struct sel_record);
		memcpy(&msg->data[ESEL_HDR_SIZE],
		       &pel_buf[pel_index], msg->req_size - ESEL_HDR_SIZE);
		esel_index += msg->req_size - ESEL_HDR_SIZE;
	}

	ipmi_queue_msg_head(msg);
	return;
}

int ipmi_elog_commit(struct errorlog *elog_buf)
{
	struct ipmi_msg *msg;

	/* Only log events that needs attention */
	if (elog_buf->event_severity < OPAL_PREDICTIVE_ERR_FAULT_RECTIFY_REBOOT ||
	    elog_buf->elog_origin != ORG_SAPPHIRE) {
		prlog(PR_INFO, "dropping non severe PEL event\n");
		return 0;
	}

	/* We pass a large request size in to mkmsg so that we have a
	 * large enough allocation to reuse the message to pass the
	 * PEL data via a series of partial add commands.  */
	msg = ipmi_mkmsg(IPMI_DEFAULT_INTERFACE, IPMI_RESERVE_SEL, ipmi_elog_poll,
			 elog_buf, NULL, IPMI_MAX_REQ_SIZE, 2);
	if (!msg) {
		opal_elog_complete(elog_buf, false);
		return OPAL_RESOURCE;
	}
	msg->error = ipmi_elog_error;

	msg->req_size = 0;
	ipmi_queue_msg(msg);

	return 0;
}

#define ACCESS_DENIED	0x00
#define ACCESS_GRANTED	0x01

static void sel_pnor(uint8_t access)
{
	struct ipmi_msg *msg;
	uint8_t granted = ACCESS_GRANTED;

	switch (access) {
	case REQUEST_PNOR:
		prlog(PR_NOTICE, "PNOR access requested\n");
		granted = flash_reserve();
		if (granted)
			occ_pnor_set_owner(PNOR_OWNER_EXTERNAL);

		/* Ack the request */
		msg = ipmi_mkmsg_simple(IPMI_PNOR_ACCESS_STATUS, &granted, 1);
		ipmi_queue_msg(msg);
		break;
	case RELEASE_PNOR:
		prlog(PR_NOTICE, "PNOR access released\n");
		flash_release();
		occ_pnor_set_owner(PNOR_OWNER_HOST);
		break;
	default:
		prlog(PR_ERR, "invalid PNOR access requested: %02x\n",
		      access);
	}
}

static void sel_power(uint8_t power)
{
	switch (power) {
	case SOFT_OFF:
		prlog(PR_NOTICE, "soft shutdown requested\n");
		opal_queue_msg(OPAL_MSG_SHUTDOWN, NULL, NULL, SOFT_OFF);
		break;
	case SOFT_REBOOT:
		prlog(PR_NOTICE, "soft reboot rqeuested\n");
		opal_queue_msg(OPAL_MSG_SHUTDOWN, NULL, NULL, SOFT_REBOOT);
		break;
	default:
		prlog(PR_WARNING, "requested bad power state: %02x\n",
		      power);
	}
}

static uint32_t occ_sensor_id_to_chip(uint8_t sensor, uint32_t *chip)
{
	/* todo: lookup sensor ID node in the DT, and map to a chip id */
	(void)sensor;
	*chip = 0;
	return 0;
}

static void sel_occ_reset(uint8_t sensor)
{
	uint32_t chip;
	int rc;

	rc = occ_sensor_id_to_chip(sensor, &chip);
	if (rc) {
		prlog(PR_ERR, "SEL message to reset an unknown OCC "
				"(sensor ID 0x%02x)\n", sensor);
		return;
	}

	prd_occ_reset(chip);
}

void ipmi_parse_sel(struct ipmi_msg *msg)
{
	struct oem_sel sel;

	assert(msg->resp_size <= 16);

	memcpy(&sel, msg->data, msg->resp_size);

	/* We do not process system event records */
	if (sel.type == SEL_RECORD_TYPE_EVENT) {
		prlog(PR_INFO, "dropping System Event Record SEL\n");
		return;
	}

	prlog(PR_DEBUG, "SEL received (%d bytes, netfn %d, cmd %d)\n",
			msg->resp_size, sel.netfun, sel.cmd);

	/* Only accept OEM SEL messages */
	if (sel.id[0] != SEL_OEM_ID_0 ||
	    sel.id[1] != SEL_OEM_ID_1 ||
	    sel.type != SEL_RECORD_TYPE_OEM) {
		prlog(PR_WARNING, "unknown SEL %02x%02x (type %02x)\n",
		      sel.id[0], sel.id[1], sel.type);
		return;
	}

	switch (sel.cmd) {
	case CMD_AMI_POWER:
		sel_power(sel.data[0]);
		break;
	case CMD_AMI_OCC_RESET:
		sel_occ_reset(sel.data[0]);
		break;
	case CMD_AMI_PNOR_ACCESS:
		sel_pnor(sel.data[0]);
		break;
	default:
		prlog(PR_WARNING,
		      "unknown OEM SEL command %02x received\n",
		      sel.cmd);
	}
}
