/*
 * TI DRV2667 piezo driver for the Lenovo Legion Y700 1st gen (TB-9707F)
 *
 * Pure sysfs driver: /sys/drv2667/play_wave. No input-FF, no input
 * device, no regulator, no GPIO, no IRQ -- the stock DT node
 * (drv2667@59 on qupv3_se1_i2c, 0x59) has none of those, and VDD is
 * battery-direct, so standby is register-only.
 *
 * Based on the upstream TI driver by Dan Murphy (2014,
 * drivers/input/misc/drv2667.c, GPLv2): same register map, same probe
 * init sequence (CTRL_2/CTRL_1/WV_SEQ, page-1 Mode 3 synthesizer
 * waveform at 195 Hz, 25 Vpp gain) and same worker (magnitude ->
 * RAM_AMP -> GO, 0 -> clear GO). The input-FF front-end is removed
 * and replaced with the stock ZUI sysfs interface: a kobject
 * "drv2667" with a "play_wave" attribute, plus the stock polling
 * reset worker (DRV2667_POLLING).
 *
 * play_wave semantics (see drv2667-research.md): userspace writes a
 * string; it is parsed to an integer play_magnitude (0-255) and the
 * single probe-programmed RAM waveform is played with that magnitude
 * as the RAM amplitude. 0 stops playback (clears GO). The only
 * in-ROM writer (ZuiPhoneWindowManager.setBackPanelLight) sends
 * hex-encoded bytes ("ff" .. "00"), so the parser accepts BOTH:
 *   - "0x.." prefix          -> hex
 *   - contains a-f / A-F     -> hex (e.g. "ff" = 255)
 *   - otherwise              -> decimal (e.g. "255" = 255, "128" = 128)
 * Values are clamped to 0-255. The show side returns the last
 * magnitude (the stock Java reader reads the node back).
 *
 * Copyright: (C) 2014 Texas Instruments, Inc. (upstream base)
 * Copyright: (C) 2026 Y700 port -- sysfs rewrite
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>

/* Control registers */
#define DRV2667_STATUS		0x00
#define DRV2667_CTRL_1		0x01
#define DRV2667_CTRL_2		0x02
/* Waveform sequencer */
#define DRV2667_WV_SEQ_0	0x03
#define DRV2667_WV_SEQ_1	0x04
#define DRV2667_WV_SEQ_2	0x05
#define DRV2667_WV_SEQ_3	0x06
#define DRV2667_WV_SEQ_4	0x07
#define DRV2667_WV_SEQ_5	0x08
#define DRV2667_WV_SEQ_6	0x09
#define DRV2667_WV_SEQ_7	0x0A
#define DRV2667_FIFO		0x0B
#define DRV2667_PAGE		0xFF
#define DRV2667_MAX_REG		DRV2667_PAGE

#define DRV2667_PAGE_0		0x00
#define DRV2667_PAGE_1		0x01

/* RAM fields */
#define DRV2667_RAM_HDR_SZ	0x0
/* RAM Header addresses */
#define DRV2667_RAM_START_HI	0x01
#define DRV2667_RAM_START_LO	0x02
#define DRV2667_RAM_STOP_HI	0x03
#define DRV2667_RAM_STOP_LO	0x04
#define DRV2667_RAM_REPEAT_CT	0x05
/* RAM data addresses */
#define DRV2667_RAM_AMP	0x06
#define DRV2667_RAM_FREQ	0x07
#define DRV2667_RAM_DURATION	0x08
#define DRV2667_RAM_ENVELOPE	0x09

/* Control 1 Register */
#define DRV2667_25_VPP_GAIN	0x00
#define DRV2667_50_VPP_GAIN	0x01
#define DRV2667_75_VPP_GAIN	0x02
#define DRV2667_100_VPP_GAIN	0x03
#define DRV2667_DIGITAL_IN	0xfc
#define DRV2667_ANALOG_IN	(1 << 2)
#define DRV2667_CHIPID_MASK	0x38	/* CTRL_1 bits 6-3 */
#define DRV2667_CHIPID		0x07	/* DRV2667 (5 = DRV2665) */

/* Control 2 Register */
#define DRV2667_GO		(1 << 0)
#define DRV2667_STANDBY	(1 << 6)
#define DRV2667_DEV_RST	(1 << 7)

/* RAM Envelope settings */
#define DRV2667_NO_ENV		0x00

