/*
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/err.h>
#include <linux/of_device.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/backlight.h>

#include "ktz8866_init.h"

#define BL_I2C_ADDRESS			  0x11


#define LCD_ID_NAME "ktz8866"

/*****************************************************************************
 * GLobal Variable
 *****************************************************************************/
static struct i2c_client *g_i2c_client = NULL;
static struct ktz8866_gpio_info gpio_arrays[32];
static DEFINE_MUTEX(read_lock);

/*****************************************************************************
 * Extern Area
 *****************************************************************************/

static int lcd_bl_write_byte(unsigned char addr, unsigned char value)
{
    int ret = 0;
    unsigned char write_data[2] = {0};

    write_data[0] = addr;
    write_data[1] = value;

    if (NULL == g_i2c_client) {
	pr_err("[LCD][BL] g_i2c_client is null!!\n");
	return -EINVAL;
    }
    ret = i2c_master_send(g_i2c_client, write_data, 2);

    if (ret < 0)
	pr_err("[LCD][BL] i2c write data fail !!\n");

    return ret;
}
int lcd_bl_enable(bool enable)
{
	if (enable) {
		usleep_range(30000,30001);/* keep mipi on-bl on 40ms,now 42ms */
		lcd_bl_write_byte(0x02, 0xD3);/* BL_CFG1 OVP=34V Exponential dimming PWM Enable */
		lcd_bl_write_byte(0x03, 0xCD);/* diming 256ms PWM_HYST 10 LSBs*/
		lcd_bl_write_byte(0x11, 0xB7);/* BL_OPTION2 10uH BL_CURRENT_LIMIT 2.5A*/
		lcd_bl_write_byte(0x15, 0xC1);/* Backlight Full-scale LED Current 24.4mA/ PWM transent time 4ms*/
		lcd_bl_write_byte(0x08, 0x5F);/* BL enabled and Current sink 1/2/3/4 /5 enabled£»*/
	} else {
		lcd_bl_write_byte(0x08, 0x00);/* Disable BL */
	}
	return 0;
}

extern unsigned int dis_set_first_level;
int lcd_bl_set_led_brightness(int value)//for set bringhtness
{
	unsigned int mapping_value;
	unsigned int scaled_value;
	/* Y700: track the KTZ8866 IC enable state directly, not the previous
	 * brightness value. The old prev_value tracking raced when the user
	 * slid the brightness bar rapidly (or auto-brightness changed it
	 * quickly): a 0 -> non-zero transition could be missed if two calls
	 * interleaved, leaving the IC disabled (reg 0x08 = 0x00) while the
	 * brightness value was non-zero — screen stuck black, brightness
	 * sysfs stuck at 1, and the watchdog eventually rebooted.
	 * NOTE: no lock here on purpose. This function can be called from
	 * atomic context during boot/mode-set (v5r hung at the first screen
	 * because a mutex_lock slept there — though v5t later proved the
	 * haptic LED name collision was the actual hang cause, not the
	 * mutex). A redundant lcd_bl_enable(true) from two racing calls is
	 * harmless — it just rewrites the same 5 regs — and the flag stays
	 * correct enough: enable whenever the IC is off and brightness > 0,
	 * disable on brightness 0. */
	static bool bl_ic_enabled = false;
	if (value < 0) {
		pr_err("%d %s --lyd invalid value=%d\n", __LINE__, __func__, value);
		return 0;
	}
	/* Y700: backlight_buf has 2048 entries (0-2047), but the value arriving
	 * here is 0-255 (sysfs backlight range). Without scaling, max brightness
	 * 255 only reaches table[255]=933 (~45% of the 2047 full scale), so the
	 * screen is dim even at "max". Scale 0-255 -> 0-2047 before indexing.
	 * If value is already >255 (framework 0-2047 range), use it as-is.
	 * Clamp to 2047 to guard against any out-of-range input. */
	if (value > 255)
		scaled_value = (unsigned int)value;
	else
		scaled_value = (unsigned int)value * 2047 / 255;
	if (scaled_value > 2047)
		scaled_value = 2047;
	mapping_value = backlight_buf[scaled_value];
	pr_info("%s:hyper bl = %d, mapping value = %d\n", __func__, value, mapping_value);

	if (value > 0) {
		/* Y700: re-enable the backlight IC whenever it is off and a
		 * non-zero brightness is requested. dis_set_first_level is only
		 * set in dsi_panel_power_on, not on a blank/unblank cycle, so
		 * after screen-off (value 0 -> IC disabled via lcd_bl_enable
		 * (false)) the next screen-on would write only the brightness
		 * regs while the IC stays disabled (reg 0x08 = 0x00), leaving
		 * the screen stuck dim. The bl_ic_enabled flag tracks the IC
		 * state directly, so rapid brightness changes cannot desync it. */
		if (!bl_ic_enabled || dis_set_first_level) {
			lcd_bl_enable(true); /* BL enabled and Current sink 1/2/3/4/5 enabled */
			dis_set_first_level = 0;
			bl_ic_enabled = true;
			pr_info("%d %s Y700 BL re-enable\n", __LINE__, __func__);
		}
		lcd_bl_write_byte(0x04, mapping_value & 0x07); /* lsb */
		lcd_bl_write_byte(0x05, (mapping_value >> 3) & 0xFF); /* msb */
	}

	if (value == 0) {
		lcd_bl_write_byte(0x04, 0x00);// lsb
		lcd_bl_write_byte(0x05, 0x00);// msb
		lcd_bl_enable(false); /* BL disabled and Current sink 1/2/3/4/5 disabled */
		bl_ic_enabled = false;
	}
	return 0;
}
EXPORT_SYMBOL(lcd_bl_set_led_brightness);

