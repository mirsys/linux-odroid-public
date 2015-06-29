#include <linux/errno.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kmod.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <media/cec.h>

#define CEC_NUM_DEVICES	256
#define CEC_NAME	"cec"

static int debug;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "debug level (0-2)");

struct cec_transmit_notifier {
	struct completion c;
	struct cec_data *data;
};

#define dprintk(lvl, fmt, arg...)					\
	do {								\
		if (lvl <= debug)					\
			pr_info("cec-%s: " fmt, adap->name, ## arg);	\
	} while (0)

static dev_t cec_dev_t;

/* Active devices */
static DEFINE_MUTEX(cec_devnode_lock);
static DECLARE_BITMAP(cec_devnode_nums, CEC_NUM_DEVICES);

/* dev to cec_devnode */
#define to_cec_devnode(cd) container_of(cd, struct cec_devnode, dev)

static inline struct cec_devnode *cec_devnode_data(struct file *filp)
{
	return filp->private_data;
}

static bool cec_are_adjacent(const struct cec_adapter *adap, u8 la1, u8 la2)
{
	u16 pa1 = adap->phys_addrs[la1];
	u16 pa2 = adap->phys_addrs[la2];
	u16 mask = 0xf000;
	int i;

	if (pa1 == 0xffff || pa2 == 0xffff)
		return false;
	for (i = 0; i < 3; i++) {
		if ((pa1 & mask) != (pa2 & mask))
			break;
		mask = (mask >> 4) | 0xf000;
	}
	if ((pa1 & ~mask) || (pa2 & ~mask))
		return false;
	if (!(pa1 & mask) ^ !(pa2 & mask))
		return true;
	return false;
}

static int cec_log_addr2idx(const struct cec_adapter *adap, u8 log_addr)
{
	int i;

	for (i = 0; i < adap->num_log_addrs; i++)
		if (adap->log_addr[i] == log_addr)
			return i;
	return -1;
}

static unsigned cec_log_addr2dev(const struct cec_adapter *adap, u8 log_addr)
{
	int i = cec_log_addr2idx(adap, log_addr);

	return adap->prim_device[i < 0 ? 0 : i];
}

/* Called when the last user of the cec device exits. */
static void cec_devnode_release(struct device *cd)
{
	struct cec_devnode *cecdev = to_cec_devnode(cd);

	mutex_lock(&cec_devnode_lock);

	/* Delete the cdev on this minor as well */
	cdev_del(&cecdev->cdev);

	/* Mark device node number as free */
	clear_bit(cecdev->minor, cec_devnode_nums);

	mutex_unlock(&cec_devnode_lock);

	/* Release cec_devnode and perform other cleanups as needed. */
	if (cecdev->release)
		cecdev->release(cecdev);
}

static struct bus_type cec_bus_type = {
	.name = CEC_NAME,
};

static bool cec_sleep(struct cec_adapter *adap, int timeout)
{
	bool timed_out = false;

	DECLARE_WAITQUEUE(wait, current);

	add_wait_queue(&adap->kthread_waitq, &wait);
	if (!kthread_should_stop()) {
		if (timeout < 0) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule();
		} else {
			timed_out = !schedule_timeout_interruptible
				(msecs_to_jiffies(timeout));
		}
	}

	remove_wait_queue(&adap->kthread_waitq, &wait);
	return timed_out;
}

/*
 * Main CEC state machine
 *
 * In the IDLE state the CEC adapter is ready to receive or transmit messages.
 * If it is woken up it will check if a new message is queued, and if so it
 * will be transmitted and the state will go to TRANSMITTING.
 *
 * When the transmit is marked as done the state machine will check if it
 * should wait for a reply. If not, it will call the notifier and go back
 * to the IDLE state. Else it will switch to the WAIT state and wait for a
 * reply. When the reply arrives it will call the notifier and go back
 * to IDLE state.
 *
 * For the transmit and the wait-for-reply states a timeout is used of
 * 1 second as per the standard.
 */
static int cec_thread_func(void *data)
{
	struct cec_adapter *adap = data;
	int timeout = -1;

	for (;;) {
		bool timed_out = cec_sleep(adap, timeout);

		if (kthread_should_stop())
			break;
		timeout = -1;
		mutex_lock(&adap->lock);
		dprintk(2, "state %d timedout: %d tx: %d@%d\n", adap->state,
			timed_out, adap->tx_qcount, adap->tx_qstart);
		if (adap->state == CEC_ADAP_STATE_TRANSMITTING && timed_out)
			adap->adap_transmit_timed_out(adap);

		if (adap->state == CEC_ADAP_STATE_WAIT ||
		    adap->state == CEC_ADAP_STATE_TRANSMITTING) {
			struct cec_data *data = adap->tx_queue +
						adap->tx_qstart;

			if (adap->state == CEC_ADAP_STATE_TRANSMITTING &&
			    data->msg.reply && !timed_out &&
			    data->msg.status == CEC_TX_STATUS_OK) {
				adap->state = CEC_ADAP_STATE_WAIT;
				timeout = 1000;
			} else {
				if (timed_out) {
					data->msg.reply = 0;
					if (adap->state ==
					    CEC_ADAP_STATE_TRANSMITTING)
						data->msg.status =
						    CEC_TX_STATUS_RETRY_TIMEOUT;
					else
						data->msg.status =
						    CEC_TX_STATUS_REPLY_TIMEOUT;
				}
				adap->state = CEC_ADAP_STATE_IDLE;
				if (data->func) {
					mutex_unlock(&adap->lock);
					data->func(adap, data, data->priv);
					mutex_lock(&adap->lock);
				}
				adap->tx_qstart = (adap->tx_qstart + 1) %
						  CEC_TX_QUEUE_SZ;
				adap->tx_qcount--;
				wake_up_interruptible(&adap->waitq);
			}
		}
		if (adap->state == CEC_ADAP_STATE_IDLE && adap->tx_qcount) {
			adap->state = CEC_ADAP_STATE_TRANSMITTING;
			timeout = adap->tx_queue[adap->tx_qstart].msg.len == 1 ?
				  200 : 1000;
			adap->adap_transmit(adap,
					  &adap->tx_queue[adap->tx_qstart].msg);
			mutex_unlock(&adap->lock);
			continue;
		}
		mutex_unlock(&adap->lock);
	}
	return 0;
}

static int cec_transmit_notify(struct cec_adapter *adap, struct cec_data *data,
		void *priv)
{
	struct cec_transmit_notifier *n = priv;

	*(n->data) = *data;
	complete(&n->c);
	return 0;
}

