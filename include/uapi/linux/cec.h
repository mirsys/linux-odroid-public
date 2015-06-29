#ifndef _CEC_H
#define _CEC_H

#include <linux/types.h>

struct cec_msg {
	__u64 ts;
	__u32 len;
	__u32 status;
	/*
	 * timeout (in ms) is used to timeout CEC_RECEIVE.
	 * Set to 0 if you want to wait forever.
	 */
	__u32 timeout;
	/*
	 * The framework assigns a sequence number to messages that are sent.
	 * This can be used to track replies to previously sent messages.
	 */
	__u32 sequence;
	__u8  msg[16];
	/*
	 * If non-zero, then wait for a reply with this opcode.
	 * If there was an error when sending the msg or FeatureAbort
	 * was returned, then reply is set to 0.
	 * If reply is non-zero upon return, then len/msg are set to
	 * the received message.
	 * If reply is zero upon return and status has the
	 * CEC_TX_STATUS_FEATURE_ABORT bit set, then len/msg are set to the
	 * received feature abort message.
	 * If reply is zero upon return and status has the
	 * CEC_TX_STATUS_REPLY_TIMEOUT
	 * bit set, then no reply was seen at all.
	 * This field is ignored with CEC_RECEIVE.
	 * If reply is non-zero for CEC_TRANSMIT and the message is a broadcast,
	 * then -EINVAL is returned.
	 * if reply is non-zero, then timeout is set to 1000 (the required
	 * maximum response time).
	 */
	__u8  reply;
	__u8 reserved[35];
};

static inline __u8 cec_msg_initiator(const struct cec_msg *msg)
{
	return msg->msg[0] >> 4;
}

static inline __u8 cec_msg_destination(const struct cec_msg *msg)
{
	return msg->msg[0] & 0xf;
}

static inline bool cec_msg_is_broadcast(const struct cec_msg *msg)
{
	return (msg->msg[0] & 0xf) == 0xf;
}

static inline void cec_msg_init(struct cec_msg *msg,
				__u8 initiator, __u8 destination)
{
	memset(msg, 0, sizeof(*msg));
	msg->msg[0] = (initiator << 4) | destination;
}

/*
 * Set the msg destination to the orig initiator and the msg initiator to the
 * orig destination. Note that msg and orig may be the same pointer, in which
 * case the change is done in place.
 */
static inline void cec_msg_set_reply_to(struct cec_msg *msg, struct cec_msg *orig)
{
	/* The destination becomes the initiator and vice versa */
	msg->msg[0] = (cec_msg_destination(orig) << 4) | cec_msg_initiator(orig);
}

/* cec status field */
#define CEC_TX_STATUS_OK            (0)
#define CEC_TX_STATUS_ARB_LOST      (1 << 0)
#define CEC_TX_STATUS_RETRY_TIMEOUT (1 << 1)
#define CEC_TX_STATUS_FEATURE_ABORT (1 << 2)
#define CEC_TX_STATUS_REPLY_TIMEOUT (1 << 3)
#define CEC_RX_STATUS_READY         (0)

#define CEC_LOG_ADDR_INVALID 0xff

/* The maximum number of logical addresses one device can be assigned to.
 * The CEC 2.0 spec allows for only 2 logical addresses at the moment. The
 * Analog Devices CEC hardware supports 3. So let's go wild and go for 4. */
#define CEC_MAX_LOG_ADDRS 4

/* The logical address types that the CEC device wants to claim */
#define CEC_LOG_ADDR_TYPE_TV		0
#define CEC_LOG_ADDR_TYPE_RECORD	1
#define CEC_LOG_ADDR_TYPE_TUNER		2
#define CEC_LOG_ADDR_TYPE_PLAYBACK	3
#define CEC_LOG_ADDR_TYPE_AUDIOSYSTEM	4
#define CEC_LOG_ADDR_TYPE_SPECIFIC	5
#define CEC_LOG_ADDR_TYPE_UNREGISTERED	6
/* Switches should use UNREGISTERED.
 * Processors should use SPECIFIC. */

/*
 * Use this if there is no vendor ID (CEC_G_VENDOR_ID) or if the vendor ID
 * should be disabled (CEC_S_VENDOR_ID)
 */
#define CEC_VENDOR_ID_NONE		0xffffffff

struct cec_event {
	__u64 ts;
	__u32 event;
	__u32 sequence;
	__u8 reserved[8];
};

/* The CEC state */
#define CEC_STATE_DISABLED		0
#define CEC_STATE_ENABLED		1

/* The passthrough mode state */
#define CEC_PASSTHROUGH_DISABLED	0
#define CEC_PASSTHROUGH_ENABLED		1