/* GO self-clears on playback completion; if it is still set after
 * this long the engine is stuck and the chip gets reset.
 */
#define DRV2667_POLL_TIMEOUT_MS	2000

/**
 * struct drv2667_data -
 * @client:	Pointer to the I2C client
 * @regmap:	Register map of the device
 * @work:	Work item that plays/stops the waveform
 * @reset_work:	Delayed work that polls GO and resets a stuck engine
 * @lock:	Serialises play_wave writes against suspend/resume
 * @kobj:	/sys/drv2667 kobject
 * @page:	RAM page holding the waveform (page 1)
 * @magnitude:	Last play magnitude (0-255), also the show() value
 * @frequency:	Waveform frequency in Hz (195)
 */
struct drv2667_data {
	struct i2c_client *client;
	struct regmap *regmap;
	struct work_struct work;
	struct delayed_work reset_work;
	struct mutex lock;
	struct kobject *kobj;
	u32 page;
	u32 magnitude;
	u32 frequency;
};

static const struct reg_default drv2667_reg_defs[] = {
	{ DRV2667_STATUS, 0x02 },
	{ DRV2667_CTRL_1, 0x28 },
	{ DRV2667_CTRL_2, 0x40 },
	{ DRV2667_WV_SEQ_0, 0x00 },
	{ DRV2667_WV_SEQ_1, 0x00 },
	{ DRV2667_WV_SEQ_2, 0x00 },
	{ DRV2667_WV_SEQ_3, 0x00 },
	{ DRV2667_WV_SEQ_4, 0x00 },
	{ DRV2667_WV_SEQ_5, 0x00 },
	{ DRV2667_WV_SEQ_6, 0x00 },
	{ DRV2667_WV_SEQ_7, 0x00 },
	{ DRV2667_FIFO, 0x00 },
	{ DRV2667_PAGE, 0x00 },
};

static int drv2667_set_waveform_freq(struct drv2667_data *haptics)
{
	unsigned int read_buf;
	int freq;
	int error;

	/* Per the data sheet:
	 * Sinusoid Frequency (Hz) = 7.8125 x Frequency
	 */
	freq = (haptics->frequency * 1000) / 78125;
	if (freq <= 0) {
		dev_err(&haptics->client->dev,
			"ERROR: Frequency calculated to %i\n", freq);
		return -EINVAL;
	}

	error = regmap_read(haptics->regmap, DRV2667_PAGE, &read_buf);
	if (error) {
		dev_err(&haptics->client->dev,
			"Failed to read the page number: %d\n", error);
		return -EIO;
	}

	if (read_buf == DRV2667_PAGE_0 ||
	    haptics->page != read_buf) {
		error = regmap_write(haptics->regmap,
				     DRV2667_PAGE, haptics->page);
		if (error) {
			dev_err(&haptics->client->dev,
				"Failed to set the page: %d\n", error);
			return -EIO;
		}
	}

	error = regmap_write(haptics->regmap, DRV2667_RAM_FREQ,
			     freq);
	if (error)
		dev_err(&haptics->client->dev,
			"Failed to set the frequency: %d\n", error);

	/* Reset back to original page */
	if (read_buf == DRV2667_PAGE_0 ||
	    haptics->page != read_buf) {
		error = regmap_write(haptics->regmap, DRV2667_PAGE, read_buf);
		if (error) {
			dev_err(&haptics->client->dev,
				"Failed to set the page: %d\n", error);
			return -EIO;
		}
	}

	return error;
}

static void drv2667_worker(struct work_struct *work)
{
	struct drv2667_data *haptics = container_of(work, struct drv2667_data, work);
	int error;

	dev_info(&haptics->client->dev, "[drv2667][%s]  magnitude:%d\n",
		 __func__, haptics->magnitude);

	if (haptics->magnitude) {
		error = regmap_write(haptics->regmap,
				     DRV2667_PAGE, haptics->page);
		if (error) {
			dev_err(&haptics->client->dev,
				"Failed to set the page: %d\n", error);
			return;
		}

		error = regmap_write(haptics->regmap, DRV2667_RAM_AMP,
				     haptics->magnitude);
		if (error) {
			dev_err(&haptics->client->dev,
				"Failed to set the amplitude: %d\n", error);
			return;
		}

		error = regmap_write(haptics->regmap,
				     DRV2667_PAGE, DRV2667_PAGE_0);
		if (error) {
			dev_err(&haptics->client->dev,
				"Failed to set the page: %d\n", error);
			return;
		}

		error = regmap_write(haptics->regmap,
				     DRV2667_CTRL_2, DRV2667_GO);
		if (error) {
			dev_err(&haptics->client->dev,
				"Failed to set the GO bit: %d\n", error);
			return;
		}

		/* GO self-clears when the sequencer finishes; poll for a
		 * stuck engine and reset it if it never clears.
		 */
		schedule_delayed_work(&haptics->reset_work,
				      msecs_to_jiffies(DRV2667_POLL_TIMEOUT_MS));
	} else {
		cancel_delayed_work(&haptics->reset_work);
		error = regmap_update_bits(haptics->regmap, DRV2667_CTRL_2,
					   DRV2667_GO, 0);
		if (error) {
			dev_err(&haptics->client->dev,
				"Failed to unset the GO bit: %d\n", error);
		}
	}
}