int cec_transmit_msg(struct cec_adapter *adap, struct cec_data *data,
		     bool block)
{
	struct cec_transmit_notifier notifier;
	struct cec_msg *msg = &data->msg;
	bool msg_is_cdc = msg->msg[1] == CEC_MSG_CDC_MESSAGE;
	int res = 0;
	unsigned idx;

	if (msg->len == 0 || msg->len > 16) {
		dprintk(1, "cec_transmit_msg: invalid length %d\n", msg->len);
		return -EINVAL;
	}
	if (msg->reply && (msg->len == 1 ||
			   (cec_msg_is_broadcast(msg) && !msg_is_cdc))) {
		dprintk(1, "cec_transmit_msg: can't reply for poll or non-CDC broadcast msg\n");
		return -EINVAL;
	}
	if (msg->len > 1 && !cec_msg_is_broadcast(msg) &&
	    cec_msg_initiator(msg) == cec_msg_destination(msg)) {
		dprintk(1, "cec_transmit_msg: initiator == destination\n");
		return -EINVAL;
	}
	if (cec_msg_initiator(msg) != 0xf &&
	    cec_log_addr2idx(adap, cec_msg_initiator(msg)) < 0) {
		dprintk(1, "cec_transmit_msg: initiator has unknown logical address\n");
		return -EINVAL;
	}

	if (msg->len == 1)
		dprintk(2, "cec_transmit_msg: 0x%02x%s\n",
				msg->msg[0], !block ? " nb" : "");
	else if (msg->reply)
		dprintk(2, "cec_transmit_msg: 0x%02x 0x%02x (wait for 0x%02x)%s\n",
				msg->msg[0], msg->msg[1],
				msg->reply, !block ? " nb" : "");
	else
		dprintk(2, "cec_transmit_msg: 0x%02x 0x%02x%s\n",
				msg->msg[0], msg->msg[1],
				!block ? " nb" : "");

	msg->status = 0;
	msg->ts = 0;
	if (msg->reply)
		msg->timeout = 1000;
	if (block) {
		init_completion(&notifier.c);
		notifier.data = data;
		data->func = cec_transmit_notify;
		data->priv = &notifier;
	} else {
		data->func = NULL;
		data->priv = NULL;
	}
	mutex_lock(&adap->lock);
	idx = (adap->tx_qstart + adap->tx_qcount) % CEC_TX_QUEUE_SZ;
	if (adap->tx_qcount == CEC_TX_QUEUE_SZ) {
		dprintk(2, "cec_transmit_msg: queue full\n");
		res = -EBUSY;
	} else {
		adap->tx_queue[idx] = *data;
		adap->tx_qcount++;
		if (adap->state == CEC_ADAP_STATE_IDLE)
			wake_up_interruptible(&adap->kthread_waitq);
	}
	msg->sequence = adap->sequence++;
	data->blocking = block;
	mutex_unlock(&adap->lock);
	if (res || !block)
		return res;
	wait_for_completion_interruptible(&notifier.c);
	mutex_lock(&adap->lock);
	if (data->func)
		complete(&notifier.c);
	adap->tx_queue[idx].func = NULL;
	mutex_unlock(&adap->lock);
	return res;
}
EXPORT_SYMBOL_GPL(cec_transmit_msg);

void cec_transmit_done(struct cec_adapter *adap, u32 status)
{
	struct cec_msg *msg;

	dprintk(2, "cec_transmit_done\n");
	mutex_lock(&adap->lock);
	if (adap->state == CEC_ADAP_STATE_TRANSMITTING) {
		msg = &adap->tx_queue[adap->tx_qstart].msg;
		msg->status = status;
		if (status)
			msg->reply = 0;
		msg->ts = ktime_get_ns();
		wake_up_interruptible(&adap->kthread_waitq);
	}
	mutex_unlock(&adap->lock);
}
EXPORT_SYMBOL_GPL(cec_transmit_done);

static int cec_report_features(struct cec_adapter *adap, unsigned la_idx)
{
	struct cec_data data = { };
	u8 *features = adap->features[la_idx];
	bool op_is_dev_features = false;
	unsigned idx;

	if (adap->cec_version < CEC_OP_CEC_VERSION_2_0)
		return 0;

	/* Report Features */
	data.msg.msg[0] = (adap->log_addr[la_idx] << 4) | 0x0f;
	data.msg.len = 4;
	data.msg.msg[1] = CEC_MSG_REPORT_FEATURES;
	data.msg.msg[2] = adap->cec_version;
	data.msg.msg[3] = adap->all_device_types[la_idx];

	/* Write RC Profiles first, then Device Features */
	for (idx = 0; idx < sizeof(adap->features[0]); idx++) {
		data.msg.msg[data.msg.len++] = features[idx];
		if ((features[idx] & CEC_OP_FEAT_EXT) == 0) {
			if (op_is_dev_features)
				break;
			op_is_dev_features = true;
		}
	}
	return cec_transmit_msg(adap, &data, false);
}

static int cec_report_phys_addr(struct cec_adapter *adap, unsigned la_idx)
{
	struct cec_data data = { };

	/* Report Physical Address */
	data.msg.msg[0] = (adap->log_addr[la_idx] << 4) | 0x0f;
	cec_msg_report_physical_addr(&data.msg, adap->phys_addr,
				     adap->prim_device[la_idx]);
	dprintk(2, "config: la %d pa %x.%x.%x.%x\n",
			adap->log_addr[la_idx],
			cec_phys_addr_exp(adap->phys_addr));
	return cec_transmit_msg(adap, &data, false);
}

static int cec_feature_abort_reason(struct cec_adapter *adap,
				    struct cec_msg *msg, u8 reason)
{
	struct cec_data tx_data = { };

	cec_msg_set_reply_to(&tx_data.msg, msg);
	cec_msg_feature_abort(&tx_data.msg, msg->msg[1], reason);
	return cec_transmit_msg(adap, &tx_data, false);
}

static int cec_feature_abort(struct cec_adapter *adap, struct cec_msg *msg)
{
	return cec_feature_abort_reason(adap, msg,
					CEC_OP_ABORT_UNRECOGNIZED_OP);
}

static int cec_feature_refused(struct cec_adapter *adap, struct cec_msg *msg)
{
	return cec_feature_abort_reason(adap, msg,
					CEC_OP_ABORT_REFUSED);
}

