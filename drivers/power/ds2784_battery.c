/* drivers/power/ds2784_battery.c
 *
 * Copyright (C) 2009 HTC Corporation
 * Copyright (C) 2009 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/module.h>
#include <linux/param.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/pm.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/spinlock.h>

#include <linux/android_alarm.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/wakelock.h>
#include <asm/gpio.h>

#include "../w1/w1.h"
#include "../w1/slaves/w1_ds2784.h"

struct ds2784_device_info {
	struct device *dev;

	/* DS2784 data, valid after calling ds2784_battery_read_status() */
	unsigned long update_time;	/* jiffies when data read */
	char raw[DS2784_DATA_SIZE];	/* raw DS2784 data */
	int voltage_uV;			/* units of uV */
	int current_uA;			/* units of uA */
	int current_avg_uA;
	int temp_raw;			/* units of 0.125 C */
	int temp_C;			/* units of 0.1 C */
	int charge_status;		/* POWER_SUPPLY_STATUS_* */
	int percentage;			/* battery percentage */
	int charge_uAh;
	int guage_status_reg;		/* battery status register offset=01h*/

	int charging_source;		/* 0: no cable, 1:usb, 2:AC */

	struct power_supply bat;
	struct device *w1_dev;
	struct workqueue_struct *monitor_wqueue;
	struct work_struct monitor_work;
	struct alarm alarm;
	struct wake_lock work_wake_lock;
};

#define psy_to_dev_info(x) container_of((x), struct ds2784_device_info, bat)

static struct wake_lock vbus_wake_lock;
static unsigned int cache_time = 1000;
module_param(cache_time, uint, 0644);
MODULE_PARM_DESC(cache_time, "cache time in milliseconds");

#define BATT_NO_SOURCE        (0)  /* 0: No source battery */
#define BATT_FIRST_SOURCE     (1)  /* 1: Main source battery */
#define BATT_SECOND_SOURCE    (2)  /* 2: Second source battery */
#define BATT_THIRD_SOURCE     (3)  /* 3: Third source battery */
#define BATT_FOURTH_SOURCE    (4)  /* 4: Fourth source battery */
#define BATT_FIFTH_SOURCE     (5)  /* 5: Fifth source battery */
#define BATT_UNKNOWN        (255)  /* Other: Unknown battery */

#define BATT_RSNSP			(67)	/*Passion battery source 1*/

#define GPIO_BATTERY_DETECTION		39
#define GPIO_BATTERY_CHARGER_EN		22
#define GPIO_BATTERY_CHARGER_CURRENT	16

static enum power_supply_property battery_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
};

static int battery_initial;

typedef enum {
	DISABLE = 0,
	ENABLE_SLOW_CHG,
	ENABLE_FAST_CHG
} batt_ctl_t;

typedef enum {
	CHARGER_BATTERY = 0,
	CHARGER_USB,
	CHARGER_AC
} charger_type_t;

static int battery_get_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val);

static void battery_ext_power_changed(struct power_supply *psy);

#define to_ds2784_device_info(x) container_of((x), struct ds2784_device_info, \
					      bat);

static void ds2784_parse_data(struct ds2784_device_info *di)
{
	short n;

	/* Get status reg */
	di->guage_status_reg = di->raw[DS2784_REG_STS];

	/* Get Level */
	di->percentage = di->raw[DS2784_REG_RARC];

	/* Get Voltage: Unit=4.886mV, range is 0V to 4.99V */
	n = (((di->raw[DS2784_REG_VOLT_MSB] << 8) |
	      (di->raw[DS2784_REG_VOLT_LSB])) >> 5);

	di->voltage_uV = n * 4886;

	/* Get Current: Unit= 1.5625uV x Rsnsp(67)=104.68 */
	n = ((di->raw[DS2784_REG_CURR_MSB]) << 8) |
		di->raw[DS2784_REG_CURR_LSB];
	di->current_uA = ((n * 15625) / 10000) * 67;

	n = ((di->raw[DS2784_REG_AVG_CURR_MSB]) << 8) |
		di->raw[DS2784_REG_AVG_CURR_LSB];
	di->current_avg_uA = ((n * 15625) / 10000) * 67;

	/* Get Temperature:
	 * Unit=0.125 degree C,therefore, give up LSB ,
	 * just caculate MSB for temperature only.
	 */
	di->temp_raw = (((signed char)di->raw[DS2784_REG_TEMP_MSB]) << 3) |
				     (di->raw[DS2784_REG_TEMP_LSB] >> 5);
	di->temp_C = di->temp_raw + (di->temp_raw / 4);

	/* RAAC is in units of 1.6mAh */
	di->charge_uAh = ((di->raw[DS2784_REG_RAAC_MSB] << 8) |
			  di->raw[DS2784_REG_RAAC_LSB]) * 1600;
}

