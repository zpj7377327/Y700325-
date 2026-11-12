// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014-2020, The Linux Foundation. All rights reserved.
 */
#include <linux/module.h>
#include <linux/suspend.h>
#include <linux/platform_device.h>
#include <linux/soc/qcom/smem_state.h>
#include <linux/interrupt.h>
#include <linux/irqreturn.h>
#include <linux/of_irq.h>
#include <linux/of.h>
#include <linux/pm_wakeup.h>
#include <linux/ktime.h>

#define PROC_AWAKE_ID 12 /* 12th bit */
#define AWAKE_BIT BIT(PROC_AWAKE_ID)
static struct qcom_smem_state *state;
static struct wakeup_source *notify_ws;

/*
 * Y700 SLPI handshake debounce (IRQ 412 standby drain):
 * The SLPI toggles the sleep-state signal ~2.5 Hz during screen-off
 * (IRQ 427), and each toggle took a full 200 ms __pm_wakeup_event hold,
 * so the APSS was held awake ~50% of the time and could never stay
 * suspended. No sensor is streaming during screen-off, so this is the
 * APSS<->SLPI sleep-state handshake itself flapping, not sensor data.
 * Coalesce rapid toggles: only take a wakeup hold if at least
 * Y700_SLPI_DEBOUNCE_MS have passed since the last one. Genuine SLPI
 * signals still wake the APSS through the smp2p wakeup IRQ (IRQ 412),
 * so this only drops redundant "stay awake" holds, not wakeups.
 */
#define Y700_SLPI_DEBOUNCE_MS	1000
#define Y700_SLPI_LOG_EVERY	100
static ktime_t y700_slpi_last_hold;
static unsigned int y700_slpi_total;
static unsigned int y700_slpi_debounced;

/**
 * sleepstate_pm_notifier() - PM notifier callback function.
 * @nb:		Pointer to the notifier block.
 * @event:	Suspend state event from PM module.
 * @unused:	Null pointer from PM module.
 *
 * This function is register as callback function to get notifications
 * from the PM module on the system suspend state.
 */
static int sleepstate_pm_notifier(struct notifier_block *nb,
				  unsigned long event, void *unused)
{
	switch (event) {
	case PM_SUSPEND_PREPARE:
		qcom_smem_state_update_bits(state, AWAKE_BIT, 0);
		break;

	case PM_POST_SUSPEND:
		qcom_smem_state_update_bits(state, AWAKE_BIT, AWAKE_BIT);
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block sleepstate_pm_nb = {
	.notifier_call = sleepstate_pm_notifier,
	.priority = INT_MAX,
};

static irqreturn_t smp2p_sleepstate_handler(int irq, void *ctxt)
{
	ktime_t now = ktime_get();
	s64 delta_ms;

	y700_slpi_total++;
	delta_ms = ktime_ms_delta(now, y700_slpi_last_hold);
	if (delta_ms >= Y700_SLPI_DEBOUNCE_MS) {
		__pm_wakeup_event(notify_ws, 200);
		y700_slpi_last_hold = now;
	} else {
		y700_slpi_debounced++;
	}

	if (y700_slpi_total % Y700_SLPI_LOG_EVERY == 0)
		pr_info("Y700 SLPI: sleepstate events=%u debounced=%u (window %d ms)\n",
			y700_slpi_total, y700_slpi_debounced,
			Y700_SLPI_DEBOUNCE_MS);
	return IRQ_HANDLED;
}

static int smp2p_sleepstate_probe(struct platform_device *pdev)
{
	int ret;
	int irq;
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;

	state = qcom_smem_state_get(&pdev->dev, 0, &ret);
	if (IS_ERR(state))
		return PTR_ERR(state);
	qcom_smem_state_update_bits(state, AWAKE_BIT, AWAKE_BIT);

	ret = register_pm_notifier(&sleepstate_pm_nb);
	if (ret) {
		dev_err(dev, "%s: power state notif error %d\n", __func__, ret);
		return ret;
	}

	notify_ws = wakeup_source_register(&pdev->dev, "smp2p-sleepstate");
	if (!notify_ws) {
		return -ENOMEM;
		goto err_ws;
	}

	irq = of_irq_get_byname(node, "smp2p-sleepstate-in");
	if (irq <= 0) {
		dev_err(dev, "failed to get irq for smp2p_sleep_state\n");
		ret = -EPROBE_DEFER;
		goto err;
	}
	dev_dbg(dev, "got smp2p-sleepstate-in irq %d\n", irq);
	ret = devm_request_threaded_irq(dev, irq, NULL,
					smp2p_sleepstate_handler,
					IRQF_ONESHOT | IRQF_TRIGGER_RISING,
					"smp2p_sleepstate", dev);
	if (ret) {
		dev_err(dev, "fail to register smp2p threaded_irq=%d\n", irq);
		goto err;
	}
	pr_info("Y700 SLPI: sleepstate debounce enabled (window %d ms, irq %d)\n",
		Y700_SLPI_DEBOUNCE_MS, irq);
	return 0;
err:
	wakeup_source_unregister(notify_ws);
err_ws:
	unregister_pm_notifier(&sleepstate_pm_nb);
	return ret;
}

static const struct of_device_id smp2p_slst_match_table[] = {
	{.compatible = "qcom,smp2p-sleepstate"},
	{},
};

static struct platform_driver smp2p_sleepstate_driver = {
	.probe = smp2p_sleepstate_probe,
	.driver = {
		.name = "smp2p_sleepstate",
		.of_match_table = smp2p_slst_match_table,
	},
};

static int __init smp2p_sleepstate_init(void)
{
	int ret;

	ret = platform_driver_register(&smp2p_sleepstate_driver);
	if (ret) {
		pr_err("%s: register failed %d\n", __func__, ret);
		return ret;
	}

	return 0;
}

module_init(smp2p_sleepstate_init);
MODULE_DESCRIPTION("SMP2P SLEEP STATE");
MODULE_LICENSE("GPL v2");