static int cec_receive_notify(struct cec_adapter *adap, struct cec_msg *msg)
{
	bool is_broadcast = cec_msg_is_broadcast(msg);
	u8 dest_laddr = cec_msg_destination(msg);
	u8 init_laddr = cec_msg_initiator(msg);
	u8 devtype = cec_log_addr2dev(adap, dest_laddr);
	int la_idx = cec_log_addr2idx(adap, dest_laddr);
	bool is_directed = la_idx >= 0;
	bool from_unregistered = init_laddr == 0xf;
	u16 cdc_phys_addr;
	struct cec_data tx_data = { };
	u8 *tx_msg = tx_data.msg.msg;
	int res = 0;
	unsigned idx;

	if (msg->len <= 1)
		return 0;
	if (!is_directed && !is_broadcast && !adap->passthrough)
		return 0;	/* Not for us */

	dprintk(1, "cec_receive_notify: %02x %02x\n", msg->msg[0], msg->msg[1]);

	cec_msg_set_reply_to(&tx_data.msg, msg);

	if (adap->received) {
		res = adap->received(adap, msg);
		if (res != -ENOMSG)
			return 0;
		res = 0;
	}

	if (adap->passthrough)
		goto skip_processing;
	if (!is_directed && !is_broadcast)
		return 0;

	switch (msg->msg[1]) {
	case CEC_MSG_GET_CEC_VERSION:
	case CEC_MSG_GIVE_DEVICE_VENDOR_ID:
	case CEC_MSG_ABORT:
	case CEC_MSG_GIVE_DEVICE_POWER_STATUS:
	case CEC_MSG_USER_CONTROL_PRESSED:
	case CEC_MSG_USER_CONTROL_RELEASED:
	case CEC_MSG_GIVE_FEATURES:
	case CEC_MSG_GIVE_PHYSICAL_ADDR:
	case CEC_MSG_GIVE_OSD_NAME:
	case CEC_MSG_INITIATE_ARC:
	case CEC_MSG_TERMINATE_ARC:
	case CEC_MSG_REQUEST_ARC_INITIATION:
	case CEC_MSG_REQUEST_ARC_TERMINATION:
		if (is_broadcast || from_unregistered)
			return 0;
		break;
	case CEC_MSG_REPORT_PHYSICAL_ADDR:
	case CEC_MSG_CDC_MESSAGE:
		if (!is_broadcast)
			return 0;
		break;
	default:
		break;
	}

	if (adap->cec_version < CEC_OP_CEC_VERSION_2_0)
		goto skip_processing;

	switch (msg->msg[1]) {
	case CEC_MSG_GET_CEC_VERSION:
		cec_msg_cec_version(&tx_data.msg, adap->cec_version);
		return cec_transmit_msg(adap, &tx_data, false);

	case CEC_MSG_GIVE_PHYSICAL_ADDR:
		/* Do nothing for CEC switches using addr 15 */
		if (devtype == CEC_OP_PRIM_DEVTYPE_SWITCH && dest_laddr == 15)
			return 0;
		cec_msg_report_physical_addr(&tx_data.msg, adap->phys_addr, devtype);
		return cec_transmit_msg(adap, &tx_data, false);

	case CEC_MSG_REPORT_PHYSICAL_ADDR:
		adap->phys_addrs[init_laddr] =
			(msg->msg[2] << 8) | msg->msg[3];
		dprintk(1, "Reported physical address %04x for logical address %d\n",
			adap->phys_addrs[init_laddr], init_laddr);
		break;

	case CEC_MSG_GIVE_DEVICE_VENDOR_ID:
		if (!(adap->capabilities & CEC_CAP_VENDOR_ID) ||
		    adap->vendor_id == CEC_VENDOR_ID_NONE)
			return cec_feature_abort(adap, msg);
		cec_msg_device_vendor_id(&tx_data.msg, adap->vendor_id);
		return cec_transmit_msg(adap, &tx_data, false);

	case CEC_MSG_ABORT:
		/* Do nothing for CEC switches */
		if (devtype == CEC_OP_PRIM_DEVTYPE_SWITCH)
			return 0;
		return cec_feature_refused(adap, msg);

	case CEC_MSG_GIVE_DEVICE_POWER_STATUS:
		/* Do nothing for CEC switches */
		if (devtype == CEC_OP_PRIM_DEVTYPE_SWITCH)
			return 0;
		cec_msg_report_power_status(&tx_data.msg, adap->pwr_state);
		return cec_transmit_msg(adap, &tx_data, false);

	case CEC_MSG_GIVE_OSD_NAME: {
		if (adap->osd_name[0] == 0)
			return cec_feature_abort(adap, msg);
		cec_msg_set_osd_name(&tx_data.msg, adap->osd_name);
		return cec_transmit_msg(adap, &tx_data, false);
	}

	case CEC_MSG_USER_CONTROL_PRESSED:
		if (!(adap->capabilities & CEC_CAP_RC))
			return cec_feature_abort(adap, msg);

		switch (msg->msg[2]) {
		/* Play function, this message can have variable length
		 * depending on the specific play function that is used.
		 */
		case 0x60:
			if (msg->len == 3)
				rc_keydown(adap->rc, RC_TYPE_CEC,
					   msg->msg[2] << 8 | msg->msg[3], 0);
			else
				rc_keydown(adap->rc, RC_TYPE_CEC, msg->msg[2],
					   0);
			break;
		/* Other function messages that are not handled.
		 * Currently the RC framework does not allow to supply an
		 * additional parameter to a keypress. These "keys" contain
		 * other information such as channel number, an input number
		 * etc.
		 * For the time being these messages are not processed by the
		 * framework and are simply forwarded to the user space.
		 */
		case 0x67: case 0x68: case 0x69: case 0x6a:
			break;
		default:
			rc_keydown(adap->rc, RC_TYPE_CEC, msg->msg[2], 0);
		}
		break;
	case CEC_MSG_USER_CONTROL_RELEASED:
		if (!(adap->capabilities & CEC_CAP_RC))
			return cec_feature_abort(adap, msg);
		rc_keyup(adap->rc);
		return 0;

	case CEC_MSG_GIVE_FEATURES:
		if (adap->cec_version < CEC_OP_CEC_VERSION_2_0)
			break;
		return cec_report_features(adap, la_idx);

	case CEC_MSG_REQUEST_ARC_INITIATION:
		if (!(adap->capabilities & CEC_CAP_ARC))
			return cec_feature_abort(adap, msg);
		if (adap->is_sink ||
		    !cec_are_adjacent(adap, dest_laddr, init_laddr))
			return cec_feature_refused(adap, msg);
		cec_msg_initiate_arc(&tx_data.msg, false);
		return cec_transmit_msg(adap, &tx_data, false);

	case CEC_MSG_REQUEST_ARC_TERMINATION:
		if (!(adap->capabilities & CEC_CAP_ARC))
			return cec_feature_abort(adap, msg);
		if (adap->is_sink ||
		    !cec_are_adjacent(adap, dest_laddr, init_laddr))
			return cec_feature_refused(adap, msg);
		cec_msg_terminate_arc(&tx_data.msg, false);
		return cec_transmit_msg(adap, &tx_data, false);

	case CEC_MSG_INITIATE_ARC:
		if (!(adap->capabilities & CEC_CAP_ARC))
			return cec_feature_abort(adap, msg);
		if (!adap->is_sink ||
		    !cec_are_adjacent(adap, dest_laddr, init_laddr))
			return cec_feature_refused(adap, msg);
		if (adap->sink_initiate_arc(adap))
			return 0;
		cec_msg_report_arc_initiated(&tx_data.msg);
		return cec_transmit_msg(adap, &tx_data, false);

	case CEC_MSG_TERMINATE_ARC:
		if (!(adap->capabilities & CEC_CAP_ARC))
			return cec_feature_abort(adap, msg);
		if (!adap->is_sink ||
		    !cec_are_adjacent(adap, dest_laddr, init_laddr))
			return cec_feature_refused(adap, msg);
		if (adap->sink_terminate_arc(adap))
			return 0;
		cec_msg_report_arc_terminated(&tx_data.msg);
		return cec_transmit_msg(adap, &tx_data, false);

	case CEC_MSG_REPORT_ARC_INITIATED:
		if (!(adap->capabilities & CEC_CAP_ARC))
			return cec_feature_abort(adap, msg);
		if (adap->is_sink ||
		    !cec_are_adjacent(adap, dest_laddr, init_laddr))
			return cec_feature_refused(adap, msg);
		adap->source_arc_initiated(adap);
		return 0;

	case CEC_MSG_REPORT_ARC_TERMINATED:
		if (!(adap->capabilities & CEC_CAP_ARC))
			return cec_feature_abort(adap, msg);
		if (adap->is_sink ||
		    !cec_are_adjacent(adap, dest_laddr, init_laddr))
			return cec_feature_refused(adap, msg);
		adap->source_arc_terminated(adap);
		return 0;

	case CEC_MSG_CDC_MESSAGE: {
		unsigned shift;
		unsigned input_port;

		if (!(adap->capabilities & CEC_CAP_CDC))
			return 0;

		switch (msg->msg[4]) {
		case CEC_MSG_CDC_HPD_REPORT_STATE:
			if (adap->is_sink)
				return 0;
			break;
		case CEC_MSG_CDC_HPD_SET_STATE:
			if (!adap->is_sink)
				return 0;
			break;
		default:
			return 0;
		}

		cdc_phys_addr = (msg->msg[2] << 8) | msg->msg[3];
		input_port = msg->msg[5] >> 4;
		for (shift = 0; shift < 16; shift += 4) {
			if (cdc_phys_addr & (0xf000 >> shift))
				continue;
			cdc_phys_addr |= input_port << (12 - shift);
			break;
		}
		if (cdc_phys_addr != adap->phys_addr)
			return 0;

		tx_data.msg.len = 6;
		/* broadcast reply */
		tx_msg[0] = (adap->log_addr[0] << 4) | 0xf;
		tx_msg[1] = CEC_MSG_CDC_MESSAGE;
		tx_msg[2] = adap->phys_addr >> 8;
		tx_msg[3] = adap->phys_addr & 0xff;
		tx_msg[4] = CEC_MSG_CDC_HPD_REPORT_STATE;
		tx_msg[5] = ((msg->msg[5] & 0xf) << 4) |
			adap->source_cdc_hpd(adap, msg->msg[5] & 0xf);
		return cec_transmit_msg(adap, &tx_data, false);
	}
	}

	if (is_directed && !(adap->flags[la_idx] & CEC_LOG_ADDRS_FL_HANDLE_MSGS))
		return cec_feature_abort(adap, msg);

skip_processing:
	if ((adap->capabilities & CEC_CAP_RECEIVE) == 0) {
		if (is_directed)
			return cec_feature_abort(adap, msg);
		return 0;
	}
	mutex_lock(&adap->lock);
	idx = (adap->rx_qstart + adap->rx_qcount) % CEC_RX_QUEUE_SZ;
	if (adap->rx_qcount == CEC_RX_QUEUE_SZ) {
		res = -EBUSY;
	} else {
		adap->rx_queue[idx] = *msg;
		adap->rx_qcount++;
		wake_up_interruptible(&adap->waitq);
	}
	mutex_unlock(&adap->lock);
	return res;
}