static int drv2667_init(struct drv2667_data *haptics);

/* Stock DRV2667_POLLING behaviour: if GO is still set long after the
 * (tens of ms) waveform should have finished, the engine is stuck --
 * reset the chip and re-run the probe init.
 */
static void drv2667_reset_func(struct work_struct *work)
{
	struct drv2667_data *haptics =
		container_of(work, struct drv2667_data, reset_work.work);
	unsigned int ctrl2;
	int error;

	error = regmap_read(haptics->regmap, DRV2667_CTRL_2, &ctrl2);
	if (error) {
		dev_err(&haptics->client->dev,
			"[%s] %s %d: drv2667: polling reset Failed !\n",
			__func__, __func__, __LINE__);
		return;
	}

	if (!(ctrl2 & DRV2667_GO))
		return;

	dev_warn(&haptics->client->dev,
		 "[drv2667] DRV2667_POLLING need to reset\n");

	error = regmap_write(haptics->regmap, DRV2667_CTRL_2, DRV2667_DEV_RST);
	if (error) {
		dev_err(&haptics->client->dev,
			"[%s] %s %d: drv2667: polling reset Failed !\n",
			__func__, __func__, __LINE__);
		return;
	}

	/* Datasheet power-up: wait 1 ms before the next I2C write. */
	msleep(1);

	error = drv2667_init(haptics);
	if (error)
		dev_err(&haptics->client->dev, "Failed to normal mode\n");
}

static const struct reg_sequence drv2667_init_regs[] = {
	{ DRV2667_CTRL_2, 0 },
	{ DRV2667_CTRL_1, DRV2667_25_VPP_GAIN },
	{ DRV2667_WV_SEQ_0, 1 },
	{ DRV2667_WV_SEQ_1, 0 }
};

static const struct reg_sequence drv2667_page1_init[] = {
	{ DRV2667_RAM_HDR_SZ, 0x05 },
	{ DRV2667_RAM_START_HI, 0x80 },
	{ DRV2667_RAM_START_LO, 0x06 },
	{ DRV2667_RAM_STOP_HI, 0x00 },
	{ DRV2667_RAM_STOP_LO, 0x09 },
	{ DRV2667_RAM_REPEAT_CT, 0 },
	{ DRV2667_RAM_DURATION, 0x05 },
	{ DRV2667_RAM_ENVELOPE, DRV2667_NO_ENV },
	{ DRV2667_RAM_AMP, 0x60 },
};

static int drv2667_init(struct drv2667_data *haptics)
{
	int error;

	/* Set default haptic frequency to 195Hz on Page 1 */
	haptics->frequency = 195;
	haptics->page = DRV2667_PAGE_1;

	error = regmap_register_patch(haptics->regmap,
				      drv2667_init_regs,
				      ARRAY_SIZE(drv2667_init_regs));
	if (error) {
		dev_err(&haptics->client->dev,
			"Failed to write init registers: %d\n",
			error);
		return error;
	}

	error = regmap_write(haptics->regmap, DRV2667_PAGE, haptics->page);
	if (error) {
		dev_err(&haptics->client->dev, "Failed to set page: %d\n",
			error);
		goto error_out;
	}

	error = drv2667_set_waveform_freq(haptics);
	if (error)
		goto error_page;

	error = regmap_register_patch(haptics->regmap,
				      drv2667_page1_init,
				      ARRAY_SIZE(drv2667_page1_init));
	if (error) {
		dev_err(&haptics->client->dev,
			"Failed to write page registers: %d\n",
			error);
		return error;
	}

	error = regmap_write(haptics->regmap, DRV2667_PAGE, DRV2667_PAGE_0);
	return error;

error_page:
	regmap_write(haptics->regmap, DRV2667_PAGE, DRV2667_PAGE_0);
error_out:
	return error;
}

