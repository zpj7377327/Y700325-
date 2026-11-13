/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Y700 AW8697 haptic driver (clean rewrite, 2026-11-12)
 *
 * Lenovo Legion Y700 1st gen (TB-9707F, Pike/SM8250): two AW8697 LRA
 * motors, right on I2C bus 6 addr 0x5A (awinic,haptic_hv_r, IRQ GPIO 26),
 * left on I2C bus 7 addr 0x5B (awinic,haptic_hv_l, IRQ GPIO 60),
 * SHARED reset GPIO 61 (the left DT node carries "share_reset").
 *
 * This replaces the OPPO aw8697.c (14k lines) + haptic_hv.c (8k lines).
 * What was wrong with those (full list in
 * ~/workspace/work-y700/aw8697-rewrite-problems.md):
 *  - a single instance + global mirror (g_aw8697 / aw8697_slave); the
 *    left motor never probed, never read, never got an IRQ;
 *  - the v6o f0=1754 "fix" lived in f0_calibration(), which is never
 *    called at boot (#ifndef OPLUS_FEATURE_CHG_BASIC, but the macro IS
 *    defined), so v6p actually ran f0=1700 / TRIM_LRA=0;
 *  - the "CONT" play path is dead: the work routine forces RAM_MODE,
 *    so playback is RAM_LOOP of waveform 9, not CONT mode;
 *  - the Y700 DT tuning (f0_pre, cont_td, cont_zc_thr, ...) is ignored;
 *    device_id defaults to 815 (an OPPO phone code), not the chip 0x97;
 *  - RAM waveforms depend on an async firmware load of a file that may
 *    not exist; 2MB vmalloc + RichTap mmap + haptic-audio + gun + misc
 *    + proc + a second, never-binding driver (haptic_hv) all compiled in.
 *
 * What this driver keeps (the v6p behaviour that actually works,
 * register sequences copied from the OPPO driver, not reinvented):
 *  - init register sequence (STANDBY, PWM 24K, BSTDBG1/2/3, TSET,
 *    R_SPARE, ANADBG IOC 4P65A, peak-cur 3P5A, motor-protect off,
 *    manual boost, VBAT HW compensation);
 *  - index=1 default waveform, activate plays SINE_CYCLE waveform 9 in
 *    RAM_LOOP with an infinite loop, stopped by an hrtimer after
 *    `duration` ms; brightness plays the current waveform in RAM mode;
 *  - both LED nodes "vibrator" + "vibrator_r" (the HAL requires both);
 *  - gain read from DATDBG, clamped to 0x80 on activate; RAM vbat
 *    compensation (gain scaled by battery voltage) on the activate path;
 *  - async RAM firmware load of "aw8697_haptic.bin".
 *
 * What it changes:
 *  - one controller, two motor slots, each with its own state (f0,
 *    gain, vmax, seq); writes are issued to each motor explicitly,
 *    never mirrored from a primary read;
 *  - f0 is measured at probe (BEMF), fallback 1780 (not hidden in an
 *    uncalled function), and re-measured in the background every
 *    5 minutes per motor, so it tracks the real resonance like
 *    v6p's runtime f0 (~1777, varies per read) instead of staying
 *    fixed; f0_pre and the CONT parameters are read
 *    from DT (fallback: the stock values); TRIM_LRA is not
 *    calibrated (stays at its power-on default 0);
 *  - the left motor gets its own IRQ and its own status reads;
 *  - no RTP, no RichTap, no haptic audio, no gun, no misc/proc,
 *    no TIMED_OUTPUT, no OPLUS #ifdef maze, no haptic_hv/aw8692x.
 *
 * Behaviour NOT verified here (state it plainly): RichTap waveforms
 * (the HAL reports "aac richtap not support" on all v6 builds; wiring
 * them in is a separate step), and the exact contents of the chip RAM
 * when aw8697_haptic.bin is absent from /vendor/firmware.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/leds.h>
#include <linux/hrtimer.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/gpio.h>

/* aw8697_reg.h uses AW8697_REG_MAX (defined in the OPPO aw8697.h,
 * which this rewrite deliberately does not include). */
#define AW8697_REG_MAX			0xff
#include "aw8697_reg.h"

/* ------------------------------------------------------------------ */
/* Y700 fixed values (stock DT / stock lra_f0)                          */
/* ------------------------------------------------------------------ */
#define Y700_AW8697_CHIPID		0x97
#define Y700_F0				1780	/* v6p measured ~1777 (BEMF); fallback if probe measure fails */
#define Y700_VMAX				0x18	/* v6p boost level (BSTDBG4 = 0x18<<1 = 0x30) */
#define Y700_F0_PRE			1700	/* DT f0_pre = 0x6a4 */
#define Y700_F0_COEFF			260	/* DT aw869x_f0_coeff = 0x104 */
#define Y700_F0_CALI_PERCENT		7	/* DT f0_cali_percent */
#define Y700_CONT_TD			0x009a	/* DT aw869x_cont_td */
#define Y700_CONT_ZC_THR		0x0ff1	/* DT aw869x_cont_zc_thr */
#define Y700_CONT_NUM_BRK		3	/* DT aw869x_cont_num_brk */
#define Y700_CONT_DRV_LVL		0x69	/* DT aw869x_cont_drv_lev */
#define Y700_CONT_DRV_LVL_OV		0x7d	/* DT aw869x_cont_drv_lvl_ov */
#define Y700_INDEX_DEFAULT		1	/* stock default waveform index */
#define Y700_WAVEFORM_SINE_CYCLE	9	/* SINE_CYCLE, binary-proven */
#define Y700_GAIN_CLAMP		0x80	/* RAM_VBAT_COMP_GAIN */
#define Y700_RAM_FW_NAME		"aw8697_haptic.bin"
#define Y700_SEQUENCER_SIZE		8
/* f0 re-measurement period. v6p's runtime f0 drifts ~1.3 Hz/min
 * (1770 -> 1782 over 9 min), so 5 min bounds the uncorrected drift
 * to ~7 Hz -- far inside the LRA bandwidth -- while keeping the
 * intrusive 500 ms-per-motor calibration buzz rare (at most once
 * per 5 min, only when idle). */
#define Y700_F0_REMEASURE_MS		(5 * 60 * 1000)

#define Y700_VBAT_REFER		4400
#define Y700_VBAT_MIN			3000
#define Y700_VBAT_MAX			4500

#define Y700_I2C_RETRIES		5
#define Y700_I2C_RETRY_DELAY_MS		2
#define Y700_CHIPID_RETRIES		5
#define Y700_CHIPID_RETRY_DELAY_MS	2

/* play modes (SYSCTRL PLAY_MODE field) */
enum y700_play_mode {
	Y700_MODE_STANDBY = 0,
	Y700_MODE_RAM = 1,
	Y700_MODE_CONT = 4,
	Y700_MODE_RAM_LOOP = 5,
};

/* ------------------------------------------------------------------ */
/* Per-motor state (each motor has its own)                             */
/* ------------------------------------------------------------------ */
struct y700_motor {
	struct i2c_client *client;
	int irq_gpio;
	int irq;
	u8 chipid;
	int f0;			/* measured at probe, re-measured in background, fallback 1780 */
	int f0_pre;		/* 1700 */
	int gain;		/* DATDBG at init */
	int vmax;		/* BSTDBG4 at init */
	u8 seq[Y700_SEQUENCER_SIZE];
	bool ram_init;
};

/* ------------------------------------------------------------------ */
/* Single controller driving both motors                                */
/* ------------------------------------------------------------------ */
struct y700_haptic {
	struct device *dev;
	struct y700_motor motor[2];	/* [0] right 0x5A, [1] left 0x5B */
	int nmotors;			/* 1 or 2 */
	int reset_gpio;

	struct mutex lock;		/* serialises play/stop/config */

	struct led_classdev cdev;	/* "vibrator"     */
	struct led_classdev cdev_r;	/* "vibrator_r"   */

	struct hrtimer timer;
	struct work_struct work;	/* vibrator_work: stop + (re)start */
	struct delayed_work f0_work;	/* periodic BEMF f0 re-measurement */
	int state;
	int duration;			/* ms, from sysfs */
	int amplitude;
	int index;			/* default 1 */
	int activate_mode;		/* stored; play path always uses RAM_LOOP */
	bool ram_vbat_comp;		/* true after init */
	int vbat;			/* mV, last read (primary motor) */
	int cont_td;
	int cont_zc_thr;
	int cont_num_brk;
};