int cec_receive_msg(struct cec_adapter *adap, struct cec_msg *msg, bool block)
{
	int res;

	do {
		mutex_lock(&adap->lock);
		if (adap->rx_qcount) {
			*msg = adap->rx_queue[adap->rx_qstart];
			adap->rx_qstart = (adap->rx_qstart + 1) %
					  CEC_RX_QUEUE_SZ;
			adap->rx_qcount--;
			res = 0;
		} else {
			res = -EAGAIN;
		}
		mutex_unlock(&adap->lock);
		if (!block || !res)
			break;
		if (msg->timeout) {
			res = wait_event_interruptible_timeout(adap->waitq,
				adap->rx_qcount,
				msecs_to_jiffies(msg->timeout));
			if (res == 0)
				res = -ETIMEDOUT;
			else if (res > 0)
				res = 0;
		} else {
			res = wait_event_interruptible(adap->waitq,
				adap->rx_qcount);
		}
	} while (!res);
	return res;
}
EXPORT_SYMBOL_GPL(cec_receive_msg);

void cec_received_msg(struct cec_adapter *adap, struct cec_msg *msg)
{
	struct cec_data *dst_data = &adap->tx_queue[adap->tx_qstart];
	bool is_cdc_msg = dst_data->msg.msg[1] == CEC_MSG_CDC_MESSAGE;
	bool is_reply = false;

	mutex_lock(&adap->lock);
	msg->ts = ktime_get_ns();
	dprintk(2, "cec_received_msg: %02x %02x\n", msg->msg[0], msg->msg[1]);
	if (msg->len > 1 && adap->state == CEC_ADAP_STATE_WAIT &&
	    cec_msg_initiator(msg) == cec_msg_destination(&dst_data->msg)) {
		struct cec_msg *dst = &dst_data->msg;
		/* TODO: check phys address */
		bool is_valid_reply = is_cdc_msg ?
			msg->msg[1] == CEC_MSG_CDC_MESSAGE &&
			msg->msg[4] == dst->reply :
			msg->msg[1] == dst->reply;

		if (is_valid_reply ||
		    msg->msg[1] == CEC_MSG_FEATURE_ABORT) {
			msg->sequence = dst->sequence;
			*dst = *msg;
			is_reply = true;
			if (msg->msg[1] == CEC_MSG_FEATURE_ABORT) {
				dst->reply = 0;
				dst->status = CEC_TX_STATUS_FEATURE_ABORT;
			}
			wake_up_interruptible(&adap->kthread_waitq);
		}
	}
	mutex_unlock(&adap->lock);
	if (!is_reply || (is_reply && !dst_data->blocking))
		adap->recv_notifier(adap, msg);
	if (is_reply && !dst_data->blocking)
		cec_post_event(adap, CEC_EVENT_GOT_REPLY, msg->sequence);
}
EXPORT_SYMBOL_GPL(cec_received_msg);

void cec_post_event(struct cec_adapter *adap, u32 event, u32 sequence)
{
	unsigned idx;

	mutex_lock(&adap->lock);
	if (adap->ev_qcount == CEC_EV_QUEUE_SZ) {
		/* Drop oldest event */
		adap->ev_qstart = (adap->ev_qstart + 1) % CEC_EV_QUEUE_SZ;
		adap->ev_qcount--;
	}

	idx = (adap->ev_qstart + adap->ev_qcount) % CEC_EV_QUEUE_SZ;

	adap->ev_queue[idx].event = event;
	adap->ev_queue[idx].sequence = sequence;
	adap->ev_queue[idx].ts = ktime_get_ns();

	adap->ev_qcount++;
	mutex_unlock(&adap->lock);
}
EXPORT_SYMBOL_GPL(cec_post_event);

int cec_enable(struct cec_adapter *adap, bool enable)
{
	int ret;

	mutex_lock(&adap->lock);
	ret = adap->adap_enable(adap, enable);
	if (ret) {
		mutex_unlock(&adap->lock);
		return ret;
	}
	if (!enable) {
		adap->state = CEC_ADAP_STATE_DISABLED;
		adap->tx_qcount = 0;
		adap->rx_qcount = 0;
		adap->ev_qcount = 0;
		adap->num_log_addrs = 0;
		memset(adap->phys_addrs, 0xff, sizeof(adap->phys_addrs));
		wake_up_interruptible(&adap->waitq);
	} else {
		adap->state = CEC_ADAP_STATE_UNCONF;
	}
	mutex_unlock(&adap->lock);
	return 0;
}
EXPORT_SYMBOL_GPL(cec_enable);

struct cec_log_addrs_int {
	struct cec_adapter *adap;
	struct cec_log_addrs log_addrs;
	struct completion c;
	bool free_on_exit;
	int err;
};