/* Userspace has to configure the adapter state (enable/disable) */
#define CEC_CAP_STATE		(1 << 0)
/* Userspace has to configure the physical address */
#define CEC_CAP_PHYS_ADDR	(1 << 1)
/* Userspace has to configure the logical addresses */
#define CEC_CAP_LOG_ADDRS	(1 << 2)
/* Userspace can transmit messages */
#define CEC_CAP_TRANSMIT	(1 << 3)
/* Userspace can receive messages */
#define CEC_CAP_RECEIVE		(1 << 4)
/* Userspace has to configure the vendor id */
#define CEC_CAP_VENDOR_ID	(1 << 5)
/* The hardware has the possibility to work in the passthrough */
#define CEC_CAP_PASSTHROUGH	(1 << 6)
/* Supports remote control */
#define CEC_CAP_RC		(1 << 7)
/* Supports ARC */
#define CEC_CAP_ARC		(1 << 8)
/* Supports CDC */
#define CEC_CAP_CDC		(1 << 9)

struct cec_caps {
	__u32 available_log_addrs;
	__u32 capabilities;
	__u8  reserved[40];
};

#define CEC_LOG_ADDRS_FL_HANDLE_MSGS	(1 << 0)

struct cec_log_addrs {
	__u8 cec_version;
	__u8 num_log_addrs;
	__u8 primary_device_type[CEC_MAX_LOG_ADDRS];
	__u8 log_addr_type[CEC_MAX_LOG_ADDRS];
	__u8 log_addr[CEC_MAX_LOG_ADDRS];
	__u8 flags[CEC_MAX_LOG_ADDRS];
	char osd_name[15];

	/* CEC 2.0 */
	__u8 all_device_types[CEC_MAX_LOG_ADDRS];
	__u8 features[CEC_MAX_LOG_ADDRS][12];

	__u8 reserved[63];
};

/* Commands */

/* One Touch Play Feature */
#define CEC_MSG_ACTIVE_SOURCE				0x82
#define CEC_MSG_IMAGE_VIEW_ON				0x04
#define CEC_MSG_TEXT_VIEW_ON				0x0d


/* Routing Control Feature */

/*
 * Has also:
 *	CEC_MSG_ACTIVE_SOURCE
 */

#define CEC_MSG_INACTIVE_SOURCE				0x9d
#define CEC_MSG_REQUEST_ACTIVE_SOURCE			0x85
#define CEC_MSG_ROUTING_CHANGE				0x80
#define CEC_MSG_ROUTING_INFORMATION			0x81
#define CEC_MSG_SET_STREAM_PATH				0x86


/* Standby Feature */
#define CEC_MSG_STANDBY					0x36


/* One Touch Record Feature */
#define CEC_MSG_RECORD_OFF				0x0b
#define CEC_MSG_RECORD_ON				0x09
/* Record Source Type Operand (rec_src_type) */
#define CEC_OP_RECORD_SRC_OWN				0x01
#define CEC_OP_RECORD_SRC_DIGITAL			0x02
#define CEC_OP_RECORD_SRC_ANALOG			0x03
#define CEC_OP_RECORD_SRC_EXT_PLUG			0x04
#define CEC_OP_RECORD_SRC_EXT_PHYS_ADDR			0x05
/* Service Identification Method Operand (service_id_method) */
#define CEC_OP_SERVICE_ID_METHOD_BY_DIG_ID		0x00
#define CEC_OP_SERVICE_ID_METHOD_BY_CHANNEL		0x01
/* Digital Service Broadcast System Operand (dig_bcast_system) */
#define CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ARIB_GEN	0x00
#define CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ATSC_GEN	0x01
#define CEC_OP_DIG_SERVICE_BCAST_SYSTEM_DVB_GEN		0x02
#define CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ARIB_BS		0x08
#define CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ARIB_CS		0x09
#define CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ARIB_T		0x0a
#define CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ATSC_CABLE	0x10
#define CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ATSC_SAT	0x11
#define CEC_OP_DIG_SERVICE_BCAST_SYSTEM_ATSC_T		0x12
#define CEC_OP_DIG_SERVICE_BCAST_SYSTEM_DVB_C		0x18
#define CEC_OP_DIG_SERVICE_BCAST_SYSTEM_DVB_S		0x19
#define CEC_OP_DIG_SERVICE_BCAST_SYSTEM_DVB_S2		0x1a
#define CEC_OP_DIG_SERVICE_BCAST_SYSTEM_DVB_T		0x1b
/* Analogue Broadcast Type Operand (ana_bcast_type) */
#define CEC_OP_ANA_BCAST_TYPE_CABLE			0x00
#define CEC_OP_ANA_BCAST_TYPE_SATELLITE			0x01
#define CEC_OP_ANA_BCAST_TYPE_TERRESTRIAL		0x02
/* Broadcast System Operand (bcast_system) */
#define CEC_OP_BCAST_SYSTEM_PAL_BG			0x00
#define CEC_OP_BCAST_SYSTEM_SECAM_LQ			0x01 /* SECAM L' */
#define CEC_OP_BCAST_SYSTEM_PAL_M			0x02
#define CEC_OP_BCAST_SYSTEM_NTSC_M			0x03
#define CEC_OP_BCAST_SYSTEM_PAL_I			0x04
#define CEC_OP_BCAST_SYSTEM_SECAM_DK			0x05
#define CEC_OP_BCAST_SYSTEM_SECAM_BG			0x06
#define CEC_OP_BCAST_SYSTEM_SECAM_L			0x07
#define CEC_OP_BCAST_SYSTEM_PAL_DK			0x08
#define CEC_OP_BCAST_SYSTEM_OTHER			0x1f
/* Channel Number Format Operand (channel_number_fmt) */
#define CEC_OP_CHANNEL_NUMBER_FMT_1_PART		0x01
#define CEC_OP_CHANNEL_NUMBER_FMT_2_PART		0x02