static inline struct y700_haptic *to_y700(struct led_classdev *cdev)
{
	return container_of(cdev, struct y700_haptic, cdev);
}

static inline struct y700_haptic *to_y700_r(struct led_classdev *cdev)
{
	return container_of(cdev, struct y700_haptic, cdev_r);
}

/* ------------------------------------------------------------------ */
/* I2C helpers (per motor; "both" variants issue to each motor)          */
/* ------------------------------------------------------------------ */
static int y700_i2c_write(struct y700_motor *m, u8 reg, u8 val)
{
	int ret = -EIO;
	unsigned char cnt = 0;

	while (cnt < Y700_I2C_RETRIES) {
		ret = i2c_smbus_write_byte_data(m->client, reg, val);
		if (ret >= 0)
			break;
		cnt++;
		msleep(Y700_I2C_RETRY_DELAY_MS);
	}
	if (ret < 0)
		pr_err("%s: i2c_write 0x%02x=0x%02x err=%d\n",
		       __func__, reg, val, ret);
	return ret;
}

static int y700_i2c_read(struct y700_motor *m, u8 reg, u8 *val)
{
	int ret = -EIO;
	unsigned char cnt = 0;

	while (cnt < Y700_I2C_RETRIES) {
		ret = i2c_smbus_read_byte_data(m->client, reg);
		if (ret >= 0) {
			*val = ret;
			break;
		}
		cnt++;
		msleep(Y700_I2C_RETRY_DELAY_MS);
	}
	if (ret < 0)
		pr_err("%s: i2c_read 0x%02x err=%d\n", __func__, reg, ret);
	return ret;
}

/*
 * Read-modify-write on THIS motor's own register. The old driver read
 * the primary and wrote the result to both motors, which corrupts the
 * slave whenever the two chips' registers diverge (e.g. interrupt
 * status bits).
 */
static int y700_i2c_write_bits(struct y700_motor *m, u8 reg,
			       unsigned int mask, u8 val)
{
	u8 reg_val = 0;
	int ret;

	ret = y700_i2c_read(m, reg, &reg_val);
	if (ret < 0)
		return ret;
	reg_val &= mask;
	reg_val |= val;
	return y700_i2c_write(m, reg, reg_val);
}