static const struct regmap_config drv2667_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,

	.max_register = DRV2667_MAX_REG,
	.reg_defaults = drv2667_reg_defs,
	.num_reg_defaults = ARRAY_SIZE(drv2667_reg_defs),
	.cache_type = REGCACHE_NONE,
};

/* Parse a play_wave string to a magnitude (0-255).
 * Accepts both spellings because the radix is unproven on stock:
 * "0x.." or any string containing a-f/A-F is parsed as hex
 * (the stock writer sends hex bytes, e.g. "ff"), anything else as
 * decimal (e.g. "255"). Out-of-range values are clamped to 255.
 * Returns 0 on success, -EINVAL on an unparsable string.
 */
static int drv2667_parse_magnitude(const char *buf, u32 *magnitude)
{
	char tmp[16];
	size_t len;
	unsigned int val;
	int error;
	bool hex = false;
	size_t i;

	/* Copy first, then strip trailing newline / whitespace the
	 * shell echo adds.
	 */
	len = strnlen(buf, sizeof(tmp) - 1);
	if (len == 0)
		return -EINVAL;
	memcpy(tmp, buf, len);
	tmp[len] = '\0';
	while (len > 0 && (tmp[len - 1] == '\n' || tmp[len - 1] == ' ' ||
			   tmp[len - 1] == '\t' || tmp[len - 1] == '\r'))
		tmp[--len] = '\0';
	if (len == 0)
		return -EINVAL;

	if (len > 2 && tmp[0] == '0' && (tmp[1] == 'x' || tmp[1] == 'X'))
		hex = true;
	else {
		for (i = 0; i < len; i++) {
			if ((tmp[i] >= 'a' && tmp[i] <= 'f') ||
			    (tmp[i] >= 'A' && tmp[i] <= 'F')) {
				hex = true;
				break;
			}
		}
	}

	error = kstrtouint(tmp, hex ? 16 : 10, &val);
	if (error)
		return error;

	if (val > 255)
		val = 255;
	*magnitude = val;
	return 0;
}

/* Single device in the system (one DRV2667 on the tablet); the
 * kobject attribute has no device pointer, so probe stashes the
 * driver data here for show/store.
 */
static struct drv2667_data *drv2667_dev;

static ssize_t play_wave_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	struct drv2667_data *haptics = drv2667_dev;

	if (!haptics)
		return -ENODEV;
	return scnprintf(buf, PAGE_SIZE, "%u\n", haptics->magnitude);
}

static ssize_t play_wave_store(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	struct drv2667_data *haptics = drv2667_dev;
	u32 magnitude;
	int error;

	if (!haptics)
		return -ENODEV;

	error = drv2667_parse_magnitude(buf, &magnitude);
	if (error)
		return error;

	dev_info(&haptics->client->dev,
		 "[drv2667][%s] buf:%s, play_magnitude:%d\n",
		 __func__, buf, magnitude);

	mutex_lock(&haptics->lock);
	haptics->magnitude = magnitude;
	schedule_work(&haptics->work);
	mutex_unlock(&haptics->lock);

	return count;
}

/* NOTE: the kernel-side mode is 0644, not 0666: VERIFY_OCTAL_PERMISSIONS
 * rejects world-writable sysfs attributes at compile time
 * (BUILD_BUG_ON_ZERO((perms) & 2)). The stock 0666 is applied at
 * runtime by the existing vendor init lines
 * (init.target.rc: chown system system + chmod 0666
 * /sys/drv2667/play_wave), so no init change is needed.
 */
static struct kobj_attribute play_wave_attribute =
	__ATTR(play_wave, 0644, play_wave_show, play_wave_store);

