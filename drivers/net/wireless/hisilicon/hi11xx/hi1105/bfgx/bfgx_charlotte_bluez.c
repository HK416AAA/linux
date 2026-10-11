// SPDX-License-Identifier: GPL-2.0
/* Native HCI endpoint for the Charlotte MP15 BFGX packet transport. */
#include <linux/kthread.h>
#include <linux/poll.h>
#include <linux/unaligned.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "bfgx_bluez.h"
#include "bfgx_bt.h"
#include "bfgx_core.h"
#include "bfgx_data_parse.h"
#include "bfgx_dev.h"
#include "plat_pm.h"

static void charlotte_bt_rx_reset(struct hisi_bt_dev *bt)
{
	mutex_lock(&bt->rx_mutex);
	kfree_skb(bt->rx_skb);
	bt->rx_skb = NULL;
	mutex_unlock(&bt->rx_mutex);
}

static int charlotte_bt_open(struct hci_dev *hdev)
{
	struct pm_drv_data *pm = hci_get_drvdata(hdev);
	int ret;

	charlotte_bt_rx_reset(&pm->st_bt_dev);
	mutex_lock(&pm_get_top()->host_mutex);
	ret = hw_bfgx_open(pm->ps_core_data, BFGX_BT);
	mutex_unlock(&pm_get_top()->host_mutex);
	/* BFGX returns positive platform error codes, unlike HCI callbacks. */
	return ret > 0 ? -EIO : ret;
}

static int charlotte_bt_close(struct hci_dev *hdev)
{
	struct pm_drv_data *pm = hci_get_drvdata(hdev);
	int ret;

	mutex_lock(&pm_get_top()->host_mutex);
	ret = hw_bfgx_close(pm->ps_core_data, BFGX_BT);
	oal_wake_unlock_force(&pm->bt_wake_lock);
	mutex_unlock(&pm_get_top()->host_mutex);
	charlotte_bt_rx_reset(&pm->st_bt_dev);
	return ret > 0 ? -EIO : ret;
}

static int charlotte_bt_send(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct pm_drv_data *pm = hci_get_drvdata(hdev);
	struct ps_core_s *core = pm->ps_core_data;
	unsigned int len = skb->len;
	u8 type = hci_skb_pkt_type(skb);
	int prepare, ret;

	if (!len || len > BT_TX_MAX_FRAME)
		return -EMSGSIZE;
	if (type != HCI_COMMAND_PKT && type != HCI_ACLDATA_PKT &&
	    type != HCI_SCODATA_PKT && type != HCI_ISODATA_PKT)
		return -EINVAL;
	if (skb_queue_len(&core->tx_high_seq) > TX_HIGH_QUE_MAX_NUM)
		return -ENOBUFS;
	/* Make the vendor header + H4 type explicit; HCI skbs never include
	 * their type byte, and one-byte payloads are valid complete packets.
	 */
	ret = skb_cow_head(skb, sizeof(struct ps_packet_head) + 1);
	if (ret)
		return ret;
	if (skb_tailroom(skb) < sizeof(struct ps_packet_end)) {
		ret = pskb_expand_head(skb, 0, sizeof(struct ps_packet_end), GFP_KERNEL);
		if (ret)
			return ret;
	}
	ret = prepare_to_visit_node(core, &prepare);
	if (ret < 0)
		return ret;
	oal_wake_lock_timeout(&pm->bt_wake_lock, DEFAULT_WAKELOCK_TIMEOUT);
	mod_timer(&pm->bfg_timer, jiffies + msecs_to_jiffies(BT_SLEEP_TIME));
	oal_atomic_inc(&pm->bfg_timer_mod_cnt);
	skb_push(skb, sizeof(struct ps_packet_head) + 1);
	skb_put(skb, sizeof(struct ps_packet_end));
	skb->data[sizeof(struct ps_packet_head)] = type;
	ps_add_packet_head(skb->data, BT_MSG, skb->len);
	hdev->stat.byte_tx += len + 1;
	if (type == HCI_COMMAND_PKT)
		hdev->stat.cmd_tx++;
	else if (type == HCI_ACLDATA_PKT)
		hdev->stat.acl_tx++;
	else if (type == HCI_SCODATA_PKT)
		hdev->stat.sco_tx++;
	ps_skb_enqueue(core, skb, TX_HIGH_QUEUE);
	ps_core_tx_work_add(core);
	core->bfgx_info[BFGX_BT].tx_pkt_num++;
	post_to_visit_node(core, prepare);
	return 0;
}

static int charlotte_bt_header_len(u8 type)
{
	switch (type) {
	case HCI_ACLDATA_PKT: return HCI_ACL_HDR_SIZE;
	case HCI_SCODATA_PKT: return HCI_SCO_HDR_SIZE;
	case HCI_EVENT_PKT: return HCI_EVENT_HDR_SIZE;
	case HCI_ISODATA_PKT: return HCI_ISO_HDR_SIZE;
	default: return -EINVAL;
	}
}