static int y700_i2c_writes(struct y700_motor *m, u8 reg, u8 *buf,
			   unsigned int len)
{
	u8 *data;
	int ret;

	data = kmalloc(len + 1, GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data[0] = reg;
	memcpy(&data[1], buf, len);
	ret = i2c_master_send(m->client, data, len + 1);
	if (ret < 0)
		pr_err("%s: i2c master send err=%d\n", __func__, ret);
	kfree(data);
	return ret;
}

/* Issue the same single-byte write to every bound motor. */
static void y700_both_write(struct y700_haptic *h, u8 reg, u8 val)
{
	int i;

	for (i = 0; i < h->nmotors; i++)
		y700_i2c_write(&h->motor[i], reg, val);
}

/* ------------------------------------------------------------------ */
/* Motor control (per motor)                                            */
/* ------------------------------------------------------------------ */
static int y700_motor_softreset(struct y700_motor *m)
{
	y700_i2c_write(m, AW8697_REG_ID, 0xAA);
	usleep_range(3000, 3500);
	return 0;
}

static void y700_motor_interrupt_clear(struct y700_motor *m)
{
	u8 reg_val = 0;

	y700_i2c_read(m, AW8697_REG_SYSINT, &reg_val);
}

/* Bring the chip to ACTIVE (out of standby) and clear interrupts. */
static int y700_motor_active(struct y700_motor *m)
{
	y700_i2c_write_bits(m, AW8697_REG_SYSCTRL,
			    AW8697_BIT_SYSCTRL_WORK_MODE_MASK,
			    AW8697_BIT_SYSCTRL_ACTIVE);
	y700_motor_interrupt_clear(m);
	y700_i2c_write_bits(m, AW8697_REG_SYSINTM,
			    AW8697_BIT_SYSINTM_UVLO_MASK,
			    AW8697_BIT_SYSINTM_UVLO_EN);
	return 0;
}

/*
 * Play mode. The old driver had four near-identical 20-line copies of
 * this (RAM / RAM_LOOP / RTP / TRIG). RAM and RAM_LOOP program the same
 * SYSCTRL bits -- the "loop" lives in the WAVLOOP register, not here --
 * so they share one branch. auto_boost is always off on the Y700
 * (manual boost), so the auto-boost branches are gone.
 */
static int y700_motor_play_mode(struct y700_motor *m, int mode)
{
	switch (mode) {
	case Y700_MODE_STANDBY:
		y700_i2c_write_bits(m, AW8697_REG_SYSINTM,
				    AW8697_BIT_SYSINTM_UVLO_MASK,
				    AW8697_BIT_SYSINTM_UVLO_OFF);
		y700_i2c_write_bits(m, AW8697_REG_SYSCTRL,
				    AW8697_BIT_SYSCTRL_WORK_MODE_MASK,
				    AW8697_BIT_SYSCTRL_STANDBY);
		break;
	case Y700_MODE_RAM:
	case Y700_MODE_RAM_LOOP:
		y700_i2c_write_bits(m, AW8697_REG_SYSCTRL,
				    AW8697_BIT_SYSCTRL_PLAY_MODE_MASK,
				    AW8697_BIT_SYSCTRL_PLAY_MODE_RAM);
		y700_i2c_write_bits(m, AW8697_REG_SYSCTRL,
				    AW8697_BIT_SYSCTRL_BST_MODE_MASK,
				    AW8697_BIT_SYSCTRL_BST_MODE_BYPASS);
		y700_motor_active(m);
		y700_i2c_write_bits(m, AW8697_REG_SYSCTRL,
				    AW8697_BIT_SYSCTRL_BST_MODE_MASK,
				    AW8697_BIT_SYSCTRL_BST_MODE_BOOST);
		mdelay(2);
		break;
	case Y700_MODE_CONT:
		y700_i2c_write_bits(m, AW8697_REG_SYSCTRL,
				    AW8697_BIT_SYSCTRL_PLAY_MODE_MASK,
				    AW8697_BIT_SYSCTRL_PLAY_MODE_CONT);
		y700_i2c_write_bits(m, AW8697_REG_SYSCTRL,
				    AW8697_BIT_SYSCTRL_BST_MODE_MASK,
				    AW8697_BIT_SYSCTRL_BST_MODE_BYPASS);
		y700_motor_active(m);
		break;
	default:
		pr_err("%s: play mode %d err\n", __func__, mode);
		break;
	}
	return 0;
}

static int y700_motor_play_go(struct y700_motor *m, bool on)
{
	if (on) {
		y700_i2c_write_bits(m, AW8697_REG_GO,
				    AW8697_BIT_GO_MASK, AW8697_BIT_GO_ENABLE);
		mdelay(2);
	} else {
		y700_i2c_write_bits(m, AW8697_REG_GO,
				    AW8697_BIT_GO_MASK, AW8697_BIT_GO_DISABLE);
	}
	return 0;
}

static int y700_motor_set_wav_seq(struct y700_motor *m, u8 wav, u8 seq)
{
	return y700_i2c_write(m, AW8697_REG_WAVSEQ1 + wav, seq);
}

static int y700_motor_set_wav_loop(struct y700_motor *m, u8 wav, u8 loop)
{
	u8 tmp;

	if (wav % 2) {
		tmp = loop << 0;
		return y700_i2c_write_bits(m, AW8697_REG_WAVLOOP1 + (wav / 2),
					   AW8697_BIT_WAVLOOP_SEQNP1_MASK, tmp);
	}
	tmp = loop << 4;
	return y700_i2c_write_bits(m, AW8697_REG_WAVLOOP1 + (wav / 2),
				   AW8697_BIT_WAVLOOP_SEQN_MASK, tmp);
}

static int y700_motor_set_repeat_wav_seq(struct y700_motor *m, u8 seq)
{
	y700_motor_set_wav_seq(m, 0x00, seq);
	y700_motor_set_wav_loop(m, 0x00, AW8697_BIT_WAVLOOP_INIFINITELY);
	return 0;
}

static int y700_motor_set_gain(struct y700_motor *m, u8 gain)
{
	return y700_i2c_write(m, AW8697_REG_DATDBG, gain);
}

static int y700_motor_set_bst_peak_cur(struct y700_motor *m, u8 peak_cur)
{
	peak_cur &= 0x07;	/* BSTCFG_PEAKCUR limit */
	return y700_i2c_write_bits(m, AW8697_REG_BSTCFG,
				   AW8697_BIT_BSTCFG_PEAKCUR_MASK, peak_cur);
}

static int y700_motor_set_pwm_24k(struct y700_motor *m)
{
	return y700_i2c_write_bits(m, AW8697_REG_PWMDBG,
				   AW8697_BIT_PWMDBG_PWM_MODE_MASK,
				   AW8697_BIT_PWMDBG_PWM_24K);
}

/* f0 preset registers, from f0_pre (1700) and the 260 coefficient. */
static int y700_motor_set_f0_preset(struct y700_motor *m)
{
	unsigned int f0_reg;

	f0_reg = 1000000000U / ((unsigned int)m->f0_pre * Y700_F0_COEFF);
	y700_i2c_write(m, AW8697_REG_F_PRE_H, (u8)((f0_reg >> 8) & 0xff));
	y700_i2c_write(m, AW8697_REG_F_PRE_L, (u8)((f0_reg >> 0) & 0xff));
	return 0;
}

/* Oscillator self-calibration (RAMINIT + DIAG_GO), runs at init. */
static int y700_motor_os_calibration(struct y700_motor *m)
{
	unsigned int cont = 2000;
	u8 reg_val = 0;

	y700_i2c_write_bits(m, AW8697_REG_SYSCTRL,
			    AW8697_BIT_SYSCTRL_RAMINIT_MASK,
			    AW8697_BIT_SYSCTRL_RAMINIT_EN);
	y700_i2c_write_bits(m, AW8697_REG_DETCTRL,
			    AW8697_BIT_DETCTRL_DIAG_GO_MASK,
			    AW8697_BIT_DETCTRL_DIAG_GO_ENABLE);
	while (1) {
		y700_i2c_read(m, AW8697_REG_DETCTRL, &reg_val);
		if ((reg_val & 0x01) == 0 || cont == 0)
			break;
		cont--;
	}
	y700_i2c_write_bits(m, AW8697_REG_SYSCTRL,
			    AW8697_BIT_SYSCTRL_RAMINIT_MASK,
			    AW8697_BIT_SYSCTRL_RAMINIT_OFF);
	return 0;
}

static int y700_motor_get_vbat(struct y700_motor *m)
{
	unsigned int cont = 2000;
	u8 reg_val = 0;
	int vbat;

	y700_i2c_write_bits(m, AW8697_REG_SYSCTRL,
			    AW8697_BIT_SYSCTRL_RAMINIT_MASK,
			    AW8697_BIT_SYSCTRL_RAMINIT_EN);
	y700_i2c_write_bits(m, AW8697_REG_DETCTRL,
			    AW8697_BIT_DETCTRL_VBAT_GO_MASK,
			    AW8697_BIT_DETCTRL_VABT_GO_ENABLE);
	while (1) {
		y700_i2c_read(m, AW8697_REG_DETCTRL, &reg_val);
		if ((reg_val & 0x02) == 0 || cont == 0)
			break;
		cont--;
	}
	y700_i2c_read(m, AW8697_REG_VBATDET, &reg_val);
	vbat = 6100 * reg_val / 256;
	if (vbat > Y700_VBAT_MAX)
		vbat = Y700_VBAT_MAX;
	if (vbat < Y700_VBAT_MIN)
		vbat = Y700_VBAT_MIN;
	y700_i2c_write_bits(m, AW8697_REG_SYSCTRL,
			    AW8697_BIT_SYSCTRL_RAMINIT_MASK,
			    AW8697_BIT_SYSCTRL_RAMINIT_OFF);
	return vbat;
}

/*
 * Stop one motor: GO off, wait for standby (up to 400 ms), then force
 * standby. On timeout the chip is soft-reset and re-initialised, as
 * the old driver did.
 */
static int y700_motor_stop(struct y700_haptic *h, struct y700_motor *m);
static int y700_motor_init_regs_common(struct y700_haptic *h,
				       struct y700_motor *m);

static int y700_motor_stop_delay(struct y700_haptic *h, struct y700_motor *m)
{
	u8 reg_val = 0;
	unsigned int cnt = 200;

	while (cnt--) {
		y700_i2c_read(m, AW8697_REG_GLB_STATE, &reg_val);
		if ((reg_val & 0x0f) == 0x00)
			return 0;
		mdelay(2);
	}
	pr_err("%s: motor 0x%02x did not enter standby, softreset\n",
	       __func__, m->client->addr);
	y700_motor_softreset(m);
	/* Re-init the registers (not f0/index/gain -- those are kept). */
	y700_motor_init_regs_common(h, m);
	return 0;
}

static int y700_motor_stop(struct y700_haptic *h, struct y700_motor *m)
{
	y700_motor_play_go(m, false);
	y700_motor_stop_delay(h, m);
	y700_motor_play_mode(m, Y700_MODE_STANDBY);
	return 0;
}

static int y700_motor_play_repeat_seq(struct y700_motor *m)
{
	y700_motor_play_mode(m, Y700_MODE_RAM_LOOP);
	y700_motor_play_go(m, true);
	return 0;
}

static int y700_motor_play_wav_seq(struct y700_motor *m)
{
	y700_motor_play_mode(m, Y700_MODE_RAM);
	y700_motor_play_go(m, true);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Controller-level play/stop (both motors, same parameters)            */
/* ------------------------------------------------------------------ */
static void y700_stop(struct y700_haptic *h)
{
	int i;

	for (i = 0; i < h->nmotors; i++)
		y700_motor_stop(h, &h->motor[i]);
}

static void y700_play_repeat_seq(struct y700_haptic *h)
{
	int i;

	for (i = 0; i < h->nmotors; i++)
		y700_motor_play_repeat_seq(&h->motor[i]);
}

static void y700_play_wav_seq(struct y700_haptic *h)
{
	int i;

	for (i = 0; i < h->nmotors; i++)
		y700_motor_play_wav_seq(&h->motor[i]);
}

static void y700_set_repeat_wav_seq(struct y700_haptic *h, u8 seq)
{
	int i;

	for (i = 0; i < h->nmotors; i++)
		y700_motor_set_repeat_wav_seq(&h->motor[i], seq);
}

/*
 * RAM vbat compensation. flag=true (activate path): scale the gain by
 * battery voltage, as the old driver did; flag=false (brightness
 * path): plain gain. vbat is read once from the primary motor -- both
 * motors share the same battery.
 */
static void y700_ram_vbat_comp(struct y700_haptic *h, bool flag)
{
	int i;
	int temp_gain;

	if (flag && h->ram_vbat_comp) {
		h->vbat = y700_motor_get_vbat(&h->motor[0]);
		for (i = 0; i < h->nmotors; i++) {
			temp_gain = h->motor[i].gain * Y700_VBAT_REFER / h->vbat;
			if (temp_gain > (128 * Y700_VBAT_REFER / Y700_VBAT_MIN))
				temp_gain = 128 * Y700_VBAT_REFER / Y700_VBAT_MIN;
			y700_motor_set_gain(&h->motor[i], temp_gain);
		}
	} else {
		for (i = 0; i < h->nmotors; i++)
			y700_motor_set_gain(&h->motor[i], h->motor[i].gain);
	}
}

/* Common register sequence (probe init and post-softreset re-init).
 * Same writes as v6p haptic_init / haptic_reset_init. */
static int y700_motor_init_regs_common(struct y700_haptic *h,
				       struct y700_motor *m)
{
	y700_motor_play_mode(m, Y700_MODE_STANDBY);
	y700_motor_set_pwm_24k(m);

	y700_i2c_write(m, AW8697_REG_BSTDBG1, 0x30);
	y700_i2c_write(m, AW8697_REG_BSTDBG2, 0xeb);
	y700_i2c_write(m, AW8697_REG_BSTDBG3, 0xd4);
	/* Boost voltage: v6p runs vmax 0x18. The old driver sets it
	 * from the HIGH_LEVEL constant (0x18) and writes BSTDBG4 on
	 * playback; without this the chip stays at its power-on 0x11
	 * and the drive is weaker. Same write_bits form as the old
	 * aw8697_haptic_set_bst_vol(). */
	y700_i2c_write_bits(m, AW8697_REG_BSTDBG4,
			    AW8697_BIT_BSTDBG4_BSTVOL_MASK,
			    (Y700_VMAX << 1));
	y700_i2c_write(m, AW8697_REG_TSET, 0x12);
	y700_i2c_write(m, AW8697_REG_R_SPARE, 0x68);

	y700_i2c_write_bits(m, AW8697_REG_ANADBG,
			    AW8697_BIT_ANADBG_IOC_MASK,
			    AW8697_BIT_ANADBG_IOC_4P65A);

	/* peak current 3.5 A */
	y700_motor_set_bst_peak_cur(m, AW8697_BIT_BSTCFG_PEAKCUR_3P5A);

	/* motor protect: no action on detection */
	y700_i2c_write_bits(m, AW8697_REG_DETCTRL,
			    AW8697_BIT_DETCTRL_PROTECT_MASK,
			    AW8697_BIT_DETCTRL_PROTECT_NO_ACTION);

	/* manual boost (auto_boost off, as in v6p) */
	y700_i2c_write_bits(m, AW8697_REG_BST_AUTO,
			    AW8697_BIT_BST_AUTO_BST_AUTOSW_MASK,
			    AW8697_BIT_BST_AUTO_BST_MANUAL_BOOST);

	y700_motor_os_calibration(m);

	/* VBAT hardware compensation */
	y700_i2c_write_bits(m, AW8697_REG_ADCTEST,
			    AW8697_BIT_ADCTEST_VBAT_MODE_MASK,
			    AW8697_BIT_ADCTEST_VBAT_HW_COMP);
	return 0;
}

/* Full per-motor init (probe only): common sequence + f0/index/gain. */
static int y700_motor_init_regs(struct y700_haptic *h, struct y700_motor *m)
{
	u8 reg_val = 0;
	int i;

	y700_motor_init_regs_common(h, m);

	/* f0: preset from DT (1700), working value starts at Y700_F0
	 * (1780) and is replaced by the probe-time BEMF measurement.
	 * NOTE: the old driver set f0=1754 inside f0_calibration(),
	 * which never runs at boot. */
	m->f0 = Y700_F0;
	y700_motor_set_f0_preset(m);

	/* default waveform index 1, as stock */
	y700_motor_set_repeat_wav_seq(m, Y700_INDEX_DEFAULT);

	/* gain / vmax as the chip powers up */
	y700_i2c_read(m, AW8697_REG_DATDBG, &reg_val);
	m->gain = reg_val & 0xFF;
	y700_i2c_read(m, AW8697_REG_BSTDBG4, &reg_val);
	m->vmax = (reg_val >> 1) & 0x1F;

	for (i = 0; i < Y700_SEQUENCER_SIZE; i++) {
		y700_i2c_read(m, AW8697_REG_WAVSEQ1 + i, &reg_val);
		m->seq[i] = reg_val;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* RAM firmware (async load, same file/behaviour as v6p)                 */
/* ------------------------------------------------------------------ */
static void y700_motor_ram_load(struct y700_motor *m, const u8 *data,
				unsigned int len)
{
	unsigned int base_addr;
	unsigned int i;

	y700_i2c_write_bits(m, AW8697_REG_SYSCTRL,
			    AW8697_BIT_SYSCTRL_RAMINIT_MASK,
			    AW8697_BIT_SYSCTRL_RAMINIT_EN);

	base_addr = (unsigned int)((data[2] << 8) | data[3]);

	y700_i2c_write(m, AW8697_REG_BASE_ADDRH, data[2]);
	y700_i2c_write(m, AW8697_REG_BASE_ADDRL, data[3]);
	y700_i2c_write(m, AW8697_REG_FIFO_AEH,
		       (u8)((base_addr >> 1) >> 8));
	y700_i2c_write(m, AW8697_REG_FIFO_AEL,
		       (u8)((base_addr >> 1) & 0x00FF));
	y700_i2c_write(m, AW8697_REG_FIFO_AFH,
		       (u8)((base_addr - (base_addr >> 2)) >> 8));
	y700_i2c_write(m, AW8697_REG_FIFO_AFL,
		       (u8)((base_addr - (base_addr >> 2)) & 0x00FF));

	y700_i2c_write(m, AW8697_REG_RAMADDRH, data[2]);
	y700_i2c_write(m, AW8697_REG_RAMADDRL, data[3]);

	i = 4;	/* data[0..1] checksum, data[2..3] base addr */
	while (i < len) {
		unsigned int chunk = len - i;

		if (chunk > 2048)
			chunk = 2048;
		y700_i2c_writes(m, AW8697_REG_RAMDATA, (u8 *)&data[i], chunk);
		i += chunk;
	}

	y700_i2c_write_bits(m, AW8697_REG_SYSCTRL,
			    AW8697_BIT_SYSCTRL_RAMINIT_MASK,
			    AW8697_BIT_SYSCTRL_RAMINIT_OFF);
}

static void y700_ram_loaded(const struct firmware *fw, void *context)
{
	struct y700_haptic *h = context;
	unsigned short check_sum = 0;
	int i;

	if (!fw) {
		pr_err("%s: failed to read %s\n", __func__, Y700_RAM_FW_NAME);
		return;
	}
	for (i = 2; i < (int)fw->size; i++)
		check_sum += fw->data[i];
	if (check_sum != (unsigned short)((fw->data[0] << 8) | fw->data[1])) {
		pr_err("%s: check sum err 0x%04x\n", __func__, check_sum);
		release_firmware(fw);
		return;
	}

	mutex_lock(&h->lock);
	for (i = 0; i < h->nmotors; i++) {
		y700_motor_ram_load(&h->motor[i], fw->data, fw->size);
		h->motor[i].ram_init = true;
	}
	mutex_unlock(&h->lock);
	release_firmware(fw);
	pr_info("%s: %s loaded to %d motor(s)\n", __func__,
		Y700_RAM_FW_NAME, h->nmotors);
}

static int y700_ram_update(struct y700_haptic *h)
{
	return request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG,
				       Y700_RAM_FW_NAME, h->dev, GFP_KERNEL,
				       h, y700_ram_loaded);
}

/* ------------------------------------------------------------------ */
/* f0 measurement (manual, via sysfs cali -- NOT run at boot)           */
/* ------------------------------------------------------------------ */
static int y700_motor_read_f0(struct y700_motor *m)
{
	u8 reg_val = 0;
	unsigned int f0_reg = 0;
	unsigned long f0_tmp = 0;

	y700_i2c_read(m, AW8697_REG_F_LRA_F0_H, &reg_val);
	f0_reg = (reg_val << 8);
	y700_i2c_read(m, AW8697_REG_F_LRA_F0_L, &reg_val);
	f0_reg |= (reg_val << 0);
	if (f0_reg == 0)
		return -EINVAL;
	f0_tmp = 1000000000UL / (f0_reg * Y700_F0_COEFF);
	return (int)f0_tmp;
}

/*
 * BEMF f0 measurement, same setup as the old get_f0(): CONT mode with
 * F0 detection, LPF 1 kHz, then read the measured F0 registers.
 * Used by the manual sysfs cali node AND by probe (v3), where the
 * measured value becomes the working f0 (v6p's runtime f0 ~1777
 * varies the same way, so this tracks the real motor).
 */
static int y700_motor_measure_f0(struct y700_haptic *h, struct y700_motor *m)
{
	int f0;

	mutex_lock(&h->lock);
	y700_motor_stop(h, m);
	y700_i2c_write(m, AW8697_REG_TRIM_LRA, 0);
	y700_motor_play_mode(m, Y700_MODE_CONT);
	y700_i2c_write_bits(m, AW8697_REG_CONT_CTRL,
			    AW8697_BIT_CONT_CTRL_EN_CLOSE_MASK,
			    AW8697_BIT_CONT_CTRL_OPEN_PLAYBACK);
	y700_i2c_write_bits(m, AW8697_REG_CONT_CTRL,
			    AW8697_BIT_CONT_CTRL_F0_DETECT_MASK,
			    AW8697_BIT_CONT_CTRL_F0_DETECT_ENABLE);
	y700_i2c_write_bits(m, AW8697_REG_DATCTRL,
			    AW8697_BIT_DATCTRL_FC_MASK,
			    AW8697_BIT_DATCTRL_FC_1000HZ);
	y700_i2c_write_bits(m, AW8697_REG_DATCTRL,
			    AW8697_BIT_DATCTRL_LPF_ENABLE_MASK,
			    AW8697_BIT_DATCTRL_LPF_ENABLE);
	y700_motor_set_f0_preset(m);
	y700_i2c_write(m, AW8697_REG_BEMF_VTHH_H, 0x10);
	y700_motor_play_go(m, true);
	msleep(500);
	y700_motor_play_go(m, false);
	f0 = y700_motor_read_f0(m);
	y700_i2c_write_bits(m, AW8697_REG_CONT_CTRL,
			    AW8697_BIT_CONT_CTRL_EN_CLOSE_MASK,
			    AW8697_BIT_CONT_CTRL_CLOSE_PLAYBACK);
	y700_i2c_write_bits(m, AW8697_REG_CONT_CTRL,
			    AW8697_BIT_CONT_CTRL_F0_DETECT_MASK,
			    AW8697_BIT_CONT_CTRL_F0_DETECT_DISABLE);
	y700_motor_stop(h, m);
	mutex_unlock(&h->lock);
	return f0;
}

/*
 * Background f0 re-measurement. v6p's runtime f0 varies (1770-1782)
 * because it is re-measured during operation; a f0 fixed at probe
 * goes stale as the motor warms up and drifts off resonance, and a
 * fixed f0 feels mediocre. This re-measures each motor
 * independently every Y700_F0_REMEASURE_MS, in the background, and
 * only when idle -- never while a vibration is playing (that cycle
 * is skipped and retried next time). A failed or out-of-range
 * measurement keeps the last good value, never a bad one.
 * Each motor buzzes ~500 ms (CONT-mode BEMF detection) per
 * measurement; the 5-minute interval keeps that rare (at most once
 * per 5 min, only when idle). The work runs on system_wq, which is
 * frozen during suspend, so it never fires while suspended.
 * y700_motor_measure_f0 takes h->lock itself, so this function must
 * not hold the lock across the call; the idle check is re-done
 * under the lock before each motor, so a vibration starting
 * between the check and the measurement is caught (that motor is
 * skipped). A vibration starting in the microsecond window between
 * the re-check and measure_f0's lock acquisition is delayed, not
 * lost: measure_f0 holds the lock, so the playback work blocks
 * until the measurement finishes, then plays normally.
 */
static void y700_f0_remeasure_work(struct work_struct *work)
{
	struct y700_haptic *h =
		container_of(work, struct y700_haptic, f0_work.work);
	int i;

	for (i = 0; i < h->nmotors; i++) {
		int mf0;

		/* Only when idle: never interrupt a real vibration. */
		mutex_lock(&h->lock);
		if (h->state != 0 || hrtimer_active(&h->timer)) {
			mutex_unlock(&h->lock);
			break;
		}
		mutex_unlock(&h->lock);

		mf0 = y700_motor_measure_f0(h, &h->motor[i]);
		if (mf0 >= 1500 && mf0 <= 2000) {
			h->motor[i].f0 = mf0;
			pr_info("%s: motor %d re-measured f0 = %d\n",
				__func__, i, mf0);
		} else {
			pr_warn("%s: motor %d f0 re-measure failed (%d), keep %d\n",
				__func__, i, mf0, h->motor[i].f0);
		}
	}

	schedule_delayed_work(&h->f0_work,
			      msecs_to_jiffies(Y700_F0_REMEASURE_MS));
}

/* ------------------------------------------------------------------ */
/* IRQ (one per motor; the old driver only watched the right motor)     */
/* ------------------------------------------------------------------ */
static irqreturn_t y700_irq(int irq, void *data)
{
	struct y700_motor *m = data;
	u8 reg_val = 0;

	/* Reading SYSINT clears the interrupt. */
	y700_i2c_read(m, AW8697_REG_SYSINT, &reg_val);
	if (reg_val & AW8697_BIT_SYSINT_OVI)
		pr_err("%s: motor 0x%02x over-voltage\n", __func__,
		       m->client->addr);
	if (reg_val & AW8697_BIT_SYSINT_UVLI)
		pr_err("%s: motor 0x%02x under-voltage\n", __func__,
		       m->client->addr);
	if (reg_val & AW8697_BIT_SYSINT_OCDI)
		pr_err("%s: motor 0x%02x over-current\n", __func__,
		       m->client->addr);
	if (reg_val & AW8697_BIT_SYSINT_OTI)
		pr_err("%s: motor 0x%02x over-temperature\n", __func__,
		       m->client->addr);
	if (reg_val & AW8697_BIT_SYSINT_DONEI)
		pr_info("%s: motor 0x%02x playback done\n", __func__,
			m->client->addr);
	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------ */
/* LED class devices (the HAL entry points)                             */
/* ------------------------------------------------------------------ */
static enum led_brightness y700_brightness_get(struct led_classdev *cdev)
{
	struct y700_haptic *h = to_y700(cdev);

	return h->amplitude;
}

/*
 * LED brightness path (what the HAL uses for simple on/off):
 * stop, then if level > 0 play the current waveform in RAM mode.
 * Same register sequence as v6p.
 */
static void y700_brightness_set(struct led_classdev *cdev,
				enum led_brightness level)
{
	struct y700_haptic *h = to_y700(cdev);

	h->amplitude = level;
	mutex_lock(&h->lock);
	y700_stop(h);
	if (h->amplitude > 0) {
		y700_ram_vbat_comp(h, false);
		y700_play_wav_seq(h);
	}
	mutex_unlock(&h->lock);
}

static enum led_brightness y700_brightness_get_r(struct led_classdev *cdev)
{
	struct y700_haptic *h = to_y700_r(cdev);

	return y700_brightness_get(&h->cdev);
}

static void y700_brightness_set_r(struct led_classdev *cdev,
				  enum led_brightness level)
{
	struct y700_haptic *h = to_y700_r(cdev);

	y700_brightness_set(&h->cdev, level);
}

/* ------------------------------------------------------------------ */
/* Activate path (sysfs activate + duration; what the HAL uses for      */
/* timed effects): program SINE_CYCLE=9 + infinite loop, then the      */
/* work routine (re)starts playback in RAM_LOOP and arms a timer.      */
/* ------------------------------------------------------------------ */
static enum hrtimer_restart y700_timer_func(struct hrtimer *timer)
{
	struct y700_haptic *h =
		container_of(timer, struct y700_haptic, timer);

	h->state = 0;
	queue_work(system_highpri_wq, &h->work);
	return HRTIMER_NORESTART;
}

static void y700_work_routine(struct work_struct *work)
{
	struct y700_haptic *h =
		container_of(work, struct y700_haptic, work);

	mutex_lock(&h->lock);
	y700_stop(h);
	mutex_unlock(&h->lock);
	if (h->state) {
		mutex_lock(&h->lock);
		y700_ram_vbat_comp(h, true);
		y700_play_repeat_seq(h);
		mutex_unlock(&h->lock);
		hrtimer_start(&h->timer,
			      ktime_set(h->duration / 1000,
					(h->duration % 1000) * 1000000),
			      HRTIMER_MODE_REL);
	}
}

/* ------------------------------------------------------------------ */
/* sysfs (attached to both LED devices)                                 */
/* ------------------------------------------------------------------ */
static struct y700_haptic *y700_from_led_dev(struct device *dev)
{
	/* The LED core sets the device's driver data to the led_classdev
	 * (led_classdev.dev is a *pointer*, so container_of() on it is
	 * wrong -- this is exactly how the old driver did it). */
	struct led_classdev *led = dev_get_drvdata(dev);

	if (strcmp(dev_name(dev), "vibrator_r") == 0)
		return container_of(led, struct y700_haptic, cdev_r);
	return container_of(led, struct y700_haptic, cdev);
}

static ssize_t y700_state_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct y700_haptic *h = y700_from_led_dev(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", h->state);
}

static ssize_t y700_state_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	return count;
}

static ssize_t y700_duration_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct y700_haptic *h = y700_from_led_dev(dev);
	s64 time_ms = 0;

	if (hrtimer_active(&h->timer))
		time_ms = ktime_to_ms(hrtimer_get_remaining(&h->timer));
	return snprintf(buf, PAGE_SIZE, "%lld\n", time_ms);
}

static ssize_t y700_duration_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct y700_haptic *h = y700_from_led_dev(dev);
	unsigned int val = 0;
	int rc;

	rc = kstrtouint(buf, 0, &val);
	if (rc < 0)
		return rc;
	if (val <= 0)
		return count;
	h->duration = val;
	return count;
}

static ssize_t y700_activate_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct y700_haptic *h = y700_from_led_dev(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", h->state);
}

static ssize_t y700_activate_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct y700_haptic *h = y700_from_led_dev(dev);
	unsigned int val = 0;
	int rc;
	int i;

	rc = kstrtouint(buf, 0, &val);
	if (rc < 0)
		return rc;
	if (val != 0 && val != 1)
		return count;

	hrtimer_cancel(&h->timer);
	h->state = val;

	if (val) {
		mutex_lock(&h->lock);
		for (i = 0; i < h->nmotors; i++) {
			if (h->motor[i].gain >= Y700_GAIN_CLAMP)
				h->motor[i].gain = Y700_GAIN_CLAMP;
			y700_motor_set_gain(&h->motor[i],
					    h->motor[i].gain);
		}
		/* SINE_CYCLE waveform 9, infinite loop (v6p behaviour). */
		y700_set_repeat_wav_seq(h, Y700_WAVEFORM_SINE_CYCLE);
		mutex_unlock(&h->lock);
		cancel_work_sync(&h->work);
		queue_work(system_highpri_wq, &h->work);
	} else {
		mutex_lock(&h->lock);
		y700_stop(h);
		mutex_unlock(&h->lock);
	}
	return count;
}