static int cec_config_log_addrs(struct cec_adapter *adap,
				struct cec_log_addrs *log_addrs)
{
	static const u8 tv_log_addrs[] = {
		0, CEC_LOG_ADDR_INVALID
	};
	static const u8 record_log_addrs[] = {
		1, 2, 9, 12, 13, CEC_LOG_ADDR_INVALID
	};
	static const u8 tuner_log_addrs[] = {
		3, 6, 7, 10, 12, 13, CEC_LOG_ADDR_INVALID
	};
	static const u8 playback_log_addrs[] = {
		4, 8, 11, 12, 13, CEC_LOG_ADDR_INVALID
	};
	static const u8 audiosystem_log_addrs[] = {
		5, 12, 13, CEC_LOG_ADDR_INVALID
	};
	static const u8 specific_use_log_addrs[] = {
		14, 12, 13, CEC_LOG_ADDR_INVALID
	};
	static const u8 unregistered_log_addrs[] = {
		CEC_LOG_ADDR_INVALID
	};
	static const u8 *type2addrs[7] = {
		[CEC_LOG_ADDR_TYPE_TV] = tv_log_addrs,
		[CEC_LOG_ADDR_TYPE_RECORD] = record_log_addrs,
		[CEC_LOG_ADDR_TYPE_TUNER] = tuner_log_addrs,
		[CEC_LOG_ADDR_TYPE_PLAYBACK] = playback_log_addrs,
		[CEC_LOG_ADDR_TYPE_AUDIOSYSTEM] = audiosystem_log_addrs,
		[CEC_LOG_ADDR_TYPE_SPECIFIC] = specific_use_log_addrs,
		[CEC_LOG_ADDR_TYPE_UNREGISTERED] = unregistered_log_addrs,
	};
	struct cec_data data;
	u32 claimed_addrs = 0;
	int i, j;
	int err;

	if (adap->phys_addr) {
		/* The TV functionality can only map to physical address 0.
		   For any other address, try the Specific functionality
		   instead as per the spec. */
		for (i = 0; i < log_addrs->num_log_addrs; i++)
			if (log_addrs->log_addr_type[i] == CEC_LOG_ADDR_TYPE_TV)
				log_addrs->log_addr_type[i] =
						CEC_LOG_ADDR_TYPE_SPECIFIC;
	}

	dprintk(2, "physical address: %x.%x.%x.%x, claim %d logical addresses\n",
			cec_phys_addr_exp(adap->phys_addr),
			log_addrs->num_log_addrs);
	adap->num_log_addrs = 0;
	adap->state = CEC_ADAP_STATE_IDLE;
	strlcpy(adap->osd_name, log_addrs->osd_name, sizeof(adap->osd_name));

	/* TODO: remember last used logical addr type to achieve
	   faster logical address polling by trying that one first.
	 */
	for (i = 0; i < log_addrs->num_log_addrs; i++) {
		const u8 *la_list = type2addrs[log_addrs->log_addr_type[i]];

		if (kthread_should_stop())
			return -EINTR;

		for (j = 0; la_list[j] != CEC_LOG_ADDR_INVALID; j++) {
			u8 log_addr = la_list[j];

			if (claimed_addrs & (1 << log_addr))
				continue;

			/* Send polling message */
			data.msg.len = 1;
			data.msg.msg[0] = 0xf0 | log_addr;
			data.msg.reply = 0;
			err = cec_transmit_msg(adap, &data, true);
			if (err)
				return err;
			if (data.msg.status == CEC_TX_STATUS_RETRY_TIMEOUT) {
				unsigned idx = adap->num_log_addrs++;

				/* Message not acknowledged, so this logical
				   address is free to use. */
				claimed_addrs |= 1 << log_addr;
				adap->log_addr[idx] = log_addr;
				log_addrs->log_addr[i] = log_addr;
				adap->flags[idx] = log_addrs->flags[i] &
					CEC_LOG_ADDRS_FL_HANDLE_MSGS;
				adap->log_addr_type[idx] =
					log_addrs->log_addr_type[i];
				adap->prim_device[idx] =
					log_addrs->primary_device_type[i];
				adap->all_device_types[idx] =
					log_addrs->all_device_types[i];
				adap->phys_addrs[log_addr] = adap->phys_addr;
				memcpy(adap->features[idx], log_addrs->features[i],
				       sizeof(adap->features[idx]));
				err = adap->adap_log_addr(adap, log_addr);
				dprintk(2, "claim addr %d (%d)\n", log_addr,
							adap->prim_device[idx]);
				if (err)
					return err;
				/*
				 * Report Features must come first according
				 * to CEC 2.0
				 */
				cec_report_features(adap, idx);
				cec_report_phys_addr(adap, idx);
				if (adap->claimed_log_addr)
					adap->claimed_log_addr(adap, idx);
				break;
			}
		}
	}
	if (adap->num_log_addrs == 0) {
		if (log_addrs->num_log_addrs > 1)
			dprintk(2, "could not claim last %d addresses\n",
				log_addrs->num_log_addrs - 1);
		adap->log_addr[0] = 15;
		adap->log_addr_type[0] = CEC_LOG_ADDR_TYPE_UNREGISTERED;
		adap->prim_device[0] = CEC_OP_PRIM_DEVTYPE_SWITCH;
		adap->flags[0] = 0;
		adap->all_device_types[0] = CEC_OP_ALL_DEVTYPE_SWITCH;
		err = adap->adap_log_addr(adap, 15);
		dprintk(2, "claim addr %d (%d)\n", 15, adap->prim_device[0]);
		if (err)
			return err;
		adap->num_log_addrs = 1;
		/* TODO: do we need to do this for an unregistered device? */
		cec_report_phys_addr(adap, 0);
		if (adap->claimed_log_addr)
			adap->claimed_log_addr(adap, 0);
	}
	return 0;
}

static int cec_config_thread_func(void *arg)
{
	struct cec_log_addrs_int *cla_int = arg;
	int err;

	cla_int->err = err = cec_config_log_addrs(cla_int->adap,
						  &cla_int->log_addrs);
	cla_int->adap->kthread_config = NULL;
	if (cla_int->free_on_exit)
		kfree(cla_int);
	else
		complete(&cla_int->c);

	cec_post_event(cla_int->adap, CEC_EVENT_READY, 0);
	return err;
}

int cec_claim_log_addrs(struct cec_adapter *adap,
			struct cec_log_addrs *log_addrs, bool block)
{
	struct cec_log_addrs_int *cla_int;
	int i;

	if (adap->state == CEC_ADAP_STATE_DISABLED)
		return -ENONET;