#define CEC_MSG_RECORD_STATUS				0x0a
/* Record Status Operand (rec_status) */
#define CEC_OP_RECORD_STATUS_CUR_SRC			0x01
#define CEC_OP_RECORD_STATUS_DIG_SERVICE		0x02
#define CEC_OP_RECORD_STATUS_ANA_SERVICE		0x03
#define CEC_OP_RECORD_STATUS_EXT_INPUT			0x04
#define CEC_OP_RECORD_STATUS_NO_DIG_SERVICE		0x05
#define CEC_OP_RECORD_STATUS_NO_ANA_SERVICE		0x06
#define CEC_OP_RECORD_STATUS_NO_SERVICE			0x07
#define CEC_OP_RECORD_STATUS_INVALID_EXT_PLUG		0x09
#define CEC_OP_RECORD_STATUS_INVALID_EXT_PHYS_ADDR	0x0a
#define CEC_OP_RECORD_STATUS_UNSUP_CA			0x0b
#define CEC_OP_RECORD_STATUS_NO_CA_ENTITLEMENTS		0x0c
#define CEC_OP_RECORD_STATUS_CANT_COPY_SRC		0x0d
#define CEC_OP_RECORD_STATUS_NO_MORE_COPIES		0x0e
#define CEC_OP_RECORD_STATUS_NO_MEDIA			0x10
#define CEC_OP_RECORD_STATUS_PLAYING			0x11
#define CEC_OP_RECORD_STATUS_ALREADY_RECORDING		0x12
#define CEC_OP_RECORD_STATUS_MEDIA_PROT			0x13
#define CEC_OP_RECORD_STATUS_NO_SIGNAL			0x14
#define CEC_OP_RECORD_STATUS_MEDIA_PROBLEM		0x15
#define CEC_OP_RECORD_STATUS_NO_SPACE			0x16
#define CEC_OP_RECORD_STATUS_PARENTAL_LOCK		0x17
#define CEC_OP_RECORD_STATUS_TERMINATED_OK		0x1a
#define CEC_OP_RECORD_STATUS_ALREADY_TERM		0x1b
#define CEC_OP_RECORD_STATUS_OTHER			0x1f

#define CEC_MSG_RECORD_TV_SCREEN			0x0f


/* Timer Programming Feature */
#define CEC_MSG_CLEAR_ANALOGUE_TIMER			0x33
/* Recording Sequence Operand (recording_seq) */
#define CEC_OP_REC_SEQ_SUNDAY				0x01
#define CEC_OP_REC_SEQ_MONDAY				0x02
#define CEC_OP_REC_SEQ_TUESDAY				0x04
#define CEC_OP_REC_SEQ_WEDNESDAY			0x08
#define CEC_OP_REC_SEQ_THURSDAY				0x10
#define CEC_OP_REC_SEQ_FRIDAY				0x20
#define CEC_OP_REC_SEQ_SATERDAY				0x40
#define CEC_OP_REC_SEQ_ONCE_ONLY			0x00

#define CEC_MSG_CLEAR_DIGITAL_TIMER			0x99

#define CEC_MSG_CLEAR_EXT_TIMER				0xa1
/* External Source Specifier Operand (ext_src_spec) */
#define CEC_OP_EXT_SRC_PLUG				0x04
#define CEC_OP_EXT_SRC_PHYS_ADDR			0x05

#define CEC_MSG_SET_ANALOGUE_TIMER			0x34
#define CEC_MSG_SET_DIGITAL_TIMER			0x97
#define CEC_MSG_SET_EXT_TIMER				0xa2

#define CEC_MSG_SET_TIMER_PROGRAM_TITLE			0x67
#define CEC_MSG_TIMER_CLEARED_STATUS			0x43
/* Timer Cleared Status Data Operand (timer_cleared_status) */
#define CEC_OP_TIMER_CLR_STAT_RECORDING			0x00
#define CEC_OP_TIMER_CLR_STAT_NO_MATCHING		0x01
#define CEC_OP_TIMER_CLR_STAT_NO_INFO			0x02
#define CEC_OP_TIMER_CLR_STAT_CLEARED			0x80