static ssize_t y700_activate_mode_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	struct y700_haptic *h = y700_from_led_dev(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", h->activate_mode);
}

static ssize_t y700_activate_mode_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct y700_haptic *h = y700_from_led_dev(dev);
	unsigned int val = 0;
	int rc;

	rc = kstrtouint(buf, 0, &val);
	if (rc < 0)
		return rc;
	/* Stored for compatibility; the activate path always plays
	 * RAM_LOOP (the old driver forced RAM_MODE in the work routine). */
	h->activate_mode = val;
	return count;
}

static ssize_t y700_index_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct y700_haptic *h = y700_from_led_dev(dev);
	u8 reg_val = 0;

	y700_i2c_read(&h->motor[0], AW8697_REG_WAVSEQ1, &reg_val);
	h->index = reg_val & 0x7F;
	return snprintf(buf, PAGE_SIZE, "%d\n", h->index);
}

static ssize_t y700_index_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct y700_haptic *h = y700_from_led_dev(dev);
	unsigned int val = 0;
	int rc;

	rc = kstrtouint(buf, 0, &val);
	if (rc < 0)
		return rc;
	mutex_lock(&h->lock);
	h->index = val;
	y700_set_repeat_wav_seq(h, h->index);
	mutex_unlock(&h->lock);
	return count;
}