	if (log_addrs->num_log_addrs > CEC_MAX_LOG_ADDRS) {
		dprintk(1, "num_log_addrs > %d\n", CEC_MAX_LOG_ADDRS);
		return -EINVAL;
	}
	if (log_addrs->num_log_addrs == 0) {
		adap->num_log_addrs = 0;
		adap->tx_qcount = 0;
		adap->rx_qcount = 0;
		adap->ev_qcount = 0;
		adap->state = CEC_ADAP_STATE_UNCONF;
		wake_up_interruptible(&adap->waitq);
		return 0;
	}
	if (log_addrs->cec_version != CEC_OP_CEC_VERSION_1_4 &&
	    log_addrs->cec_version != CEC_OP_CEC_VERSION_2_0) {
		dprintk(1, "unsupported CEC version\n");
		return -EINVAL;
	}
	if (log_addrs->num_log_addrs > 1)
		for (i = 0; i < log_addrs->num_log_addrs; i++)
			if (log_addrs->log_addr_type[i] ==
					CEC_LOG_ADDR_TYPE_UNREGISTERED) {
				dprintk(1, "can't claim unregistered logical address\n");
				return -EINVAL;
			}
	for (i = 0; i < log_addrs->num_log_addrs; i++) {
		u8 *features = log_addrs->features[i];
		bool op_is_dev_features = false;

		if (log_addrs->primary_device_type[i] >
					CEC_OP_PRIM_DEVTYPE_PROCESSOR) {
			dprintk(1, "unknown primary device type\n");
			return -EINVAL;
		}
		if (log_addrs->primary_device_type[i] == 2) {
			dprintk(1, "invalid primary device type\n");
			return -EINVAL;
		}
		if (log_addrs->log_addr_type[i] > CEC_LOG_ADDR_TYPE_UNREGISTERED) {
			dprintk(1, "unknown logical address type\n");
			return -EINVAL;
		}
		if (log_addrs->cec_version < CEC_OP_CEC_VERSION_2_0)
			continue;

		for (i = 0; i < sizeof(adap->features[0]); i++) {
			if ((features[i] & 0x80) == 0) {
				if (op_is_dev_features)
					break;
				op_is_dev_features = true;
			}
		}
		if (!op_is_dev_features || i == sizeof(adap->features[0])) {
			dprintk(1, "malformed features\n");
			return -EINVAL;
		}
	}

	/* For phys addr 0xffff only the Unregistered functionality is
	   allowed. */
	if (adap->phys_addr == 0xffff &&
	    (log_addrs->num_log_addrs > 1 ||
	     log_addrs->log_addr_type[0] != CEC_LOG_ADDR_TYPE_UNREGISTERED)) {
		dprintk(1, "physical addr 0xffff only allows unregistered logical address\n");
		return -EINVAL;
	}

	cla_int = kzalloc(sizeof(*cla_int), GFP_KERNEL);
	if (cla_int == NULL)
		return -ENOMEM;
	init_completion(&cla_int->c);
	cla_int->free_on_exit = !block;
	cla_int->adap = adap;
	cla_int->log_addrs = *log_addrs;
	adap->kthread_config = kthread_run(cec_config_thread_func, cla_int,
							"cec_log_addrs");
	if (block) {
		wait_for_completion(&cla_int->c);
		*log_addrs = cla_int->log_addrs;
		kfree(cla_int);
	}
	return 0;
}
EXPORT_SYMBOL_GPL(cec_claim_log_addrs);

u8 cec_sink_cdc_hpd(struct cec_adapter *adap, u8 input_port, u8 cdc_hpd_state)
{
	struct cec_data data;
	struct cec_msg *msg = &data.msg;
	int err;

	if (adap->state <= CEC_ADAP_STATE_UNCONF)
		return CEC_OP_HPD_ERROR_INITIATOR_WRONG_STATE;

	msg->len = 6;
	msg->reply = CEC_MSG_CDC_HPD_REPORT_STATE;
	msg->msg[0] = (adap->log_addr[0] << 4) | 0xf;
	msg->msg[1] = CEC_MSG_CDC_MESSAGE;
	msg->msg[2] = adap->phys_addr >> 8;
	msg->msg[3] = adap->phys_addr & 0xff;
	msg->msg[4] = CEC_MSG_CDC_HPD_SET_STATE;
	msg->msg[5] = (input_port << 4) | cdc_hpd_state;
	err = cec_transmit_msg(adap, &data, false);
	if (err)
		return err;
	return 0;
}
EXPORT_SYMBOL_GPL(cec_sink_cdc_hpd);

static unsigned int cec_poll(struct file *filp,
			       struct poll_table_struct *poll)
{
	struct cec_devnode *cecdev = cec_devnode_data(filp);
	struct cec_adapter *adap = to_cec_adapter(cecdev);
	unsigned res = 0;

	if (!cec_devnode_is_registered(cecdev))
		return POLLERR | POLLHUP;
	mutex_lock(&adap->lock);
	if (adap->tx_qcount < CEC_TX_QUEUE_SZ)
		res |= POLLOUT | POLLWRNORM;
	if (adap->rx_qcount)
		res |= POLLIN | POLLRDNORM;
	if (adap->ev_qcount)
		res |= POLLPRI;
	poll_wait(filp, &adap->waitq, poll);
	mutex_unlock(&adap->lock);
	return res;
}

