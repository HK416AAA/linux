// SPDX-License-Identifier: GPL-2.0-only
/* TGR-W00 Novatek host processing transport.
 * The controller supplies raw capacitance frames, not Linux contacts.
 * The original AFE ABI is used by an isolated userspace AFE/TSA service,
 * which reports contacts through uinput. No Android tpkit dependencies.
 */
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/spi/spi.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define THP_FRAME_MAX 8192
#define THP_SYNC_MAX 245762
#define THP_QUEUE_LEN 8
#define THP_TYPE 0xb8

/* This private 64-bit ABI matches vendor huawei_thp.h. Scalar control
 * commands pass their value directly, despite being declared _IOW.
 * The second field is timeval-compatible: seconds and microseconds.
 */
struct thp_data {
	u64 first;
	u64 second;
	u32 size;
	u32 padding;
};

struct thp_frame {
	u8 data[THP_FRAME_MAX];
	s64 tv[2];
	u32 size;
};

struct charlotte_thp {
	struct spi_device *spi;
	struct miscdevice misc;
	struct gpio_desc *reset, *irq_gpio;
	struct mutex ops_lock, spi_lock, queue_lock;
	wait_queue_head_t wait;
	atomic_t opened;
	bool irq_enabled, blocking;
	u32 timeout_ms, frame_size, speed;
	u8 *tx, *rx;
	struct thp_frame *frames;
	unsigned int head, count;
};

static void thp_clear(struct charlotte_thp *thp)
{
	mutex_lock(&thp->queue_lock);
	thp->head = thp->count = 0;
	mutex_unlock(&thp->queue_lock);
}

static void thp_set_irq(struct charlotte_thp *thp, bool enabled)
{
	if (thp->irq_enabled == enabled)
		return;
	thp->irq_enabled = enabled;
	if (enabled)
		enable_irq(thp->spi->irq);
	else
		disable_irq(thp->spi->irq);
}

/* Caller holds spi_lock; one message keeps CS asserted for every byte. */
static int thp_transfer(struct charlotte_thp *thp, unsigned int size)
{
	struct spi_transfer transfer = {
		.tx_buf = thp->tx, .rx_buf = thp->rx,
		.len = size, .speed_hz = thp->speed,
	};

	return spi_sync_transfer(thp->spi, &transfer, 1);
}

static irqreturn_t thp_irq_thread(int irq, void *data)
{
	struct charlotte_thp *thp = data;
	struct thp_frame *frame;
	struct timespec64 now;
	u32 size;
	int ret;

	ktime_get_real_ts64(&now);
	mutex_lock(&thp->spi_lock);
	size = thp->frame_size;
	memset(thp->tx, 0, size);
	thp->tx[0] = 0x20;
	ret = thp_transfer(thp, size);
	if (!ret) {
		mutex_lock(&thp->queue_lock);
		/* Drop the oldest frame when userspace falls behind. */
		if (thp->count == THP_QUEUE_LEN) {
			thp->head = (thp->head + 1) % THP_QUEUE_LEN;
			thp->count--;
		}
		frame = &thp->frames[(thp->head + thp->count) % THP_QUEUE_LEN];
		memcpy(frame->data, thp->rx, size);
		frame->size = size;
		frame->tv[0] = now.tv_sec;
		frame->tv[1] = now.tv_nsec / 1000;
		thp->count++;
		mutex_unlock(&thp->queue_lock);
	}
	mutex_unlock(&thp->spi_lock);
	if (ret)
		dev_err_ratelimited(&thp->spi->dev, "frame transfer failed: %d\n", ret);
	else
		wake_up_interruptible(&thp->wait);
	return IRQ_HANDLED;
}