static ssize_t y700_gain_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct y700_haptic *h = y700_from_led_dev(dev);

	return snprintf(buf, PAGE_SIZE, "0x%02x\n", h->motor[0].gain);
}

static ssize_t y700_gain_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct y700_haptic *h = y700_from_led_dev(dev);
	unsigned int val = 0;
	int rc;
	int i;

	rc = kstrtouint(buf, 0, &val);
	if (rc < 0)
		return rc;
	mutex_lock(&h->lock);
	for (i = 0; i < h->nmotors; i++) {
		h->motor[i].gain = val & 0xFF;
		y700_motor_set_gain(&h->motor[i], h->motor[i].gain);
	}
	mutex_unlock(&h->lock);
	return count;
}

static ssize_t y700_vmax_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct y700_haptic *h = y700_from_led_dev(dev);

	return snprintf(buf, PAGE_SIZE, "0x%02x\n", h->motor[0].vmax);
}

static ssize_t y700_seq_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct y700_haptic *h = y700_from_led_dev(dev);
	u8 reg_val = 0;
	int i;
	ssize_t len = 0;

	for (i = 0; i < Y700_SEQUENCER_SIZE; i++) {
		y700_i2c_read(&h->motor[0], AW8697_REG_WAVSEQ1 + i, &reg_val);
		h->motor[0].seq[i] = reg_val;
		len += snprintf(buf + len, PAGE_SIZE - len, "%d ", reg_val);
	}
	len += snprintf(buf + len, PAGE_SIZE - len, "\n");
	return len;
}