int lcd_set_bias(bool enable)
{
	pr_info("--lcd, enter lcd_disable_bias function,value = %d", enable);
	if (enable) {
		lcd_bl_write_byte(0x0C, 0x2C);/* LCD_BOOST_CFG 6.2V*/
		lcd_bl_write_byte(0x0D, 0x24);/* OUTP_CFG OUTP = 5.8V */
		lcd_bl_write_byte(0x0E, 0x24);/* OUTN_CFG OUTN = -5.8V */
		lcd_bl_write_byte(0x09, 0x9C);/* enable OUTP */
	    	usleep_range(5000,5001);
		lcd_bl_write_byte(0x09, 0x9E);/* enable OUTN */
		usleep_range(13000,13001);/* avee-reset >10ms */
	} else {
		lcd_bl_write_byte(0x09, 0x9C);/* Disable OUTN */
		usleep_range(5000,5001);
		lcd_bl_write_byte(0x09, 0x98);/* Disable OUTP */
		usleep_range(50000,50001);
	}
	return 0;
}
EXPORT_SYMBOL(lcd_set_bias);

static const struct of_device_id lcd_ktz8866_of_match[] = {
    { .compatible = "ktz,ktz8866", },
    {},
};

static int ktz8866_pwr_parse_gpios(struct device *dev)
{
	int rc = 0;
 	int index = 0;
	struct device_node *np = dev->of_node;
	gpio_arrays[KTZ8866_HW_EN].gpio_name = "ktz8866_hw_en";

	for(index = 0; index < LCD_PWR_GPIO_MAX; index++) {
		gpio_arrays[index].gpio_num = of_get_named_gpio(np, gpio_arrays[index].gpio_name, 0);
		if (!gpio_is_valid(gpio_arrays[index].gpio_num)) {
			rc = gpio_arrays[index].gpio_num;
			pr_err("[%s] failed get gpio from dts, rc=%d\n", __func__,rc);
		} else {
			rc = gpio_request(gpio_arrays[index].gpio_num, gpio_arrays[index].gpio_name);
			if(rc) {
				pr_err("request %s gpio %d failed,rc = %d\n", gpio_arrays[index].gpio_name, gpio_arrays[index].gpio_num,rc);
			} else {
				pr_err("names:%s gpio %d requested success!\n", gpio_arrays[index].gpio_name, gpio_arrays[index].gpio_num);
			}
		}
	}
	return rc;
}

int turn_on_ktz8866_hw_en(bool on)
{
	int ret;
	u8 value  = on ? 1 : 0;

	if(gpio_is_valid(gpio_arrays[KTZ8866_HW_EN].gpio_num)) {
		ret = gpio_direction_output(gpio_arrays[KTZ8866_HW_EN].gpio_num, value);
		if(ret){
			pr_err("failed to set %s gpio %d, ret = %d\n", gpio_arrays[KTZ8866_HW_EN].gpio_name, value, ret);
			return ret;
		}
	} else {
		pr_err("KTZ8866_HW_EN is not vaild,KTZ8866_HW_EN is %d\n", gpio_arrays[KTZ8866_HW_EN].gpio_num);
	}

	return 0;
}
EXPORT_SYMBOL(turn_on_ktz8866_hw_en);

static int lcd_power_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int ret;

	if(!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_warn(&client->dev, "xx adapter does not support I2C\n");
		return -EIO;
	} else {
		g_i2c_client = client;
	}

	//parse dts
	if (client->dev.of_node) {
		ret = ktz8866_pwr_parse_gpios(&client->dev);
		if(ret){
			dev_err(&client->dev,"failed to parse dts\n");
			return -EINVAL;
		}

	}
	//turn on hw_en
	ret = turn_on_ktz8866_hw_en(true);
	if(ret) {
		pr_err("failed to turn on hwen!\n");
	}
    return 0;
}

static int lcd_power_remove(struct i2c_client *client)
{
    g_i2c_client = NULL;
    i2c_unregister_device(client);

    return 0;
}

/************************************************************
Attention:
Althouh i2c_bus do not use .id_table to match, but it must be defined,
otherwise the probe function will not be executed!
************************************************************/
static struct i2c_driver lcd_ktz8866_i2c_driver = {
    .probe = lcd_power_probe,
    .remove = lcd_power_remove,
    .driver = {
	.owner = THIS_MODULE,
	.name = LCD_ID_NAME,
	.of_match_table = lcd_ktz8866_of_match,

    },
};


static int __init lcd_ktz8866_init(void)
{
	pr_info("lcd_ktz8866_init\n");

	if (i2c_add_driver(&lcd_ktz8866_i2c_driver)) {
		pr_err("[LCD][BL] Failed to register lcd_ktz8866_i2c_driver!\n");
		return -EINVAL;
	}

    return 0;
}

static void __exit lcd_ktz8866_exit(void)
{
    i2c_del_driver(&lcd_ktz8866_i2c_driver);
}

subsys_initcall(lcd_ktz8866_init);
module_exit(lcd_ktz8866_exit);

MODULE_AUTHOR("<xx>");
MODULE_DESCRIPTION("QCOM ktz8866 Driver");
MODULE_LICENSE("GPL");