#define CEC_MSG_TIMER_STATUS				0x35
/* Timer Overlap Warning Operand (timer_overlap_warning) */
#define CEC_OP_TIMER_OVERLAP_WARNING_NO_OVERLAP		0x00
#define CEC_OP_TIMER_OVERLAP_WARNING_OVERLAP		0x01
/* Media Info Operand (media_info) */
#define CEC_OP_MEDIA_INFO_UNPROT_MEDIA			0x00
#define CEC_OP_MEDIA_INFO_PROT_MEDIA			0x01
#define CEC_OP_MEDIA_INFO_NO_MEDIA			0x02
/* Programmed Indicator Operand (prog_indicator) */
#define CEC_OP_PROG_IND_NOT_PROGRAMMED			0x00
#define CEC_OP_PROG_IND_PROGRAMMED			0x01
/* Programmed Info Operand (prog_info) */
#define CEC_OP_PROG_INFO_ENOUGH_SPACE			0x08
#define CEC_OP_PROG_INFO_NOT_ENOUGH_SPACE		0x09
#define CEC_OP_PROG_INFO_MIGHT_NOT_BE_ENOUGH_SPACE	0x0b
#define CEC_OP_PROG_INFO_NONE_AVAILABLE			0x0a
/* Not Programmed Error Info Operand (prog_error) */
#define CEC_OP_PROG_ERROR_NO_FREE_TIMER			0x01
#define CEC_OP_PROG_ERROR_DATE_OUT_OF_RANGE		0x02
#define CEC_OP_PROG_ERROR_REC_SEQ_ERROR			0x03
#define CEC_OP_PROG_ERROR_INV_EXT_PLUG			0x04
#define CEC_OP_PROG_ERROR_INV_EXT_PHYS_ADDR		0x05
#define CEC_OP_PROG_ERROR_CA_UNSUPP			0x06
#define CEC_OP_PROG_ERROR_INSUF_CA_ENTITLEMENTS		0x07
#define CEC_OP_PROG_ERROR_RESOLUTION_UNSUPP		0x08
#define CEC_OP_PROG_ERROR_PARENTAL_LOCK			0x09
#define CEC_OP_PROG_ERROR_CLOCK_FAILURE			0x0a
#define CEC_OP_PROG_ERROR_DUPLICATE			0x0e


/* System Information Feature */
#define CEC_MSG_CEC_VERSION				0x9e
/* CEC Version Operand (cec_version) */
#define CEC_OP_CEC_VERSION_1_3A				4
#define CEC_OP_CEC_VERSION_1_4				5
#define CEC_OP_CEC_VERSION_2_0				6

#define CEC_MSG_GET_CEC_VERSION				0x9f
#define CEC_MSG_GIVE_PHYSICAL_ADDR			0x83
#define CEC_MSG_GET_MENU_LANGUAGE			0x91
#define CEC_MSG_REPORT_PHYSICAL_ADDR			0x84
/* Primary Device Type Operand (prim_devtype) */
#define CEC_OP_PRIM_DEVTYPE_TV				0
#define CEC_OP_PRIM_DEVTYPE_RECORD			1
#define CEC_OP_PRIM_DEVTYPE_TUNER			3
#define CEC_OP_PRIM_DEVTYPE_PLAYBACK			4
#define CEC_OP_PRIM_DEVTYPE_AUDIOSYSTEM			5
#define CEC_OP_PRIM_DEVTYPE_SWITCH			6
#define CEC_OP_PRIM_DEVTYPE_PROCESSOR			7

#define CEC_MSG_SET_MENU_LANGUAGE			0x32
#define CEC_MSG_REPORT_FEATURES				0xa6	/* HDMI 2.0 */
/* All Device Types Operand (all_device_types) */
#define CEC_OP_ALL_DEVTYPE_TV				0x80
#define CEC_OP_ALL_DEVTYPE_RECORD			0x40
#define CEC_OP_ALL_DEVTYPE_TUNER			0x20
#define CEC_OP_ALL_DEVTYPE_PLAYBACK			0x10
#define CEC_OP_ALL_DEVTYPE_AUDIOSYSTEM			0x08
#define CEC_OP_ALL_DEVTYPE_SWITCH			0x04
/* And if you wondering what happened to PROCESSOR devices: those should
 * be mapped to a SWITCH. */