static ssize_t y700_seq_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct y700_haptic *h = y700_from_led_dev(dev);
	unsigned int val = 0;
	int rc;

	rc = kstrtouint(buf, 0, &val);
	if (rc < 0)
		return rc;
	/* Debug: put one waveform index in sequencer slot 0 (both motors). */
	mutex_lock(&h->lock);
	h->index = val & 0x7F;
	y700_set_repeat_wav_seq(h, h->index);
	mutex_unlock(&h->lock);
	return count;
}

static ssize_t y700_f0_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct y700_haptic *h = y700_from_led_dev(dev);
	/* Per-motor working f0 (re-measured in the background). The
	 * vibrator node shows motor 0 (right), vibrator_r shows
	 * motor 1 (left); the two motors differ by ~1 Hz, like v6p.
	 * (v3 always showed motor 0 on both nodes, which is why both
	 * read an identical 1774.) */
	int idx = (strcmp(dev_name(dev), "vibrator_r") == 0) ? 1 : 0;

	if (idx >= h->nmotors)
		idx = 0;
	return snprintf(buf, PAGE_SIZE, "%d\n", h->motor[idx].f0);
}

static ssize_t y700_cali_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct y700_haptic *h = y700_from_led_dev(dev);
	int f0;

	f0 = y700_motor_measure_f0(h, &h->motor[0]);
	return snprintf(buf, PAGE_SIZE, "measured f0 = %d\n", f0);
}

static ssize_t y700_cali_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct y700_haptic *h = y700_from_led_dev(dev);
	unsigned int val = 0;
	int rc;
	int i;
	int f0;

	rc = kstrtouint(buf, 0, &val);
	if (rc < 0)
		return rc;
	if (!val)
		return count;
	for (i = 0; i < h->nmotors; i++) {
		f0 = y700_motor_measure_f0(h, &h->motor[i]);
		pr_info("%s: motor 0x%02x measured f0 = %d (working f0 stays %d)\n",
			__func__, h->motor[i].client->addr, f0,
			h->motor[i].f0);
	}
	return count;
}

static ssize_t y700_ram_update_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct y700_haptic *h = y700_from_led_dev(dev);

	y700_ram_update(h);
	return count;
}

static ssize_t y700_vbat_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct y700_haptic *h = y700_from_led_dev(dev);
	int vbat;

	/* get_vbat is a multi-step sequence; hold the lock so it cannot
	 * interleave with a concurrent play/stop. */
	mutex_lock(&h->lock);
	vbat = y700_motor_get_vbat(&h->motor[0]);
	h->vbat = vbat;
	mutex_unlock(&h->lock);
	return snprintf(buf, PAGE_SIZE, "%d\n", vbat);
}

static ssize_t y700_ram_vbat_comp_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	struct y700_haptic *h = y700_from_led_dev(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", h->ram_vbat_comp);
}

static ssize_t y700_ram_vbat_comp_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct y700_haptic *h = y700_from_led_dev(dev);
	unsigned int val = 0;
	int rc;

	rc = kstrtouint(buf, 0, &val);
	if (rc < 0)
		return rc;
	h->ram_vbat_comp = !!val;
	return count;
}

static ssize_t y700_osc_cali_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct y700_haptic *h = y700_from_led_dev(dev);
	unsigned int val = 0;
	int rc;
	int i;

	rc = kstrtouint(buf, 0, &val);
	if (rc < 0)
		return rc;
	if (!val)
		return count;
	mutex_lock(&h->lock);
	for (i = 0; i < h->nmotors; i++)
		y700_motor_os_calibration(&h->motor[i]);
	mutex_unlock(&h->lock);
	return count;
}

static DEVICE_ATTR(state, S_IRUGO | S_IWUSR | S_IWGRP,
		   y700_state_show, y700_state_store);
static DEVICE_ATTR(duration, S_IRUGO | S_IWUSR | S_IWGRP,
		   y700_duration_show, y700_duration_store);