static int drv2667_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct drv2667_data *haptics;
	unsigned int ctrl1;
	int error;

	haptics = devm_kzalloc(&client->dev, sizeof(*haptics), GFP_KERNEL);
	if (!haptics)
		return -ENOMEM;

	haptics->client = client;
	i2c_set_clientdata(client, haptics);
	mutex_init(&haptics->lock);
	INIT_WORK(&haptics->work, drv2667_worker);
	INIT_DELAYED_WORK(&haptics->reset_work, drv2667_reset_func);

	haptics->regmap = devm_regmap_init_i2c(client, &drv2667_regmap_config);
	if (IS_ERR(haptics->regmap)) {
		error = PTR_ERR(haptics->regmap);
		dev_err(&client->dev, "Failed to allocate register map: %d\n",
			error);
		return error;
	}

	/* Datasheet power-up: wait 1 ms before the first I2C access. */
	msleep(1);

	/* Presence check: CTRL_1 must read back CHIPID 7 (DRV2667).
	 * A missing chip fails the read; a wrong chip fails the ID.
	 * Either way probe fails cleanly and boot continues.
	 */
	error = regmap_read(haptics->regmap, DRV2667_CTRL_1, &ctrl1);
	if (error) {
		dev_err(&client->dev, "Failed to read CHIPID: %d\n", error);
		return -ENODEV;
	}
	if (((ctrl1 & DRV2667_CHIPID_MASK) >> 3) != DRV2667_CHIPID) {
		dev_err(&client->dev, "Unknown CHIPID 0x%02x, not a DRV2667\n",
			ctrl1);
		return -ENODEV;
	}

	error = drv2667_init(haptics);
	if (error) {
		dev_err(&client->dev, "[drv2667][%s] Device init failed: %d\n",
			__func__, error);
		return error;
	}

	haptics->kobj = kobject_create_and_add("drv2667", NULL);
	if (!haptics->kobj) {
		dev_err(&client->dev, "drv2667 create dir failed!\n");
		return -ENOMEM;
	}

	error = sysfs_create_file(haptics->kobj, &play_wave_attribute.attr);
	if (error) {
		dev_err(&client->dev, "drv2667 create node failed!\n");
		kobject_put(haptics->kobj);
		haptics->kobj = NULL;
		return error;
	}

	drv2667_dev = haptics;
	dev_info(&client->dev, "drv2667 probed (CHIPID 7), /sys/drv2667/play_wave ready\n");
	return 0;
}

static int drv2667_remove(struct i2c_client *client)
{
	struct drv2667_data *haptics = i2c_get_clientdata(client);

	cancel_work_sync(&haptics->work);
	cancel_delayed_work_sync(&haptics->reset_work);
	drv2667_dev = NULL;
	if (haptics->kobj) {
		sysfs_remove_file(haptics->kobj, &play_wave_attribute.attr);
		kobject_put(haptics->kobj);
	}
	return 0;
}

static int __maybe_unused drv2667_suspend(struct device *dev)
{
	struct drv2667_data *haptics = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&haptics->lock);
	cancel_work_sync(&haptics->work);
	cancel_delayed_work_sync(&haptics->reset_work);
	ret = regmap_update_bits(haptics->regmap, DRV2667_CTRL_2,
				 DRV2667_STANDBY, DRV2667_STANDBY);
	if (ret)
		dev_err(dev, "Failed to set standby mode\n");
	mutex_unlock(&haptics->lock);
	return ret;
}

static int __maybe_unused drv2667_resume(struct device *dev)
{
	struct drv2667_data *haptics = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&haptics->lock);
	ret = regmap_update_bits(haptics->regmap, DRV2667_CTRL_2,
				 DRV2667_STANDBY, 0);
	if (ret)
		dev_err(dev, "Failed to unset standby mode (active mode)\n");
	mutex_unlock(&haptics->lock);
	return ret;
}

static SIMPLE_DEV_PM_OPS(drv2667_pm_ops, drv2667_suspend, drv2667_resume);

static const struct i2c_device_id drv2667_id[] = {
	{ "drv2667", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, drv2667_id);

#ifdef CONFIG_OF
static const struct of_device_id drv2667_of_match[] = {
	{ .compatible = "ti,drv2667", },
	{ }
};
MODULE_DEVICE_TABLE(of, drv2667_of_match);
#endif

static struct i2c_driver drv2667_driver = {
	.probe		= drv2667_probe,
	.remove		= drv2667_remove,
	.driver		= {
		.name	= "drv2667",
		.of_match_table = of_match_ptr(drv2667_of_match),
		.pm	= &drv2667_pm_ops,
	},
	.id_table = drv2667_id,
};
module_i2c_driver(drv2667_driver);

MODULE_DESCRIPTION("TI DRV2667 piezo driver, sysfs play_wave (Y700 back-panel)");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dan Murphy <dmurphy@ti.com> (upstream base); Y700 sysfs rewrite");