static long cec_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct cec_devnode *cecdev = cec_devnode_data(filp);
	struct cec_adapter *adap = to_cec_adapter(cecdev);
	void __user *parg = (void __user *)arg;
	int err;

	if (!cec_devnode_is_registered(cecdev))
		return -EIO;

	switch (cmd) {
	case CEC_G_CAPS: {
		struct cec_caps caps;

		caps.available_log_addrs = adap->available_log_addrs;
		caps.capabilities = adap->capabilities;
		memset(caps.reserved, 0, sizeof(caps.reserved));
		if (copy_to_user(parg, &caps, sizeof(caps)))
			return -EFAULT;
		break;
	}

	case CEC_TRANSMIT: {
		struct cec_data data;

		if (!(adap->capabilities & CEC_CAP_TRANSMIT))
			return -ENOTTY;
		if (copy_from_user(&data.msg, parg, sizeof(data.msg)))
			return -EFAULT;
		memset(data.msg.reserved, 0, sizeof(data.msg.reserved));
		if (adap->state <= CEC_ADAP_STATE_UNCONF)
			return -ENONET;

		err = cec_transmit_msg(adap, &data,
						!(filp->f_flags & O_NONBLOCK));
		if (err)
			return err;
		if (copy_to_user(parg, &data.msg, sizeof(data.msg)))
			return -EFAULT;
		break;
	}

	case CEC_RECEIVE: {
		struct cec_data data;

		if (!(adap->capabilities & CEC_CAP_RECEIVE))
			return -ENOTTY;
		if (copy_from_user(&data.msg, parg, sizeof(data.msg)))
			return -EFAULT;
		memset(data.msg.reserved, 0, sizeof(data.msg.reserved));
		if (adap->state <= CEC_ADAP_STATE_UNCONF)
			return -ENONET;

		err = cec_receive_msg(adap, &data.msg,
						!(filp->f_flags & O_NONBLOCK));
		if (err)
			return err;
		if (copy_to_user(parg, &data.msg, sizeof(data.msg)))
			return -EFAULT;
		break;
	}

	case CEC_G_EVENT: {
		struct cec_event ev;

		mutex_lock(&adap->lock);
		err = -EAGAIN;
		if (adap->state <= CEC_ADAP_STATE_UNCONF) {
			err = -ENONET;
		} else if (adap->ev_qcount) {
			err = 0;
			ev = adap->ev_queue[adap->ev_qstart];
			adap->ev_qstart = (adap->ev_qstart + 1) % CEC_EV_QUEUE_SZ;
			adap->ev_qcount--;
		}
		mutex_unlock(&adap->lock);
		if (err)
			return err;
		if (copy_to_user((void __user *)arg, &ev, sizeof(ev)))
			return -EFAULT;
		break;
	}

	case CEC_G_ADAP_STATE: {
		u32 state = adap->state != CEC_ADAP_STATE_DISABLED;

		if (copy_to_user(parg, &state, sizeof(state)))
			return -EFAULT;
		break;
	}

	case CEC_S_ADAP_STATE: {
		u32 state;

		if (!(adap->capabilities & CEC_CAP_STATE))
			return -ENOTTY;
		if (copy_from_user(&state, parg, sizeof(state)))
			return -EFAULT;
		if (!state && adap->state == CEC_ADAP_STATE_DISABLED)
			return 0;
		if (state && adap->state != CEC_ADAP_STATE_DISABLED)
			return 0;
		cec_enable(adap, !!state);
		break;
	}

	case CEC_G_ADAP_PHYS_ADDR:
		if (copy_to_user(parg, &adap->phys_addr,
						sizeof(adap->phys_addr)))
			return -EFAULT;
		break;

	case CEC_S_ADAP_PHYS_ADDR: {
		u16 phys_addr;

		if (!(adap->capabilities & CEC_CAP_PHYS_ADDR))
			return -ENOTTY;
		if (copy_from_user(&phys_addr, parg, sizeof(phys_addr)))
			return -EFAULT;
		if (adap->phys_addr == phys_addr)
			return 0;
		if (adap->state > CEC_ADAP_STATE_UNCONF)
			return -EBUSY;
		adap->phys_addr = phys_addr;
		break;
	}

	case CEC_S_ADAP_LOG_ADDRS: {
		struct cec_log_addrs log_addrs;

		if (!(adap->capabilities & CEC_CAP_LOG_ADDRS))
			return -ENOTTY;
		if (copy_from_user(&log_addrs, parg, sizeof(log_addrs)))
			return -EFAULT;
		memset(log_addrs.reserved, 0, sizeof(log_addrs.reserved));
		if (log_addrs.num_log_addrs &&
		    adap->state > CEC_ADAP_STATE_UNCONF)
			return -EBUSY;

		err = cec_claim_log_addrs(adap, &log_addrs,
					!(filp->f_flags & O_NONBLOCK));
		if (err)
			return err;

		if (filp->f_flags & O_NONBLOCK) {
			if (copy_to_user(parg, &log_addrs, sizeof(log_addrs)))
				return -EFAULT;
			break;
		}

		/* fall through */
	}

	case CEC_G_ADAP_LOG_ADDRS: {
		struct cec_log_addrs log_addrs = { adap->cec_version };
		unsigned i;

		log_addrs.num_log_addrs = adap->num_log_addrs;
		strlcpy(log_addrs.osd_name, adap->osd_name,
			sizeof(log_addrs.osd_name));
		for (i = 0; i < adap->num_log_addrs; i++) {
			log_addrs.primary_device_type[i] = adap->prim_device[i];
			log_addrs.log_addr_type[i] = adap->log_addr_type[i];
			log_addrs.log_addr[i] = adap->log_addr[i];
			log_addrs.flags[i] = adap->flags[i];
			log_addrs.all_device_types[i] = adap->all_device_types[i];
			memcpy(log_addrs.features[i], adap->features[i],
			       sizeof(log_addrs.features[i]));
		}

		if (copy_to_user(parg, &log_addrs, sizeof(log_addrs)))
			return -EFAULT;
		break;
	}

	case CEC_G_VENDOR_ID:
		if (copy_to_user(parg, &adap->vendor_id,
						sizeof(adap->vendor_id)))
			return -EFAULT;
		break;

	case CEC_S_VENDOR_ID: {
		u32 vendor_id;

		if (!(adap->capabilities & CEC_CAP_VENDOR_ID))
			return -ENOTTY;
		if (copy_from_user(&vendor_id, parg, sizeof(vendor_id)))
			return -EFAULT;
		/* Vendor ID is a 24 bit number, so check if the value is
		 * within the correct range. */
		if (vendor_id != CEC_VENDOR_ID_NONE &&
		    (vendor_id & 0xff000000) != 0)
			return -EINVAL;
		if (adap->vendor_id == vendor_id)
			return 0;
		if (adap->state > CEC_ADAP_STATE_UNCONF)
			return -EBUSY;
		adap->vendor_id = vendor_id;
		break;
	}

	case CEC_G_PASSTHROUGH: {
		u32 state = adap->passthrough;

		if (copy_to_user(parg, &state, sizeof(state)))
			return -EFAULT;
		break;
	}

	case CEC_S_PASSTHROUGH: {
		u32 state;

		if (!(adap->capabilities & CEC_CAP_PASSTHROUGH))
			return -ENOTTY;
		if (copy_from_user(&state, parg, sizeof(state)))
			return -EFAULT;
		if (state == CEC_PASSTHROUGH_DISABLED)
			adap->passthrough = state;
		else if (state == CEC_PASSTHROUGH_ENABLED)
			adap->passthrough = state;
		else
			return -EINVAL;
		break;
	}

	default:
		return -ENOTTY;
	}
	return 0;
}

/* Override for the open function */
static int cec_open(struct inode *inode, struct file *filp)
{
	struct cec_devnode *cecdev;

	/* Check if the cec device is available. This needs to be done with
	 * the cec_devnode_lock held to prevent an open/unregister race:
	 * without the lock, the device could be unregistered and freed between
	 * the cec_devnode_is_registered() and get_device() calls, leading to
	 * a crash.
	 */
	mutex_lock(&cec_devnode_lock);
	cecdev = container_of(inode->i_cdev, struct cec_devnode, cdev);
	/* return ENXIO if the cec device has been removed
	   already or if it is not registered anymore. */
	if (!cec_devnode_is_registered(cecdev)) {
		mutex_unlock(&cec_devnode_lock);
		return -ENXIO;
	}
	/* and increase the device refcount */
	get_device(&cecdev->dev);
	mutex_unlock(&cec_devnode_lock);

	filp->private_data = cecdev;

	return 0;
}

/* Override for the release function */
static int cec_release(struct inode *inode, struct file *filp)
{
	struct cec_devnode *cecdev = cec_devnode_data(filp);
	int ret = 0;

	/* decrease the refcount unconditionally since the release()
	   return value is ignored. */
	put_device(&cecdev->dev);
	filp->private_data = NULL;
	return ret;
}

static const struct file_operations cec_devnode_fops = {
	.owner = THIS_MODULE,
	.open = cec_open,
	.unlocked_ioctl = cec_ioctl,
	.release = cec_release,
	.poll = cec_poll,
	.llseek = no_llseek,
};

/**
 * cec_devnode_register - register a cec device node
 * @cecdev: cec device node structure we want to register
 *
 * The registration code assigns minor numbers and registers the new device node
 * with the kernel. An error is returned if no free minor number can be found,
 * or if the registration of the device node fails.
 *
 * Zero is returned on success.
 *
 * Note that if the cec_devnode_register call fails, the release() callback of
 * the cec_devnode structure is *not* called, so the caller is responsible for
 * freeing any data.
 */
static int __must_check cec_devnode_register(struct cec_devnode *cecdev,
		struct module *owner)
{
	int minor;
	int ret;