static long thp_get_frame(struct charlotte_thp *thp, unsigned long arg)
{
	struct thp_data data;
	struct thp_frame *frame;
	long ret;

	if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
		return -EFAULT;
	if (!data.first || !data.second || !data.size || data.size > THP_FRAME_MAX)
		return -EINVAL;
	if (data.size != thp->frame_size) {
		/* Synchronize with the reader before changing the transfer size. */
		mutex_lock(&thp->spi_lock);
		WRITE_ONCE(thp->frame_size, data.size);
		thp_clear(thp);
		mutex_unlock(&thp->spi_lock);
	}
	thp_set_irq(thp, true);
	if (thp->blocking) {
		ret = wait_event_interruptible_timeout(thp->wait,
			READ_ONCE(thp->count), msecs_to_jiffies(thp->timeout_ms));
		if (ret <= 0)
			return ret ? ret : -ETIMEDOUT;
	}
	mutex_lock(&thp->queue_lock);
	ret = -ENODATA;
	if (thp->count) {
		frame = &thp->frames[thp->head];
		if (frame->size != data.size) {
			ret = -EIO;
		} else if (copy_to_user(u64_to_user_ptr(data.first), frame->data, data.size) ||
			   copy_to_user(u64_to_user_ptr(data.second), frame->tv, sizeof(frame->tv))) {
			ret = -EFAULT;
		} else {
			ret = 0;
		}
		thp->head = (thp->head + 1) % THP_QUEUE_LEN;
		thp->count--;
	}
	mutex_unlock(&thp->queue_lock);
	return ret;
}

static long thp_sync(struct charlotte_thp *thp, unsigned long arg)
{
	struct thp_data data;
	int ret;

	if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
		return -EFAULT;
	if (!data.first || !data.size || data.size > THP_SYNC_MAX)
		return -EINVAL;
	mutex_lock(&thp->spi_lock);
	if (copy_from_user(thp->tx, u64_to_user_ptr(data.first), data.size)) {
		ret = -EFAULT;
		goto out;
	}
	ret = thp_transfer(thp, data.size);
	if (!ret && data.second &&
	    copy_to_user(u64_to_user_ptr(data.second), thp->rx, data.size))
		ret = -EFAULT;
out:
	mutex_unlock(&thp->spi_lock);
	return ret;
}

static long thp_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct charlotte_thp *thp = file->private_data;
	u32 value;
	long ret = 0;

	if (_IOC_TYPE(cmd) != THP_TYPE)
		return -ENOTTY;
	if (mutex_lock_interruptible(&thp->ops_lock))
		return -ERESTARTSYS;
	switch (cmd) {
	case _IOWR(THP_TYPE, 1, struct thp_data):
		ret = thp_get_frame(thp, arg);
		break;
	case _IOW(THP_TYPE, 2, u32):
		thp_set_irq(thp, false);
		/* Vendor scalar is the physical reset level. */
		gpiod_set_value_cansleep(thp->reset, !arg);
		thp_clear(thp);
		break;
	case _IOW(THP_TYPE, 3, u32):
		thp->timeout_ms = clamp_val(arg, 1, 2000);
		break;
	case _IOWR(THP_TYPE, 4, struct thp_data):
		ret = thp_sync(thp, arg);
		break;
	case _IO(THP_TYPE, 5): /* AFE finish notification */
		break;
	case _IOW(THP_TYPE, 6, u32):
		thp->blocking = !!arg;
		break;
	case _IOW(THP_TYPE, 7, u32):
		thp_set_irq(thp, !!arg);
		break;
	case _IOW(THP_TYPE, 8, u32):
		value = READ_ONCE(thp->count);
		ret = put_user(value, (u32 __user *)arg);
		break;
	case _IOW(THP_TYPE, 9, u32):
		thp_clear(thp);
		break;
	case _IOW(THP_TYPE, 10, u32):
		ret = gpiod_get_value_cansleep(thp->irq_gpio);
		if (ret >= 0) {
			value = ret;
			ret = put_user(value, (u32 __user *)arg);
		}
		break;
	case _IOW(THP_TYPE, 11, u32):
		if (arg < 10000 || arg > thp->spi->max_speed_hz)
			ret = -EINVAL;
		else {
			mutex_lock(&thp->spi_lock);
			thp->speed = arg;
			mutex_unlock(&thp->spi_lock);
		}
		break;
	case _IOW(THP_TYPE, 13, u32[3]): /* AFE firmware/test status */
		/* No Android notifier consumers. Still validate the userspace ABI. */
		{
			u32 status[3];

			if (copy_from_user(status, (void __user *)arg, sizeof(status)))
				ret = -EFAULT;
		}
		break;
	default:
		ret = -ENOTTY;
	}
	mutex_unlock(&thp->ops_lock);
	return ret;
}

