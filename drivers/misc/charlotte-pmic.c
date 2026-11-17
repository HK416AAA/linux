// SPDX-License-Identifier: GPL-2.0
/*
 * Kirin 9010 (charlotte) PMIC bring-up helper.
 *
 * The published tree carries the SPMI controller but neither the PMIC
 * regulator driver nor the reboot driver, so the touch rails cannot be
 * switched on through the regulator framework.  This driver drops to the
 * raw SPMI register interface instead: it dumps the PMIC register file at
 * probe time and exposes read/write/ldo controls through debugfs.
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spmi.h>
#include <linux/uaccess.h>

#define CHARLOTTE_PMIC_REGS	256
#define CHARLOTTE_PMIC_CHUNK	16
/* SPMI register reads are 5-bit; 0x20 and up need the extended command. */
#define CHARLOTTE_PMIC_BASIC_MAX	0x1f

struct charlotte_pmic_ldo {
	const char *name;
	u8 en_reg;
	u8 v_reg;
	u8 vsel_mask;
};

static const struct charlotte_pmic_ldo charlotte_pmic_ldos[] = {
	{ "ldo3",  0x16, 0x51, 0x0f },
	{ "ldo4",  0x17, 0x52, 0x07 },
	{ "ldo9",  0x1c, 0x57, 0x07 },
	{ "ldo15", 0x21, 0x5c, 0x07 },
	{ "ldo16", 0x22, 0x5d, 0x07 },
	{ "ldo17", 0x23, 0x5e, 0x07 },
	{ "ldo33", 0x32, 0x6d, 0x07 },
	{ "ldo34", 0x33, 0x6e, 0x07 },
};

static bool allow_writes;
module_param(allow_writes, bool, 0644);
MODULE_PARM_DESC(allow_writes,
		 "permit SPMI register writes from debugfs (default off: a wrong voltage select can brown out the SoC)");

struct charlotte_pmic {
	struct spmi_device *sdev;
	struct dentry *root;
};

static int charlotte_pmic_read(struct charlotte_pmic *pmic, u8 reg, u8 *val)
{
	int i, ret = -EIO;

	for (i = 0; i < 3; i++) {
		if (reg <= CHARLOTTE_PMIC_BASIC_MAX)
			ret = spmi_register_read(pmic->sdev, reg, val);
		else
			ret = spmi_ext_register_read(pmic->sdev, reg, val, 1);
		if (!ret)
			return 0;
		usleep_range(50, 100);
	}

	return ret;
}

static int charlotte_pmic_write(struct charlotte_pmic *pmic, u8 reg, u8 val)
{
	int i, ret = -EIO;

	for (i = 0; i < 3; i++) {
		if (reg <= CHARLOTTE_PMIC_BASIC_MAX)
			ret = spmi_register_write(pmic->sdev, reg, val);
		else
			ret = spmi_ext_register_write(pmic->sdev, reg, &val, 1);
		if (!ret) {
			usleep_range(50, 100);
			return 0;
		}
		usleep_range(50, 100);
	}

	return ret;
}

static void charlotte_pmic_dump(struct charlotte_pmic *pmic, const char *tag)
{
	struct device *dev = &pmic->sdev->dev;
	int i, j, errors = 0;

	for (i = 0; i < CHARLOTTE_PMIC_REGS; i += CHARLOTTE_PMIC_CHUNK) {
		char line[CHARLOTTE_PMIC_CHUNK * 4 + 8];
		int n = 0;

		n += scnprintf(line + n, sizeof(line) - n, "%s %02x:", tag, i);
		for (j = 0; j < CHARLOTTE_PMIC_CHUNK; j++) {
			u8 val;

			if (charlotte_pmic_read(pmic, i + j, &val)) {
				errors++;
				n += scnprintf(line + n, sizeof(line) - n, " ??");
			} else {
				n += scnprintf(line + n, sizeof(line) - n,
					       " %02x", val);
			}
		}
		dev_info(dev, "%s\n", line);
		if (errors >= 48) {
			dev_info(dev, "%s: too many failures, dump aborted\n",
				 tag);
			break;
		}
	}

	dev_info(dev, "%s: %d register read errors\n", tag, errors);
}