	/* Part 1: Find a free minor number */
	mutex_lock(&cec_devnode_lock);
	minor = find_next_zero_bit(cec_devnode_nums, CEC_NUM_DEVICES, 0);
	if (minor == CEC_NUM_DEVICES) {
		mutex_unlock(&cec_devnode_lock);
		pr_err("could not get a free minor\n");
		return -ENFILE;
	}

	set_bit(minor, cec_devnode_nums);
	mutex_unlock(&cec_devnode_lock);

	cecdev->minor = minor;

	/* Part 2: Initialize and register the character device */
	cdev_init(&cecdev->cdev, &cec_devnode_fops);
	cecdev->cdev.owner = owner;

	ret = cdev_add(&cecdev->cdev, MKDEV(MAJOR(cec_dev_t), cecdev->minor),
									1);
	if (ret < 0) {
		pr_err("%s: cdev_add failed\n", __func__);
		goto error;
	}

	/* Part 3: Register the cec device */
	cecdev->dev.bus = &cec_bus_type;
	cecdev->dev.devt = MKDEV(MAJOR(cec_dev_t), cecdev->minor);
	cecdev->dev.release = cec_devnode_release;
	if (cecdev->parent)
		cecdev->dev.parent = cecdev->parent;
	dev_set_name(&cecdev->dev, "cec%d", cecdev->minor);
	ret = device_register(&cecdev->dev);
	if (ret < 0) {
		pr_err("%s: device_register failed\n", __func__);
		goto error;
	}

	/* Part 4: Activate this minor. The char device can now be used. */
	set_bit(CEC_FLAG_REGISTERED, &cecdev->flags);

	return 0;

error:
	cdev_del(&cecdev->cdev);
	clear_bit(cecdev->minor, cec_devnode_nums);
	return ret;
}

/**
 * cec_devnode_unregister - unregister a cec device node
 * @cecdev: the device node to unregister
 *
 * This unregisters the passed device. Future open calls will be met with
 * errors.
 *
 * This function can safely be called if the device node has never been
 * registered or has already been unregistered.
 */
static void cec_devnode_unregister(struct cec_devnode *cecdev)
{
	/* Check if cecdev was ever registered at all */
	if (!cec_devnode_is_registered(cecdev))
		return;

	mutex_lock(&cec_devnode_lock);
	clear_bit(CEC_FLAG_REGISTERED, &cecdev->flags);
	mutex_unlock(&cec_devnode_lock);
	device_unregister(&cecdev->dev);
}

int cec_create_adapter(struct cec_adapter *adap, const char *name, u32 caps,
		       bool is_sink, struct module *owner, struct device *parent)
{
	int res = 0;

	adap->owner = owner;
	WARN_ON(!owner);
	adap->devnode.parent = parent;
	WARN_ON(!parent);
	adap->state = CEC_ADAP_STATE_DISABLED;
	adap->name = name;
	adap->is_sink = is_sink;
	adap->phys_addr = 0xffff;
	adap->capabilities = caps;
	adap->cec_version = CEC_OP_CEC_VERSION_2_0;
	adap->vendor_id = CEC_VENDOR_ID_NONE;
	adap->available_log_addrs = 1;
	adap->sequence = 0;
	memset(adap->phys_addrs, 0xff, sizeof(adap->phys_addrs));
	mutex_init(&adap->lock);
	adap->kthread = kthread_run(cec_thread_func, adap, name);
	init_waitqueue_head(&adap->kthread_waitq);
	init_waitqueue_head(&adap->waitq);
	if (IS_ERR(adap->kthread)) {
		pr_err("cec-%s: kernel_thread() failed\n", name);
		return PTR_ERR(adap->kthread);
	}
	if (caps) {
		res = cec_devnode_register(&adap->devnode, adap->owner);
		if (res) {
			kthread_stop(adap->kthread);
			return res;
		}
	}
	adap->recv_notifier = cec_receive_notify;

	if (!(caps & CEC_CAP_RC))
		return 0;

	/* Prepare the RC input device */
	adap->rc = rc_allocate_device();
	if (!adap->rc) {
		pr_err("cec-%s: failed to allocate memory for rc_dev\n", name);
		cec_devnode_unregister(&adap->devnode);
		kthread_stop(adap->kthread);
		return -ENOMEM;
	}

	snprintf(adap->input_name, sizeof(adap->input_name), "RC for %s", name);
	snprintf(adap->input_phys, sizeof(adap->input_phys), "%s/input0", name);
	strncpy(adap->input_drv, name, sizeof(adap->input_drv));

	adap->rc->input_name = adap->input_name;
	adap->rc->input_phys = adap->input_phys;
	adap->rc->input_id.bustype = BUS_CEC;
	adap->rc->input_id.vendor = 0;
	adap->rc->input_id.product = 0;
	adap->rc->input_id.version = 1;
	adap->rc->dev.parent = adap->devnode.parent;
	adap->rc->driver_name = adap->input_drv;
	adap->rc->driver_type = RC_DRIVER_CEC;
	adap->rc->allowed_protocols = RC_BIT_CEC;
	adap->rc->priv = adap;
	adap->rc->map_name = RC_MAP_CEC;
	adap->rc->timeout = MS_TO_NS(100);

	res = rc_register_device(adap->rc);

	if (res) {
		pr_err("cec-%s: failed to prepare input device\n", name);
		cec_devnode_unregister(&adap->devnode);
		rc_free_device(adap->rc);
		kthread_stop(adap->kthread);
	}

	return res;
}
EXPORT_SYMBOL_GPL(cec_create_adapter);

void cec_delete_adapter(struct cec_adapter *adap)
{
	if (adap->kthread == NULL)
		return;
	kthread_stop(adap->kthread);
	if (adap->kthread_config)
		kthread_stop(adap->kthread_config);
	if (adap->state != CEC_ADAP_STATE_DISABLED)
		cec_enable(adap, false);
	rc_unregister_device(adap->rc);
	if (cec_devnode_is_registered(&adap->devnode))
		cec_devnode_unregister(&adap->devnode);
}
EXPORT_SYMBOL_GPL(cec_delete_adapter);

/*
 *	Initialise cec for linux
 */
static int __init cec_devnode_init(void)
{
	int ret;

	pr_info("Linux cec interface: v0.10\n");
	ret = alloc_chrdev_region(&cec_dev_t, 0, CEC_NUM_DEVICES,
				  CEC_NAME);
	if (ret < 0) {
		pr_warn("cec: unable to allocate major\n");
		return ret;
	}

	ret = bus_register(&cec_bus_type);
	if (ret < 0) {
		unregister_chrdev_region(cec_dev_t, CEC_NUM_DEVICES);
		pr_warn("cec: bus_register failed\n");
		return -EIO;
	}

	return 0;
}

static void __exit cec_devnode_exit(void)
{
	bus_unregister(&cec_bus_type);
	unregister_chrdev_region(cec_dev_t, CEC_NUM_DEVICES);
}

subsys_initcall(cec_devnode_init);
module_exit(cec_devnode_exit)

MODULE_AUTHOR("Hans Verkuil <hans.verkuil@cisco.com>");
MODULE_DESCRIPTION("Device node registration for cec drivers");
MODULE_LICENSE("GPL");
