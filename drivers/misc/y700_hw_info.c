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

/*
 * get_cached_platform_id：OPPO 树里它只在通用 oplus_wakelock_profiler.c 里，
 * 但该 Makefile 在 CONFIG_OPLUS_POWER_QCOM=y 时只编 qcom 变体文件，不编通用
 * 文件，导致 irq-gic/ipcc/nwpower 链接不到它。这里按本机 SoC 补一个：
 * Y700 1st gen 是 kona（sm8250），通用文件里 KONA 定义为 1，调用处只比较
 * == KONA 做唤醒原因命名，所以返回 1 与原语义一致，不是随意打桩。
 */
int get_cached_platform_id(void)
{
	return 1; /* KONA */
}

/* 说明：opticalfp_irq_handler_register 由 oplus_fp_common.c 提供（stock 配置
 * CONFIG_OPLUS_FINGERPRINT 相关开启）。此处不再重复定义，避免链接冲突。 */