/* Valid for RC Profile and Device Feature operands */
#define CEC_OP_FEAT_EXT					0x80	/* Extension bit */
/* RC Profile Operand (rc_profile) */
#define CEC_OP_FEAT_RC_TV_PROFILE_NONE			0x00
#define CEC_OP_FEAT_RC_TV_PROFILE_1			0x02
#define CEC_OP_FEAT_RC_TV_PROFILE_2			0x06
#define CEC_OP_FEAT_RC_TV_PROFILE_3			0x0a
#define CEC_OP_FEAT_RC_TV_PROFILE_4			0x0e
#define CEC_OP_FEAT_RC_SRC_HAS_DEV_ROOT_MENU		0x50
#define CEC_OP_FEAT_RC_SRC_HAS_DEV_SETUP_MENU		0x48
#define CEC_OP_FEAT_RC_SRC_HAS_CONTENTS_MENU		0x44
#define CEC_OP_FEAT_RC_SRC_HAS_MEDIA_TOP_MENU		0x42
#define CEC_OP_FEAT_RC_SRC_HAS_MEDIA_CONTEXT_MENU	0x41
/* Device Feature Operand (dev_features) */
#define CEC_OP_FEAT_DEV_HAS_RECORD_TV_SCREEN		0x40
#define CEC_OP_FEAT_DEV_HAS_SET_OSD_STRING		0x20
#define CEC_OP_FEAT_DEV_HAS_DECK_CONTROL		0x10
#define CEC_OP_FEAT_DEV_HAS_SET_AUDIO_RATE		0x08
#define CEC_OP_FEAT_DEV_SINK_HAS_ARC_TX			0x04
#define CEC_OP_FEAT_DEV_SOURCE_HAS_ARC_RX		0x02

#define CEC_MSG_GIVE_FEATURES				0xa5	/* HDMI 2.0 */


/* Deck Control Feature */
#define CEC_MSG_DECK_CONTROL				0x42
/* Deck Control Mode Operand (deck_control_mode) */
#define CEC_OP_DECK_CTL_MODE_SKIP_FWD			0x01
#define CEC_OP_DECK_CTL_MODE_SKIP_REV			0x02
#define CEC_OP_DECK_CTL_MODE_STOP			0x03
#define CEC_OP_DECK_CTL_MODE_EJECT			0x04

#define CEC_MSG_DECK_STATUS				0x1b
/* Deck Info Operand (deck_info) */
#define CEC_OP_DECK_INFO_PLAY				0x11
#define CEC_OP_DECK_INFO_RECORD				0x12
#define CEC_OP_DECK_INFO_PLAY_REV			0x13
#define CEC_OP_DECK_INFO_STILL				0x14
#define CEC_OP_DECK_INFO_SLOW				0x15
#define CEC_OP_DECK_INFO_SLOW_REV			0x16
#define CEC_OP_DECK_INFO_FAST_FWD			0x17
#define CEC_OP_DECK_INFO_FAST_REV			0x18
#define CEC_OP_DECK_INFO_NO_MEDIA			0x19
#define CEC_OP_DECK_INFO_STOP				0x1a
#define CEC_OP_DECK_INFO_SKIP_FWD			0x1b
#define CEC_OP_DECK_INFO_SKIP_REV			0x1c
#define CEC_OP_DECK_INFO_INDEX_SEARCH_FWD		0x1d
#define CEC_OP_DECK_INFO_INDEX_SEARCH_REV		0x1e
#define CEC_OP_DECK_INFO_OTHER				0x1f

#define CEC_MSG_GIVE_DECK_STATUS			0x1a
/* Status Request Operand (status_req) */
#define CEC_OP_STATUS_REQ_ON				0x01
#define CEC_OP_STATUS_REQ_OFF				0x02
#define CEC_OP_STATUS_REQ_ONCE				0x03

#define CEC_MSG_PLAY					0x41
/* Play Mode Operand (play_mode) */
#define CEC_OP_PLAY_MODE_PLAY_FWD			0x24
#define CEC_OP_PLAY_MODE_PLAY_REV			0x20
#define CEC_OP_PLAY_MODE_PLAY_STILL			0x25
#define CEC_OP_PLAY_MODE_PLAY_FAST_FWD_MIN		0x05
#define CEC_OP_PLAY_MODE_PLAY_FAST_FWD_MED		0x06
#define CEC_OP_PLAY_MODE_PLAY_FAST_FWD_MAX		0x07
#define CEC_OP_PLAY_MODE_PLAY_FAST_REV_MIN		0x09
#define CEC_OP_PLAY_MODE_PLAY_FAST_REV_MED		0x0a
#define CEC_OP_PLAY_MODE_PLAY_FAST_REV_MAX		0x0b
#define CEC_OP_PLAY_MODE_PLAY_SLOW_FWD_MIN		0x15
#define CEC_OP_PLAY_MODE_PLAY_SLOW_FWD_MED		0x16
#define CEC_OP_PLAY_MODE_PLAY_SLOW_FWD_MAX		0x17
#define CEC_OP_PLAY_MODE_PLAY_SLOW_REV_MIN		0x19
#define CEC_OP_PLAY_MODE_PLAY_SLOW_REV_MED		0x1a
#define CEC_OP_PLAY_MODE_PLAY_SLOW_REV_MAX		0x1b