static DEVICE_ATTR(activate, S_IRUGO | S_IWUSR | S_IWGRP,
		   y700_activate_show, y700_activate_store);
static DEVICE_ATTR(activate_mode, S_IRUGO | S_IWUSR | S_IWGRP,
		   y700_activate_mode_show, y700_activate_mode_store);
static DEVICE_ATTR(index, S_IRUGO | S_IWUSR | S_IWGRP,
		   y700_index_show, y700_index_store);
static DEVICE_ATTR(gain, S_IRUGO | S_IWUSR | S_IWGRP,
		   y700_gain_show, y700_gain_store);
static DEVICE_ATTR(vmax, S_IRUGO, y700_vmax_show, NULL);
static DEVICE_ATTR(seq, S_IRUGO | S_IWUSR | S_IWGRP,
		   y700_seq_show, y700_seq_store);
static DEVICE_ATTR(f0, S_IRUGO, y700_f0_show, NULL);
static DEVICE_ATTR(cali, S_IRUGO | S_IWUSR | S_IWGRP,
		   y700_cali_show, y700_cali_store);
static DEVICE_ATTR(ram_update, S_IWUSR | S_IWGRP, NULL,
		   y700_ram_update_store);
static DEVICE_ATTR(vbat_monitor, S_IRUGO, y700_vbat_show, NULL);
static DEVICE_ATTR(ram_vbat_comp, S_IRUGO | S_IWUSR | S_IWGRP,
		   y700_ram_vbat_comp_show, y700_ram_vbat_comp_store);
static DEVICE_ATTR(osc_cali, S_IWUSR | S_IWGRP, NULL,
		   y700_osc_cali_store);

static struct attribute *y700_vibrator_attributes[] = {
	&dev_attr_state.attr,
	&dev_attr_duration.attr,
	&dev_attr_activate.attr,
	&dev_attr_activate_mode.attr,
	&dev_attr_index.attr,
	&dev_attr_gain.attr,
	&dev_attr_vmax.attr,
	&dev_attr_seq.attr,
	&dev_attr_f0.attr,
	&dev_attr_cali.attr,
	&dev_attr_ram_update.attr,
	&dev_attr_vbat_monitor.attr,
	&dev_attr_ram_vbat_comp.attr,
	&dev_attr_osc_cali.attr,
	NULL
};

static struct attribute_group y700_vibrator_attribute_group = {
	.attrs = y700_vibrator_attributes,
};

/* Direct register access on the primary I2C device (debug). */
static ssize_t y700_reg_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct y700_haptic *h = dev_get_drvdata(dev);
	ssize_t len = 0;
	u8 reg_val = 0;
	int i;

	for (i = 0; i < 0x80; i++) {
		if (!(aw8697_reg_access[i] & REG_RD_ACCESS))
			continue;
		y700_i2c_read(&h->motor[0], i, &reg_val);
		len += snprintf(buf + len, PAGE_SIZE - len,
				"reg 0x%02x = 0x%02x\n", i, reg_val);
	}
	return len;
}

static ssize_t y700_reg_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct y700_haptic *h = dev_get_drvdata(dev);
	unsigned int databuf[2] = {0, 0};

	if (2 == sscanf(buf, "%x %x", &databuf[0], &databuf[1]))
		y700_both_write(h, (u8)databuf[0], (u8)databuf[1]);
	return count;
}

static DEVICE_ATTR(reg, S_IRUGO | S_IWUSR | S_IWGRP,
		   y700_reg_show, y700_reg_store);

static struct attribute *y700_attributes[] = {
	&dev_attr_reg.attr,
	NULL
};

static struct attribute_group y700_attribute_group = {
	.attrs = y700_attributes,
};

/* ------------------------------------------------------------------ */
/* Device tree                                                          */
/* ------------------------------------------------------------------ */
static int y700_parse_dt(struct device *dev, struct y700_haptic *h,
			 struct device_node *np)
{
	u32 val;
	u8 val8;

	h->reset_gpio = of_get_named_gpio(np, "reset-gpio", 0);
	if (h->reset_gpio < 0) {
		dev_err(dev, "%s: no reset gpio\n", __func__);
		return -EINVAL;
	}
	h->motor[0].irq_gpio = of_get_named_gpio(np, "irq-gpio", 0);
	if (h->motor[0].irq_gpio < 0)
		dev_warn(dev, "%s: no irq gpio\n", __func__);

	/* f0_pre and CONT parameters come from the Y700 DT (the old
	 * driver ignored all of them and used OPPO machine constants). */
	if (of_property_read_u32(np, "f0_pre", &val))
		val = Y700_F0_PRE;
	h->motor[0].f0_pre = val;
	if (of_property_read_u32(np, "aw869x_cont_td", &val))
		val = Y700_CONT_TD;
	h->cont_td = val;
	if (of_property_read_u32(np, "aw869x_cont_zc_thr", &val))
		val = Y700_CONT_ZC_THR;
	h->cont_zc_thr = val;
	if (of_property_read_u8(np, "aw869x_cont_num_brk", &val8))
		val8 = Y700_CONT_NUM_BRK;
	h->cont_num_brk = val8;
	return 0;
}

static void y700_hw_reset(struct y700_haptic *h)
{
	if (!gpio_is_valid(h->reset_gpio))
		return;
	/* One pulse on the shared line resets both chips. */
	gpio_set_value_cansleep(h->reset_gpio, 0);
	usleep_range(1000, 2000);
	gpio_set_value_cansleep(h->reset_gpio, 1);
	usleep_range(3500, 4000);
}

static int y700_read_chipid(struct y700_motor *m)
{
	u8 reg = 0;
	int ret;
	unsigned char cnt = 0;

	while (cnt < Y700_CHIPID_RETRIES) {
		ret = y700_i2c_read(m, AW8697_REG_ID, &reg);
		if (ret >= 0 && reg == Y700_AW8697_CHIPID) {
			m->chipid = Y700_AW8697_CHIPID;
			y700_motor_softreset(m);
			return 0;
		}
		pr_info("%s: motor 0x%02x id 0x%02x (try %d)\n",
			__func__, m->client->addr, reg, cnt);
		cnt++;
		msleep(Y700_CHIPID_RETRY_DELAY_MS);
	}
	return -EINVAL;
}