static int ds2784_battery_read_status(struct ds2784_device_info *di)
{
	int ret, start, count;

	/* The first time we read the entire contents of SRAM/EEPROM,
	 * but after that we just read the interesting bits that change. */
	if (di->raw[DS2784_REG_RSNSP] == 0x00) {
		start = 0;
		count = DS2784_DATA_SIZE;
	} else {
		start = DS2784_REG_PORT;
		count = DS2784_REG_CURR_LSB - start + 1;
	}

	ret = w1_ds2784_read(di->w1_dev, di->raw + start, start, count);
	if (ret != count) {
		dev_warn(di->dev, "call to w1_ds2784_read failed (0x%p)\n",
			 di->w1_dev);
		return 1;
	}
	di->update_time = jiffies;

	/*
	 * Check if dummy battery in.
	 * Workaround for dummy battery
	 * Write ACR MSB to 0x05, ensure there must be 500mAH .
	 * ONLY check when battery driver init.
	 */
	if (battery_initial == 0) {
		if (di->raw[DS2784_REG_USER_EEPROM_20] == 0x01) {
			unsigned char acr[2];
			acr[0] = 0x05;
			acr[1] = 0x06;
			w1_ds2784_write(di->w1_dev, acr,DS2784_REG_ACCUMULATE_CURR_MSB, 2);
		}
		dev_warn(di->dev, "battery dummy battery = %d\n", di->raw[DS2784_REG_USER_EEPROM_20]);
		battery_initial = 1;
	}

	pr_info("batt: %02x %02x %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x\n",
		di->raw[0x00], di->raw[0x01], di->raw[0x02], di->raw[0x03], 
		di->raw[0x04], di->raw[0x05], di->raw[0x06], di->raw[0x07], 
		di->raw[0x08], di->raw[0x09], di->raw[0x0a], di->raw[0x0b], 
		di->raw[0x0c], di->raw[0x0d], di->raw[0x0e], di->raw[0x0f]
		);

	ds2784_parse_data(di);

	pr_info("batt: %3d%%, %d mV, %d mA (%d avg), %d C, %d mAh\n",
		di->raw[DS2784_REG_RARC],
		di->voltage_uV / 1000, di->current_uA / 1000,
		di->current_avg_uA / 1000,
		di->temp_C, di->charge_uAh / 1000);
	
	return 0;
}