/* Tuner Control Feature */
#define CEC_MSG_GIVE_TUNER_DEVICE_STATUS		0x08
#define CEC_MSG_SELECT_ANALOGUE_SERVICE			0x92
#define CEC_MSG_SELECT_DIGITAL_SERVICE			0x93
#define CEC_MSG_TUNER_DEVICE_STATUS			0x07
/* Recording Flag Operand (rec_flag) */
#define CEC_OP_REC_FLAG_USED				0x00
#define CEC_OP_REC_FLAG_NOT_USED			0x01
/* Tuner Display Info Operand (tuner_display_info) */
#define CEC_OP_TUNER_DISPLAY_INFO_DIGITAL		0x00
#define CEC_OP_TUNER_DISPLAY_INFO_NONE			0x01
#define CEC_OP_TUNER_DISPLAY_INFO_ANALOGUE		0x02

#define CEC_MSG_TUNER_STEP_DECREMENT			0x06
#define CEC_MSG_TUNER_STEP_INCREMENT			0x05


/* Vendor Specific Commands Feature */

/*
 * Has also:
 *	CEC_MSG_CEC_VERSION
 *	CEC_MSG_GET_CEC_VERSION
 */
#define CEC_MSG_DEVICE_VENDOR_ID			0x87
#define CEC_MSG_GIVE_DEVICE_VENDOR_ID			0x8c
#define CEC_MSG_VENDOR_COMMAND				0x89
#define CEC_MSG_VENDOR_COMMAND_WITH_ID			0xa0
#define CEC_MSG_VENDOR_REMOTE_BUTTON_DOWN		0x8a
#define CEC_MSG_VENDOR_REMOTE_BUTTON_UP			0x8b


/* OSD Display Feature */
#define CEC_MSG_SET_OSD_STRING				0x64
/* Display Control Operand (disp_ctl) */
#define CEC_OP_DISP_CTL_DEFAULT				0x00
#define CEC_OP_DISP_CTL_UNTIL_CLEARED			0x40
#define CEC_OP_DISP_CTL_CLEAR				0x80


/* Device OSD Transfer Feature */
#define CEC_MSG_GIVE_OSD_NAME				0x46
#define CEC_MSG_SET_OSD_NAME				0x47


/* Device Menu Control Feature */
#define CEC_MSG_MENU_REQUEST				0x8d
/* Menu Request Type Operand (menu_req) */
#define CEC_OP_MENU_REQUEST_ACTIVATE			0x00
#define CEC_OP_MENU_REQUEST_DEACTIVATE			0x01
#define CEC_OP_MENU_REQUEST_QUERY			0x02

#define CEC_MSG_MENU_STATUS				0x8e
/* Menu State Operand (menu_state) */
#define CEC_OP_MENU_STATE_ACTIVATED			0x00
#define CEC_OP_MENU_STATE_DEACTIVATED			0x01

#define CEC_MSG_USER_CONTROL_PRESSED			0x44
/* UI Broadcast Type Operand (ui_bcast_type) */
#define CEC_OP_UI_BCAST_TYPE_TOGGLE_ALL			0x00
#define CEC_OP_UI_BCAST_TYPE_TOGGLE_DIG_ANA		0x01
#define CEC_OP_UI_BCAST_TYPE_ANALOGUE			0x10
#define CEC_OP_UI_BCAST_TYPE_ANALOGUE_T			0x20
#define CEC_OP_UI_BCAST_TYPE_ANALOGUE_CABLE		0x30
#define CEC_OP_UI_BCAST_TYPE_ANALOGUE_SAT		0x40
#define CEC_OP_UI_BCAST_TYPE_DIGITAL			0x50
#define CEC_OP_UI_BCAST_TYPE_DIGITAL_T			0x60
#define CEC_OP_UI_BCAST_TYPE_DIGITAL_CABLE		0x70
#define CEC_OP_UI_BCAST_TYPE_DIGITAL_SAT		0x80
#define CEC_OP_UI_BCAST_TYPE_DIGITAL_COM_SAT		0x90
#define CEC_OP_UI_BCAST_TYPE_DIGITAL_COM_SAT2		0x91
#define CEC_OP_UI_BCAST_TYPE_IP				0xa0
/* UI Sound Presentation Control Operand (ui_snd_pres_ctl) */
#define CEC_OP_UI_SND_PRES_CTL_DUAL_MONO		0x10
#define CEC_OP_UI_SND_PRES_CTL_KARAOKE			0x20
#define CEC_OP_UI_SND_PRES_CTL_DOWNMIX			0x80
#define CEC_OP_UI_SND_PRES_CTL_REVERB			0x90
#define CEC_OP_UI_SND_PRES_CTL_EQUALIZER		0xa0
#define CEC_OP_UI_SND_PRES_CTL_BASS_UP			0xb1
#define CEC_OP_UI_SND_PRES_CTL_BASS_NEUTRAL		0xb2
#define CEC_OP_UI_SND_PRES_CTL_BASS_DOWN		0xb3
#define CEC_OP_UI_SND_PRES_CTL_TREBLE_UP		0xc1
#define CEC_OP_UI_SND_PRES_CTL_TREBLE_NEUTRAL		0xc2
#define CEC_OP_UI_SND_PRES_CTL_TREBLE_DOWN		0xc3