/* BFGX framing can split an H4 packet across receive queue entries. */
static int charlotte_bt_receive(struct hci_dev *hdev, const u8 *data, size_t len)
{
	struct pm_drv_data *pm = hci_get_drvdata(hdev);
	struct hisi_bt_dev *bt = &pm->st_bt_dev;
	struct sk_buff *skb;
	unsigned int count, payload;
	int header, ret = 0;

	mutex_lock(&bt->rx_mutex);
	while (len) {
		skb = bt->rx_skb;
		if (!skb) {
			header = charlotte_bt_header_len(*data);
			if (header < 0) {
				ret = header;
				break;
			}
			skb = bt_skb_alloc(BT_TX_MAX_FRAME, GFP_KERNEL);
			if (!skb) {
				ret = -ENOMEM;
				break;
			}
			hci_skb_pkt_type(skb) = *data++;
			hci_skb_expect(skb) = header;
			bt->rx_skb = skb;
			len--;
		}
		count = min_t(size_t, hci_skb_expect(skb) - skb->len, len);
		skb_put_data(skb, data, count);
		data += count;
		len -= count;
		if (skb->len < hci_skb_expect(skb))
			continue;
		header = charlotte_bt_header_len(hci_skb_pkt_type(skb));
		if (skb->len == header) {
			if (hci_skb_pkt_type(skb) == HCI_EVENT_PKT)
				payload = skb->data[1];
			else if (hci_skb_pkt_type(skb) == HCI_SCODATA_PKT)
				payload = skb->data[2];
			else
				payload = get_unaligned_le16(skb->data + 2);
			if (hci_skb_pkt_type(skb) == HCI_ISODATA_PKT)
				payload &= 0x3fff;
			if (payload > skb_tailroom(skb)) {
				ret = -EMSGSIZE;
				break;
			}
			hci_skb_expect(skb) += payload;
			if (payload)
				continue;
		}
		bt->rx_skb = NULL;
		ret = hci_recv_frame(hdev, skb);
		if (ret)
			break;
	}
	if (ret) {
		kfree_skb(bt->rx_skb);
		bt->rx_skb = NULL;
		hdev->stat.err_rx++;
	}
	mutex_unlock(&bt->rx_mutex);
	return ret;
}

static int charlotte_bt_rx_thread(void *data)
{
	struct pm_drv_data *pm = data;
	struct ps_core_s *core = pm->ps_core_data;
	struct hci_dev *hdev = pm->st_bt_dev.hdev;
	struct sk_buff *skb;

	while (!kthread_should_stop()) {
		wait_event_interruptible(core->bfgx_info[BFGX_BT].rx_wait,
			!skb_queue_empty(&core->bfgx_info[BFGX_BT].rx_queue) ||
			kthread_should_stop());
		if (kthread_should_stop())
			break;
		while ((skb = ps_skb_dequeue(core, RX_BT_QUEUE))) {
			hdev->stat.byte_rx += skb->len;
			charlotte_bt_receive(hdev, skb->data, skb->len);
			kfree_skb(skb);
		}
	}
	return 0;
}

static int charlotte_bt_register(struct ps_core_s *core, struct device *parent)
{
	struct pm_drv_data *pm = core->pm_data;
	struct hisi_bt_dev *bt = &pm->st_bt_dev;
	struct hci_dev *hdev;
	int ret;

	if (bt->hdev)
		return -EEXIST;
	hdev = hci_alloc_dev();
	if (!hdev)
		return -ENOMEM;
	hdev->bus = HCI_UART;
	hdev->open = charlotte_bt_open;
	hdev->close = charlotte_bt_close;
	hdev->send = charlotte_bt_send;
	hci_set_drvdata(hdev, pm);
	SET_HCIDEV_DEV(hdev, parent);
	mutex_init(&bt->rx_mutex);
	bt->hdev = hdev;
	bt->bt_recv_task = kthread_run(charlotte_bt_rx_thread, pm, "charlotte-bt-rx");
	if (IS_ERR(bt->bt_recv_task)) {
		ret = PTR_ERR(bt->bt_recv_task);
		bt->bt_recv_task = NULL;
		goto free;
	}
	ret = hci_register_dev(hdev);
	if (!ret)
		return 0;
	kthread_stop(bt->bt_recv_task);
	bt->bt_recv_task = NULL;
free:
	bt->hdev = NULL;
	hci_free_dev(hdev);
	mutex_destroy(&bt->rx_mutex);
	return ret;
}

void charlotte_bt_unregister(struct ps_core_s *core)
{
	struct hisi_bt_dev *bt = &core->pm_data->st_bt_dev;
	struct hci_dev *hdev = bt->hdev;

	if (!hdev)
		return;
	/* Close HCI while RX still runs: the firmware's final replies and
	 * close acknowledgments must remain serviceable until unregister.
	 */
	hci_unregister_dev(hdev);
	kthread_stop(bt->bt_recv_task);
	bt->bt_recv_task = NULL;
	charlotte_bt_rx_reset(bt);
	bt->hdev = NULL;
	hci_free_dev(hdev);
	mutex_destroy(&bt->rx_mutex);
}

int32_t hw_bt_open(struct inode *inode, struct file *file) { return 0; }
int32_t hw_bt_release(struct inode *inode, struct file *file) { return 0; }
ssize_t hw_bt_read(struct file *file, char __user *buf, size_t len, loff_t *pos)
{ return -EOPNOTSUPP; }
ssize_t hw_bt_write(struct file *file, const char __user *buf, size_t len, loff_t *pos)
{ return -EOPNOTSUPP; }
uint32_t hw_bt_poll(struct file *file, poll_table *wait) { return EPOLLERR; }

long hw_bt_ioctl(struct file *file, uint32_t cmd, unsigned long arg)
{
	struct ps_core_s *core;
	struct miscdevice *misc;
	int ret;

	if (!file || !capable(CAP_NET_ADMIN))
		return -EPERM;
	misc = file->private_data;
	core = get_ps_core_from_miscdev(misc);
	if (!core)
		return -ENODEV;
	if (cmd == BT_IOCTL_HCISETPROTO) {
		mutex_lock(&pm_get_top()->host_mutex);
		ret = charlotte_bt_register(core, misc->this_device);
		mutex_unlock(&pm_get_top()->host_mutex);
		return ret;
	}
	/* Endpoint removal is owned by ps_remove, avoiding a second
	 * unregister concurrent with asynchronous HCI open/close work.
	 */
	return -ENOTTY;
}