static int battery_get_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val)
{
	struct ds2784_device_info *di = psy_to_dev_info(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		switch (di->charging_source) {
		case CHARGER_BATTERY:
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
			break;
		case CHARGER_USB:
		case CHARGER_AC:
			if (di->percentage == 100) 
				val->intval = POWER_SUPPLY_STATUS_FULL;
			else
				val->intval = POWER_SUPPLY_STATUS_CHARGING;
			break;
		default:
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
			break;
		}
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = POWER_SUPPLY_HEALTH_GOOD;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		/* XXX todo */
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = di->percentage;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = di->voltage_uV;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = di->temp_C;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = di->current_uA;
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		val->intval = di->current_avg_uA;
		break;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		val->intval = di->charge_uAh;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void ds2784_battery_update_status(struct ds2784_device_info *di)
{
	u8 last_level;
	last_level = di->percentage;

	ds2784_battery_read_status(di);

#if 0
	if (htc_batt_info.rep.batt_id == BATT_UNKNOWN) {
		htc_batt_info.rep.level = 0;
		printk("Not support battery %d, power down system\n",htc_batt_info.rep.batt_id);
		power_supply_changed(&di->bat);
	}
#endif

	if (last_level != di->percentage)
		power_supply_changed(&di->bat);
}

static unsigned last_source = 0xffffffff;
static unsigned last_status = 0xffffffff;
static spinlock_t charge_state_lock;

static void battery_adjust_charge_state(struct ds2784_device_info *di)
{
	unsigned source;
	unsigned status;
	unsigned long flags;

	spin_lock_irqsave(&charge_state_lock, flags);

	source = di->charging_source;
	status = di->raw[0x01];

	if ((source == last_source) && (status == last_status))
		goto done;

	last_source = source;
	last_status = status;

	if (status & 0x80)
		/* if charge termination flag is set, ignore actual source */
		source = CHARGER_BATTERY;
	else
		source = di->charging_source;

	switch (source) {
	case CHARGER_BATTERY:
		/* CHARGER_EN is active low.  Set to 1 to disable. */
		gpio_direction_output(GPIO_BATTERY_CHARGER_EN, 1);
		pr_info("batt: charging OFF%s\n",
			(status & 0x80) ? " [CHGTF]" : "");
		break;
	case CHARGER_USB:
		/* slow charge mode */
		gpio_direction_output(GPIO_BATTERY_CHARGER_CURRENT, 0);
		gpio_direction_output(GPIO_BATTERY_CHARGER_EN, 0);
		pr_info("batt: charging SLOW\n");
		break;
	case CHARGER_AC:
		/* fast charge mode */
		gpio_direction_output(GPIO_BATTERY_CHARGER_CURRENT, 1);
		gpio_direction_output(GPIO_BATTERY_CHARGER_EN, 0);
		pr_info("batt: charging FAST\n");
		break;
	}

done:
	spin_unlock_irqrestore(&charge_state_lock, flags);
}

static void ds2784_battery_work(struct work_struct *work)
{
	struct ds2784_device_info *di =
		container_of(work, struct ds2784_device_info, monitor_work);
	const ktime_t low_interval = ktime_set(50, 0);
	const ktime_t slack = ktime_set(20, 0);
	ktime_t now;
	ktime_t next_alarm;
	unsigned long flags;

	ds2784_battery_update_status(di);

	battery_adjust_charge_state(di);

	now = alarm_get_elapsed_realtime();
	next_alarm = ktime_add(now, low_interval);

	/* prevent suspend before starting the alarm */
	local_irq_save(flags);

	wake_unlock(&di->work_wake_lock);
	alarm_start_range(&di->alarm, next_alarm, ktime_add(next_alarm, slack));
	local_irq_restore(flags);
}

static void ds2784_battery_alarm(struct alarm *alarm)
{
	struct ds2784_device_info *di =
		container_of(alarm, struct ds2784_device_info, alarm);
	wake_lock(&di->work_wake_lock);
	queue_work(di->monitor_wqueue, &di->monitor_work);
}

static void battery_ext_power_changed(struct power_supply *psy)
{
	struct ds2784_device_info *di;
	int got_power;

	di = psy_to_dev_info(psy);
	got_power = power_supply_am_i_supplied(psy);

	pr_info("*** batt ext power changed (%d) ***\n", got_power);

	if (got_power) {
		di->charging_source = CHARGER_USB;
		wake_lock(&vbus_wake_lock);
	} else {
		di->charging_source = CHARGER_BATTERY;
		/* give userspace some time to see the uevent and update
		 * LED state or whatnot...
		 */
		wake_lock_timeout(&vbus_wake_lock, HZ / 2);
	}
	battery_adjust_charge_state(di);
	power_supply_changed(psy);
}

void notify_usb_connected(int online) 
{
}


static int ds2784_battery_probe(struct platform_device *pdev)
{
	int rc;
	struct ds2784_device_info *di;
	struct ds2784_platform_data *pdata;

	di = kzalloc(sizeof(*di), GFP_KERNEL);
	if (!di)
		return -ENOMEM;

	di->update_time = jiffies;
	platform_set_drvdata(pdev, di);

	pdata = pdev->dev.platform_data;
	di->dev = &pdev->dev;
	di->w1_dev = pdev->dev.parent;

	di->bat.name = "battery";
	di->bat.type = POWER_SUPPLY_TYPE_BATTERY;
	di->bat.properties = battery_properties;
	di->bat.num_properties = ARRAY_SIZE(battery_properties);
	di->bat.external_power_changed = battery_ext_power_changed;
	di->bat.get_property = battery_get_property;

	rc = power_supply_register(&pdev->dev, &di->bat);
	if (rc)
		goto fail_register;

	INIT_WORK(&di->monitor_work, ds2784_battery_work);
	di->monitor_wqueue = create_singlethread_workqueue(dev_name(&pdev->dev));
	if (!di->monitor_wqueue) {
		rc = -ESRCH;
		goto fail_workqueue;
	}
	wake_lock_init(&di->work_wake_lock, WAKE_LOCK_SUSPEND,
			"ds2784-battery");
	alarm_init(&di->alarm, ANDROID_ALARM_ELAPSED_REALTIME_WAKEUP,
			ds2784_battery_alarm);
	wake_lock(&di->work_wake_lock);
	queue_work(di->monitor_wqueue, &di->monitor_work);
	return 0;

fail_workqueue:
	power_supply_unregister(&di->bat);
fail_register:
	kfree(di);
	return rc;
}

static struct platform_driver ds2784_battery_driver = {
	.driver = {
		.name = "ds2784-battery",
	},
	.probe	  = ds2784_battery_probe,
};

static int __init ds2784_battery_init(void)
{
	spin_lock_init(&charge_state_lock);
	wake_lock_init(&vbus_wake_lock, WAKE_LOCK_SUSPEND, "vbus_present");
	return platform_driver_register(&ds2784_battery_driver);
}

module_init(ds2784_battery_init);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Justin Lin <Justin_lin@htc.com>");
MODULE_DESCRIPTION("ds2784 battery driver");