#define CEC_MSG_USER_CONTROL_RELEASED			0x45


/* Remote Control Passthrough Feature */

/*
 * Has also:
 *	CEC_MSG_USER_CONTROL_PRESSED
 *	CEC_MSG_USER_CONTROL_RELEASED
 */


/* Power Status Feature */
#define CEC_MSG_GIVE_DEVICE_POWER_STATUS		0x8f
#define CEC_MSG_REPORT_POWER_STATUS			0x90
/* Power Status Operand (pwr_state) */
#define CEC_OP_POWER_STATUS_ON				0x00
#define CEC_OP_POWER_STATUS_STANDBY			0x01
#define CEC_OP_POWER_STATUS_TO_ON			0x02
#define CEC_OP_POWER_STATUS_TO_STANDBY			0x03


/* General Protocol Messages */
#define CEC_MSG_FEATURE_ABORT				0x00
/* Abort Reason Operand (reason) */
#define CEC_OP_ABORT_UNRECOGNIZED_OP			0
#define CEC_OP_ABORT_INCORRECT_MODE			1
#define CEC_OP_ABORT_NO_SOURCE				2
#define CEC_OP_ABORT_INVALID_OP				3
#define CEC_OP_ABORT_REFUSED				4
#define CEC_OP_ABORT_UNDETERMINED			5

#define CEC_MSG_ABORT					0xff


/* System Audio Control Feature */

/*
 * Has also:
 *	CEC_MSG_USER_CONTROL_PRESSED
 *	CEC_MSG_USER_CONTROL_RELEASED
 */
#define CEC_MSG_GIVE_AUDIO_STATUS			0x71
#define CEC_MSG_GIVE_SYSTEM_AUDIO_MODE_STATUS		0x7d
#define CEC_MSG_REPORT_AUDIO_STATUS			0x7a
/* Audio Mute Status Operand (aud_mute_status) */
#define CEC_OP_AUD_MUTE_STATUS_OFF			0x00
#define CEC_OP_AUD_MUTE_STATUS_ON			0x01

#define CEC_MSG_REPORT_SHORT_AUDIO_DESCRIPTOR		0xa3
#define CEC_MSG_REQUEST_SHORT_AUDIO_DESCRIPTOR		0xa4
#define CEC_MSG_SET_SYSTEM_AUDIO_MODE			0x72
/* System Audio Status Operand (sys_aud_status) */
#define CEC_OP_SYS_AUD_STATUS_OFF			0x00
#define CEC_OP_SYS_AUD_STATUS_ON			0x01

#define CEC_MSG_SYSTEM_AUDIO_MODE_REQUEST		0x70
#define CEC_MSG_SYSTEM_AUDIO_MODE_STATUS		0x7e
/* Audio Format ID Operand (audio_format_id) */
#define CEC_OP_AUD_FMT_ID_CEA861			0x00
#define CEC_OP_AUD_FMT_ID_CEA861_CXT			0x01


/* Audio Rate Control Feature */
#define CEC_MSG_SET_AUDIO_RATE				0x9a
/* Audio Rate Operand (audio_rate) */
#define CEC_OP_AUD_RATE_OFF				0x00
#define CEC_OP_AUD_RATE_WIDE_STD			0x01
#define CEC_OP_AUD_RATE_WIDE_FAST			0x02
#define CEC_OP_AUD_RATE_WIDE_SLOW			0x03
#define CEC_OP_AUD_RATE_NARROW_STD			0x04
#define CEC_OP_AUD_RATE_NARROW_FAST			0x05
#define CEC_OP_AUD_RATE_NARROW_SLOW			0x06


/* Audio Return Channel Control Feature */
#define CEC_MSG_INITIATE_ARC				0xc0
#define CEC_MSG_REPORT_ARC_INITIATED			0xc1
#define CEC_MSG_REPORT_ARC_TERMINATED			0xc2
#define CEC_MSG_REQUEST_ARC_INITIATION			0xc3
#define CEC_MSG_REQUEST_ARC_TERMINATION			0xc4
#define CEC_MSG_TERMINATE_ARC				0xc5


/* Dynamic Audio Lipsync Feature */
/* Only for CEC 2.0 and up */
#define CEC_MSG_REQUEST_CURRENT_LATENCY			0xa7
#define CEC_MSG_REPORT_CURRENT_LATENCY			0xa8
/* Low Latency Mode Operand (low_latency_mode) */
#define CEC_OP_LOW_LATENCY_MODE_OFF			0x00
#define CEC_OP_LOW_LATENCY_MODE_ON			0x01
/* Audio Output Compensated Operand (audio_out_compensated) */
#define CEC_OP_AUD_OUT_COMPENSATED_NA			0x00
#define CEC_OP_AUD_OUT_COMPENSATED_DELAY		0x01
#define CEC_OP_AUD_OUT_COMPENSATED_NO_DELAY		0x02
#define CEC_OP_AUD_OUT_COMPENSATED_PARTIAL_DELAY	0x03