/* ------------------------------------------------------------------ */
/* Probe / remove                                                       */
/* ------------------------------------------------------------------ */
static int y700_probe(struct i2c_client *i2c,
		      const struct i2c_device_id *id)
{
	struct y700_haptic *h;
	struct device_node *np = i2c->dev.of_node;
	struct device_node *sn;
	struct i2c_client *sl;
	int ret;
	int i;

	/*
	 * The left motor (0x5B) never gets its own probe: the controller
	 * created by the right motor (0x5A) drives both motors
	 * explicitly. This matches v6p (which also returned -ENODEV
	 * for 0x5B) and avoids a second instance overwriting the first.
	 */
	if (i2c->addr == 0x5B)
		return -ENODEV;

	if (!i2c_check_functionality(i2c->adapter, I2C_FUNC_I2C))
		return -EIO;

	h = devm_kzalloc(&i2c->dev, sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;

	h->dev = &i2c->dev;
	h->motor[0].client = i2c;
	h->motor[0].f0_pre = Y700_F0_PRE;
	h->motor[0].f0 = Y700_F0;
	h->nmotors = 1;
	h->index = Y700_INDEX_DEFAULT;
	h->ram_vbat_comp = false;
	h->cont_td = Y700_CONT_TD;
	h->cont_zc_thr = Y700_CONT_ZC_THR;
	h->cont_num_brk = Y700_CONT_NUM_BRK;
	mutex_init(&h->lock);
	i2c_set_clientdata(i2c, h);
	dev_set_drvdata(&i2c->dev, h);

	if (np) {
		ret = y700_parse_dt(&i2c->dev, h, np);
		if (ret)
			return ret;
	} else {
		h->reset_gpio = -1;
		h->motor[0].irq_gpio = -1;
	}

	/* Shared reset: requested once, by this controller only. */
	if (gpio_is_valid(h->reset_gpio)) {
		ret = devm_gpio_request_one(&i2c->dev, h->reset_gpio,
					    GPIOF_OUT_INIT_LOW,
					    "y700_haptic_rst");
		if (ret) {
			dev_err(&i2c->dev, "%s: rst request failed %d\n",
				__func__, ret);
			return ret;
		}
	}

	/* Find the left motor's client (bus 7, addr 0x5B). */
	sn = of_find_compatible_node(NULL, NULL, "awinic,haptic_hv_l");
	if (sn) {
		u32 val;

		/* Read the left node's properties BEFORE releasing it. */
		h->motor[1].irq_gpio =
			of_get_named_gpio(sn, "irq-gpio", 0);
		if (of_property_read_u32(sn, "f0_pre", &val))
			val = Y700_F0_PRE;
		h->motor[1].f0_pre = val;
		h->motor[1].f0 = Y700_F0;

		sl = of_find_i2c_device_by_node(sn);
		of_node_put(sn);
		if (!sl) {
			dev_warn(&i2c->dev,
				 "%s: left motor client not ready, single-motor mode\n",
				 __func__);
		} else {
			h->motor[1].client = sl;
			/* The chip ID is verified after the shared reset
			 * below (y700_read_chipid); if it fails we drop
			 * to single-motor mode there. */
			h->nmotors = 2;
			dev_info(&i2c->dev,
				 "%s: dual motor, left client 7-005b found\n",
				 __func__);
		}
	} else {
		dev_info(&i2c->dev, "%s: no left motor node, single-motor mode\n",
			 __func__);
	}

	/* One reset pulse for both chips (shared line). */
	y700_hw_reset(h);

	/* Chip ID + soft reset, per motor. */
	for (i = 0; i < h->nmotors; i++) {
		ret = y700_read_chipid(&h->motor[i]);
		if (ret < 0) {
			dev_err(&i2c->dev,
				"%s: motor %d chipid failed\n", __func__, i);
			if (i == 0) {
				if (h->nmotors > 1 && h->motor[1].client)
					put_device(&h->motor[1].client->dev);
				return ret;
			}
			/* Left motor failed: drop to single-motor. */
			if (h->motor[1].client)
				put_device(&h->motor[1].client->dev);
			h->motor[1].client = NULL;
			h->nmotors = 1;
		}
	}

	/* IRQs, one per motor (the old driver only watched the right). */
	for (i = 0; i < h->nmotors; i++) {
		struct y700_motor *m = &h->motor[i];

		if (!gpio_is_valid(m->irq_gpio))
			continue;
		ret = devm_gpio_request_one(&i2c->dev, m->irq_gpio,
					    GPIOF_DIR_IN, "y700_haptic_int");
		if (ret) {
			dev_warn(&i2c->dev, "%s: irq gpio %d request failed\n",
				 __func__, m->irq_gpio);
			m->irq_gpio = -1;
			continue;
		}
		m->irq = gpio_to_irq(m->irq_gpio);
		ret = devm_request_threaded_irq(&i2c->dev, m->irq, NULL,
						y700_irq,
						IRQF_TRIGGER_FALLING |
						IRQF_ONESHOT,
						i ? "y700_haptic_l" :
						    "y700_haptic_r", m);
		if (ret) {
			dev_warn(&i2c->dev, "%s: irq %d request failed\n",
				 __func__, m->irq);
			m->irq = 0;
		}
	}

	ret = sysfs_create_group(&i2c->dev.kobj, &y700_attribute_group);
	if (ret < 0)
		dev_warn(&i2c->dev, "%s: sysfs group failed\n", __func__);

	/* LED nodes: the HAL requires both to exist. */
	h->cdev.name = "vibrator";
	h->cdev.brightness_get = y700_brightness_get;
	h->cdev.brightness_set = y700_brightness_set;
	ret = devm_led_classdev_register(&i2c->dev, &h->cdev);
	if (ret < 0) {
		dev_err(&i2c->dev, "%s: vibrator led failed\n", __func__);
		return ret;
	}
	ret = sysfs_create_group(&h->cdev.dev->kobj,
				 &y700_vibrator_attribute_group);
	if (ret < 0)
		dev_warn(&i2c->dev, "%s: vibrator sysfs failed\n", __func__);

	h->cdev_r.name = "vibrator_r";
	h->cdev_r.brightness_get = y700_brightness_get_r;
	h->cdev_r.brightness_set = y700_brightness_set_r;
	ret = devm_led_classdev_register(&i2c->dev, &h->cdev_r);
	if (ret < 0) {
		dev_warn(&i2c->dev, "%s: vibrator_r led failed\n", __func__);
	} else {
		ret = sysfs_create_group(&h->cdev_r.dev->kobj,
					 &y700_vibrator_attribute_group);
		if (ret < 0)
			dev_warn(&i2c->dev, "%s: vibrator_r sysfs failed\n",
				 __func__);
	}

	hrtimer_init(&h->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	h->timer.function = y700_timer_func;
	INIT_WORK(&h->work, y700_work_routine);
	INIT_DELAYED_WORK(&h->f0_work, y700_f0_remeasure_work);

	/* Per-motor init, under the lock (same register sequence as v6p). */
	mutex_lock(&h->lock);
	for (i = 0; i < h->nmotors; i++)
		y700_motor_init_regs(h, &h->motor[i]);
	h->ram_vbat_comp = true;
	mutex_unlock(&h->lock);

	/* Measure the real resonance (BEMF) per motor and use it as the
	 * working f0, like v6p's runtime f0 (~1777, varies per read).
	 * y700_motor_measure_f0 takes the lock itself, so it must run
	 * after the unlock above. A 500 ms CONT-mode buzz per motor at
	 * boot is the price of the measurement. Falls back to Y700_F0
	 * (1780) if the measurement fails or is out of range. */
	for (i = 0; i < h->nmotors; i++) {
		int mf0 = y700_motor_measure_f0(h, &h->motor[i]);

		if (mf0 >= 1500 && mf0 <= 2000) {
			h->motor[i].f0 = mf0;
			pr_info("%s: motor %d measured f0 = %d\n",
				__func__, i, mf0);
		} else {
			pr_warn("%s: motor %d f0 measure failed (%d), keep %d\n",
				__func__, i, mf0, h->motor[i].f0);
		}
	}

	/* Async RAM waveform load (same file as v6p). */
	y700_ram_update(h);

	/* Start the background f0 re-measurement; the first cycle
	 * runs Y700_F0_REMEASURE_MS after boot. */
	schedule_delayed_work(&h->f0_work,
			      msecs_to_jiffies(Y700_F0_REMEASURE_MS));

	pr_info("%s: probe ok, %d motor(s), f0=%d/%d, index=%d\n", __func__,
		h->nmotors, h->motor[0].f0,
		h->nmotors > 1 ? h->motor[1].f0 : 0, h->index);
	return 0;
}

static int y700_remove(struct i2c_client *i2c)
{
	struct y700_haptic *h = i2c_get_clientdata(i2c);

	hrtimer_cancel(&h->timer);
	cancel_work_sync(&h->work);
	cancel_delayed_work_sync(&h->f0_work);
	sysfs_remove_group(&i2c->dev.kobj, &y700_attribute_group);
	if (h->nmotors > 1 && h->motor[1].client)
		put_device(&h->motor[1].client->dev);
	return 0;
}

static int __maybe_unused y700_suspend(struct device *dev)
{
	struct y700_haptic *h = dev_get_drvdata(dev);

	mutex_lock(&h->lock);
	y700_stop(h);
	mutex_unlock(&h->lock);
	return 0;
}

static int __maybe_unused y700_resume(struct device *dev)
{
	return 0;
}

static SIMPLE_DEV_PM_OPS(y700_pm_ops, y700_suspend, y700_resume);

static const struct i2c_device_id y700_haptic_id[] = {
	{ "aw8697_y700", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, y700_haptic_id);

static struct of_device_id y700_haptic_dt_match[] = {
	{ .compatible = "awinic,haptic_hv_r" },
	{ .compatible = "awinic,haptic_hv_l" },
	{ .compatible = "awinic,aw8697_haptic" },
	{ },
};

static struct i2c_driver y700_haptic_driver = {
	.driver = {
		.name = "aw8697_y700",
		.of_match_table = of_match_ptr(y700_haptic_dt_match),
		.pm = &y700_pm_ops,
	},
	.probe = y700_probe,
	.remove = y700_remove,
	.id_table = y700_haptic_id,
};

module_i2c_driver(y700_haptic_driver);

MODULE_DESCRIPTION("Y700 AW8697 dual haptic driver (clean rewrite)");
MODULE_LICENSE("GPL v2");