static ssize_t charlotte_pmic_regs_read(struct file *file, char __user *ubuf,
					size_t count, loff_t *ppos)
{
	struct charlotte_pmic *pmic = file->private_data;
	char *buf;
	int i, j, errors = 0, n = 0;
	ssize_t ret;

	buf = kzalloc(CHARLOTTE_PMIC_REGS * 4 + CHARLOTTE_PMIC_CHUNK * 8,
		      GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (i = 0; i < CHARLOTTE_PMIC_REGS; i += CHARLOTTE_PMIC_CHUNK) {
		n += scnprintf(buf + n, 4096 - n, "%02x:", i);
		for (j = 0; j < CHARLOTTE_PMIC_CHUNK; j++) {
			u8 val;

			if (charlotte_pmic_read(pmic, i + j, &val)) {
				errors++;
				n += scnprintf(buf + n, 4096 - n, " ??");
				continue;
			}
			n += scnprintf(buf + n, 4096 - n, " %02x", val);
		}
		n += scnprintf(buf + n, 4096 - n, "\n");
	}
	n += scnprintf(buf + n, 4096 - n, "errors: %d\n", errors);

	ret = simple_read_from_buffer(ubuf, count, ppos, buf, n);
	kfree(buf);
	return ret;
}

static ssize_t charlotte_pmic_rw_write(struct file *file,
				       const char __user *ubuf,
				       size_t count, loff_t *ppos)
{
	struct charlotte_pmic *pmic = file->private_data;
	unsigned int reg, val;
	char buf[32], op;
	int ret;
	u8 cur;

	if (count == 0 || count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	if (sscanf(buf, " %c %x %x", &op, &reg, &val) < 2)
		return -EINVAL;
	if (reg >= CHARLOTTE_PMIC_REGS)
		return -EINVAL;

	if (op == 'r') {
		ret = charlotte_pmic_read(pmic, reg, &cur);
		if (ret)
			return ret;
		dev_info(&pmic->sdev->dev, "reg %02x = %02x\n", reg, cur);
	} else if (op == 'w') {
		if (!allow_writes)
			return -EPERM;
		if (sscanf(buf, " %c %x %x", &op, &reg, &val) != 3)
			return -EINVAL;
		ret = charlotte_pmic_write(pmic, reg, val & 0xff);
		if (ret)
			return ret;
		dev_info(&pmic->sdev->dev, "reg %02x <- %02x\n", reg, val & 0xff);
	} else {
		return -EINVAL;
	}

	return count;
}

static ssize_t charlotte_pmic_ldo_write(struct file *file,
					const char __user *ubuf,
					size_t count, loff_t *ppos)
{
	struct charlotte_pmic *pmic = file->private_data;
	const struct charlotte_pmic_ldo *ldo = NULL;
	unsigned int id, on, vsel = 0;
	char buf[32];
	u8 rb_en = 0, rb_vsel = 0;
	int nargs, ret;

	if (count == 0 || count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	if (!allow_writes)
		return -EPERM;

	nargs = sscanf(buf, "%u %u %u", &id, &on, &vsel);
	if (nargs < 2)
		return -EINVAL;
	if (id >= ARRAY_SIZE(charlotte_pmic_ldos))
		return -EINVAL;

	ldo = &charlotte_pmic_ldos[id];
	if (nargs == 3) {
		if (vsel > ldo->vsel_mask)
			return -EINVAL;
		ret = charlotte_pmic_write(pmic, ldo->v_reg, vsel);
		if (ret)
			return ret;
		usleep_range(200, 400);
	} else {
		u8 cur = 0;

		charlotte_pmic_read(pmic, ldo->v_reg, &cur);
		vsel = cur;
	}
	ret = charlotte_pmic_write(pmic, ldo->en_reg, on ? 0x01 : 0x00);
	if (ret)
		return ret;
	usleep_range(2000, 4000);

	charlotte_pmic_read(pmic, ldo->en_reg, &rb_en);
	charlotte_pmic_read(pmic, ldo->v_reg, &rb_vsel);
	dev_info(&pmic->sdev->dev, "%s vsel %02x en %u (readback en %02x vsel %02x)\n",
		 ldo->name, vsel, on ? 1 : 0, rb_en, rb_vsel);
	return count;
}

static ssize_t charlotte_pmic_ldo_read(struct file *file, char __user *ubuf,
				       size_t count, loff_t *ppos)
{
	struct charlotte_pmic *pmic = file->private_data;
	char *buf;
	ssize_t ret;
	size_t i;
	int n = 0;

	buf = kzalloc(512, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(charlotte_pmic_ldos); i++) {
		const struct charlotte_pmic_ldo *ldo = &charlotte_pmic_ldos[i];
		u8 en = 0xff, vsel = 0xff;

		charlotte_pmic_read(pmic, ldo->en_reg, &en);
		charlotte_pmic_read(pmic, ldo->v_reg, &vsel);
		n += scnprintf(buf + n, 512 - n,
			       "%zu %s en_reg %02x v_reg %02x en %02x vsel %02x\n",
			       i, ldo->name, ldo->en_reg, ldo->v_reg, en, vsel);
	}
	n += scnprintf(buf + n, 512 - n,
		       "write: echo \"<index> <0|1> [vsel]\" > ldo (omit vsel to keep it)\n");

	ret = simple_read_from_buffer(ubuf, count, ppos, buf, n);
	kfree(buf);
	return ret;
}

static int charlotte_pmic_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static const struct file_operations charlotte_pmic_regs_fops = {
	.open = charlotte_pmic_open,
	.read = charlotte_pmic_regs_read,
	.llseek = default_llseek,
};

static const struct file_operations charlotte_pmic_rw_fops = {
	.open = charlotte_pmic_open,
	.write = charlotte_pmic_rw_write,
	.llseek = noop_llseek,
};

static const struct file_operations charlotte_pmic_ldo_fops = {
	.open = charlotte_pmic_open,
	.read = charlotte_pmic_ldo_read,
	.write = charlotte_pmic_ldo_write,
	.llseek = noop_llseek,
};

static int charlotte_pmic_probe(struct spmi_device *sdev)
{
	struct charlotte_pmic *pmic;

	pmic = devm_kzalloc(&sdev->dev, sizeof(*pmic), GFP_KERNEL);
	if (!pmic)
		return -ENOMEM;

	pmic->sdev = sdev;

	charlotte_pmic_dump(pmic, "spmi");

	pmic->root = debugfs_create_dir("charlotte-pmic", NULL);
	if (!IS_ERR_OR_NULL(pmic->root)) {
		debugfs_create_file("regs", 0444, pmic->root, pmic,
				    &charlotte_pmic_regs_fops);
		debugfs_create_file("rw", 0200, pmic->root, pmic,
				    &charlotte_pmic_rw_fops);
		debugfs_create_file("ldo", 0600, pmic->root, pmic,
				    &charlotte_pmic_ldo_fops);
	}

	return 0;
}

static const struct of_device_id charlotte_pmic_match[] = {
	{ .compatible = "hisilicon,charlotte-pmic" },
	{ }
};
MODULE_DEVICE_TABLE(of, charlotte_pmic_match);

static struct spmi_driver charlotte_pmic_driver = {
	.probe = charlotte_pmic_probe,
	.driver = {
		.name = "charlotte-pmic",
		.of_match_table = charlotte_pmic_match,
	},
};
module_spmi_driver(charlotte_pmic_driver);

MODULE_DESCRIPTION("Kirin 9010 PMIC bring-up helper");
MODULE_LICENSE("GPL v2");