static int thp_open(struct inode *inode, struct file *file)
{
	struct charlotte_thp *thp = container_of(file->private_data,
						struct charlotte_thp, misc);

	if (atomic_cmpxchg(&thp->opened, 0, 1))
		return -EBUSY;
	file->private_data = thp;
	thp->blocking = true;
	thp->timeout_ms = 480;
	thp->frame_size = THP_FRAME_MAX;
	thp->speed = thp->spi->max_speed_hz;
	thp_clear(thp);
	gpiod_set_value_cansleep(thp->reset, 1);
	msleep(5);
	gpiod_set_value_cansleep(thp->reset, 0);
	msleep(15);
	return nonseekable_open(inode, file);
}

static int thp_release(struct inode *inode, struct file *file)
{
	struct charlotte_thp *thp = file->private_data;

	thp_set_irq(thp, false);
	gpiod_set_value_cansleep(thp->reset, 1);
	thp_clear(thp);
	atomic_set(&thp->opened, 0);
	return 0;
}

static const struct file_operations thp_fops = {
	.owner = THIS_MODULE,
	.open = thp_open,
	.release = thp_release,
	.unlocked_ioctl = thp_ioctl,
};

static void thp_unregister(void *data)
{
	struct charlotte_thp *thp = data;

	misc_deregister(&thp->misc);
}

static int thp_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct charlotte_thp *thp;
	int ret;

	if (spi->irq <= 0)
		return -EINVAL;
	thp = devm_kzalloc(dev, sizeof(*thp), GFP_KERNEL);
	if (!thp)
		return -ENOMEM;
	thp->spi = spi;
	thp->tx = devm_kzalloc(dev, THP_SYNC_MAX, GFP_KERNEL);
	thp->rx = devm_kzalloc(dev, THP_SYNC_MAX, GFP_KERNEL);
	thp->frames = devm_kcalloc(dev, THP_QUEUE_LEN, sizeof(*thp->frames), GFP_KERNEL);
	if (!thp->tx || !thp->rx || !thp->frames)
		return -ENOMEM;
	thp->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(thp->reset))
		return dev_err_probe(dev, PTR_ERR(thp->reset), "reset GPIO\n");
	thp->irq_gpio = devm_gpiod_get(dev, "irq", GPIOD_IN);
	if (IS_ERR(thp->irq_gpio))
		return dev_err_probe(dev, PTR_ERR(thp->irq_gpio), "IRQ GPIO\n");
	mutex_init(&thp->ops_lock);
	mutex_init(&thp->spi_lock);
	mutex_init(&thp->queue_lock);
	init_waitqueue_head(&thp->wait);
	ret = devm_request_threaded_irq(dev, spi->irq, NULL, thp_irq_thread,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					dev_name(dev), thp);
	if (ret)
		return dev_err_probe(dev, ret, "touch IRQ\n");
	thp->misc = (struct miscdevice) {
		.minor = MISC_DYNAMIC_MINOR, .name = "thp",
		.fops = &thp_fops, .parent = dev, .mode = 0600,
	};
	ret = misc_register(&thp->misc);
	if (ret)
		return ret;
	spi_set_drvdata(spi, thp);
	dev_info(dev, "Novatek THP transport ready (raw frames, private 64-bit AFE ABI)\n");
	return devm_add_action_or_reset(dev, thp_unregister, thp);
}

static void thp_shutdown(struct spi_device *spi)
{
	struct charlotte_thp *thp = spi_get_drvdata(spi);

	thp_set_irq(thp, false);
	gpiod_set_value_cansleep(thp->reset, 1);
}

static int thp_suspend(struct device *dev)
{
	struct charlotte_thp *thp = dev_get_drvdata(dev);

	/* AFE suspend/resume and gesture firmware are not implemented yet. */
	return atomic_read(&thp->opened) ? -EBUSY : 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(thp_pm, thp_suspend, NULL);

static const struct of_device_id thp_match[] = {
	{ .compatible = "hisilicon,charlotte-novatek-thp" }, { }
};

static const struct spi_device_id thp_ids[] = {
	{ "charlotte-novatek-thp" }, { }
};

static struct spi_driver thp_driver = {
	.probe = thp_probe,
	.shutdown = thp_shutdown,
	.id_table = thp_ids,
	.driver = {
		.name = "charlotte-thp", .of_match_table = thp_match,
		.pm = pm_sleep_ptr(&thp_pm), .suppress_bind_attrs = true,
	},
};
module_spi_driver(thp_driver);
MODULE_DESCRIPTION("Charlotte Novatek host processing touch transport");
MODULE_LICENSE("GPL");