/* Capability Discovery and Control Feature */
#define CEC_MSG_CDC_MESSAGE				0xf8
/* Ethernet-over-HDMI: nobody ever does this... */
#define CEC_MSG_CDC_HEC_INQUIRE_STATE			0x00
#define CEC_MSG_CDC_HEC_REPORT_STATE			0x01
#define CEC_MSG_CDC_HEC_SET_STATE_ADJACENT		0x02
#define CEC_MSG_CDC_HEC_SET_STATE			0x03
#define CEC_MSG_CDC_HEC_REQUEST_DEACTIVATION		0x04
#define CEC_MSG_CDC_HEC_NOTIFY_ALIVE			0x05
#define CEC_MSG_CDC_HEC_DISCOVER			0x06
/* Hotplug Detect messages */
#define CEC_MSG_CDC_HPD_SET_STATE			0x10
/* CDC HPD State Operand */
#define CEC_OP_HPD_STATE_CP_EDID_DISABLE		0x00
#define CEC_OP_HPD_STATE_CP_EDID_ENABLE			0x01
#define CEC_OP_HPD_STATE_CP_EDID_DISABLE_ENABLE		0x02
#define CEC_OP_HPD_STATE_EDID_DISABLE			0x03
#define CEC_OP_HPD_STATE_EDID_ENABLE			0x04
#define CEC_OP_HPD_STATE_EDID_DISABLE_ENABLE		0x05
#define CEC_MSG_CDC_HPD_REPORT_STATE			0x11
/* CDC HPD Error Code Operand */
#define CEC_OP_HPD_ERROR_NONE				0x00
#define CEC_OP_HPD_ERROR_INITIATOR_NOT_CAPABLE		0x01
#define CEC_OP_HPD_ERROR_INITIATOR_WRONG_STATE		0x02
#define CEC_OP_HPD_ERROR_OTHER				0x03
#define CEC_OP_HPD_ERROR_NONE_NO_VIDEO			0x04

/* Events */
/* Event that occurs when a cable is connected */
#define CEC_EVENT_CONNECT	1
/* Event that occurs when all logical addresses were claimed */
#define CEC_EVENT_READY		2
/* Event that is sent when the cable is disconnected */
#define CEC_EVENT_DISCONNECT	3
/* This event is sent when a reply to a message is received */
#define CEC_EVENT_GOT_REPLY	4

/* ioctls */

/* issue a CEC command */
#define CEC_G_CAPS		_IOWR('a', 0, struct cec_caps)
#define CEC_TRANSMIT		_IOWR('a', 1, struct cec_msg)
#define CEC_RECEIVE		_IOWR('a', 2, struct cec_msg)

/*
   Configure the CEC adapter. It sets the device type and which
   logical types it will try to claim. It will return which
   logical addresses it could actually claim.
   An error is returned if the adapter is disabled or if there
   is no physical address assigned.
 */

#define CEC_G_ADAP_LOG_ADDRS	_IOR('a', 3, struct cec_log_addrs)
#define CEC_S_ADAP_LOG_ADDRS	_IOWR('a', 4, struct cec_log_addrs)

/*
   Enable/disable the adapter. The Set state ioctl may not
   be available if that is handled internally.
 */
#define CEC_G_ADAP_STATE	_IOR('a', 5, __u32)
#define CEC_S_ADAP_STATE	_IOW('a', 6, __u32)

/*
   phys_addr is either 0 (if this is the CEC root device)
   or a valid physical address obtained from the sink's EDID
   as read by this CEC device (if this is a source device)
   or a physical address obtained and modified from a sink
   EDID and used for a sink CEC device.
   If nothing is connected, then phys_addr is 0xffff.
   See HDMI 1.4b, section 8.7 (Physical Address).

   The Set ioctl may not be available if that is handled
   internally.
 */
#define CEC_G_ADAP_PHYS_ADDR	_IOR('a', 7, __u16)
#define CEC_S_ADAP_PHYS_ADDR	_IOW('a', 8, __u16)

#define CEC_G_EVENT		_IOWR('a', 9, struct cec_event)
/*
   Read and set the vendor ID of the CEC adapter.
 */
#define CEC_G_VENDOR_ID		_IOR('a', 10, __u32)
#define CEC_S_VENDOR_ID		_IOW('a', 11, __u32)
/*
   Enable/disable the passthrough mode
 */
#define CEC_G_PASSTHROUGH	_IOR('a', 12, __u32)
#define CEC_S_PASSTHROUGH	_IOW('a', 13, __u32)

#endif
