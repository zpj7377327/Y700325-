// SPDX-License-Identifier: GPL-2.0
/*
 * y700_hw_info.c - Lenovo Legion Y700 (TB-9707F) port on OPPO kona tree.
 *
 * 提供 Lenovo 内核符号 register_hardware_info（goodix/es7210/pi2dpx1217 等
 * Y700 专有驱动注册硬件型号用），以及 OPPO owakelock qcom 变体缺失的
 * get_cached_platform_id（gpiolib/i2c-geni/binder/printk 引用）。
 */
#include <linux/module.h>
#include <linux/string.h>
#include <linux/mutex.h>

static DEFINE_MUTEX(hw_info_lock);
static char hw_info_name[64];
static char hw_info_model[64];

/*
 * Lenovo 语义：驱动把自己的子系统名 + 硬件型号注册进来，供用户态/工厂模式读取。
 * OPPO 树里没有对应实现，这里给一个完整的落地版本。
 */
int register_hardware_info(const char *name, const char *model)
{
	if (!name || !model)
		return -EINVAL;

	mutex_lock(&hw_info_lock);
	strlcpy(hw_info_name, name, sizeof(hw_info_name));
	strlcpy(hw_info_model, model, sizeof(hw_info_model));
	mutex_unlock(&hw_info_lock);

	return 0;
}
EXPORT_SYMBOL(register_hardware_info);

/* 说明：get_cached_platform_id 由 oplus_wakelock_profiler.c 提供（stock 配置
 * 编译该文件），此处不再重复定义，避免链接冲突。 */

/* 说明：opticalfp_irq_handler_register 由 oplus_fp_common.c 提供（stock 配置
 * CONFIG_OPLUS_FINGERPRINT 相关开启）。此处不再重复定义，避免链接冲突。 */
