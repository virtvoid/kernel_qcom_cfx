/* Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/mutex.h>
#include <linux/msm_tsens.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>

#define DEF_TEMP_SENSOR      0

static int enabled;

//Highest thermal limit
#define DEF_ALLOWED_MAX_HIGH 76
#define DEF_ALLOWED_MAX_FREQ 384000
static int allowed_max_high = DEF_ALLOWED_MAX_HIGH;
static int allowed_max_low = (DEF_ALLOWED_MAX_HIGH - 4);
static int allowed_max_freq = DEF_ALLOWED_MAX_FREQ;

//mid thermal limit
#define DEF_ALLOWED_MID_HIGH 72
#define DEF_ALLOWED_MID_FREQ 648000
static int allowed_mid_high = DEF_ALLOWED_MID_HIGH;
static int allowed_mid_low = (DEF_ALLOWED_MID_HIGH - 4);
static int allowed_mid_freq = DEF_ALLOWED_MID_FREQ;

//low thermal limit
#define DEF_ALLOWED_LOW_HIGH 70
#define DEF_ALLOWED_LOW_FREQ 972000
static int allowed_low_high = DEF_ALLOWED_LOW_HIGH;
static int allowed_low_low = (DEF_ALLOWED_LOW_HIGH - 4);
static int allowed_low_freq = DEF_ALLOWED_LOW_FREQ;

//Sampling interval
#define DEF_THERMAL_CHECK_MS 1000
static int check_interval_ms = DEF_THERMAL_CHECK_MS;

//Throttling indicator, 0=not throttled, 1=low, 2=mid, 3=max
static int thermal_throttled = 0;

//Safe the cpu max freq before throttling
static int pre_throttled_max = 0;

module_param(allowed_max_high, int, 0);
module_param(allowed_max_freq, int, 0);
module_param(check_interval_ms, int, 0);

static struct delayed_work check_temp_work;

static int update_cpu_max_freq(struct cpufreq_policy *cpu_policy,
			       int cpu, int max_freq)
{
	int ret = 0;

	if (!cpu_policy)
		return -EINVAL;

	cpufreq_verify_within_limits(cpu_policy,
				cpu_policy->min, max_freq);
	cpu_policy->user_policy.max = max_freq;

	ret = cpufreq_update_policy(cpu);
	if (!ret)
		pr_info("msm_thermal: Limiting core%d max frequency to %d\n",
			cpu, max_freq);

	return ret;
}

static void __cpuinit do_core_control(long temp)
{
	int i = 0;
	int ret = 0;

	if (!core_control_enabled)
		return;

	/**
	 *  Offline cores starting from the max MPIDR to 1, when above limit,
	 *  The core control mask is non zero and allows the core to be turned
	 *  off.
	 *  The core was not previously offlined by this module
	 *  The core is the next in sequence.
	 *  If the core was online for some reason, even after it was offlined
	 *  by this module, offline it again.
	 *  Online the back on if the temp is below the hysteresis and was
	 *  offlined by this module and not already online.
	 */
	mutex_lock(&core_control_mutex);
	if (msm_thermal_info.core_control_mask &&
		temp >= msm_thermal_info.core_limit_temp_degC) {
		for (i = num_possible_cpus(); i > 0; i--) {
			if (!(msm_thermal_info.core_control_mask & BIT(i)))
				continue;
			if (cpus_offlined & BIT(i) && !cpu_online(i))
				continue;
			pr_info("%s: Set Offline: CPU%d Temp: %ld\n",
					KBUILD_MODNAME, i, temp);
			ret = cpu_down(i);
			if (ret)
				pr_err("%s: Error %d offline core %d\n",
					KBUILD_MODNAME, ret, i);
			cpus_offlined |= BIT(i);
			break;
		}
	} else if (msm_thermal_info.core_control_mask && cpus_offlined &&
		temp <= (msm_thermal_info.core_limit_temp_degC -
			msm_thermal_info.core_temp_hysteresis_degC)) {
		for (i = 0; i < num_possible_cpus(); i++) {
			if (!(cpus_offlined & BIT(i)))
				continue;
			cpus_offlined &= ~BIT(i);
			pr_info("%s: Allow Online CPU%d Temp: %ld\n",
					KBUILD_MODNAME, i, temp);
			/* If this core is already online, then bring up the
			 * next offlined core.
			 */
			if (cpu_online(i))
				continue;
			ret = cpu_up(i);
			if (ret)
				pr_err("%s: Error %d online core %d\n",
						KBUILD_MODNAME, ret, i);
			break;
		}
	}
	mutex_unlock(&core_control_mutex);
}

static void __cpuinit check_temp(struct work_struct *work)
{
	struct cpufreq_policy *cpu_policy = NULL;
	struct tsens_device tsens_dev;
	unsigned long temp = 0;
	unsigned int max_freq = 0;
	int update_policy = 0;
	int cpu = 0;
	int ret = 0;

	tsens_dev.sensor_num = DEF_TEMP_SENSOR;
	ret = tsens_get_temp(&tsens_dev, &temp);
	if (ret) {
		pr_err("msm_thermal: Unable to read TSENS sensor %d\n",
				tsens_dev.sensor_num);
		goto reschedule;
	}

	for_each_possible_cpu(cpu) {
		update_policy = 0;
		cpu_policy = cpufreq_cpu_get(cpu);
		if (!cpu_policy) {
			pr_debug("msm_thermal: NULL policy on cpu %d\n", cpu);
			continue;
		}

		//low trip point
		if ((temp >= allowed_low_high) &&
		    (temp < allowed_mid_high) &&
		    (cpu_policy->max > allowed_low_freq)) {
			update_policy = 1;
			/* save pre-throttled max freq value */
			pre_throttled_max = cpu_policy->max;
			max_freq = allowed_low_freq;
			thermal_throttled = 1;
			pr_warn("msm_thermal: Thermal Throttled (low)! temp: %lu\n", temp);
		//low clr point
		} else if ((temp < allowed_low_low) &&
			   (thermal_throttled > 0)) {
			if (cpu_policy->max < cpu_policy->cpuinfo.max_freq) {
				if (pre_throttled_max != 0)
					max_freq = pre_throttled_max;
				else {
					max_freq = 1566000;
					pr_warn("msm_thermal: ERROR! pre_throttled_max=0, falling back to %u\n", max_freq);
				}
				update_policy = 1;
				/* wait until 2nd core is unthrottled */
				if (cpu == 1)
					thermal_throttled = 0;
				pr_warn("msm_thermal: Low Thermal Throttling Ended! temp: %lu\n", temp);
			}
		//mid trip point
		} else if ((temp >= allowed_low_high) &&
			   (temp < allowed_mid_low) &&
			   (cpu_policy->max > allowed_mid_freq)) {
			update_policy = 1;
			max_freq = allowed_low_freq;
			thermal_throttled = 2;
			pr_warn("msm_thermal: Thermal Throttled (mid)! temp: %lu\n", temp);
		//mid clr point
		} else if ( (temp < allowed_mid_low) &&
			   (thermal_throttled > 1)) {
			if (cpu_policy->max < cpu_policy->cpuinfo.max_freq) {
				max_freq = allowed_low_freq;
				update_policy = 1;
				/* wait until 2nd core is unthrottled */
				if (cpu == 1)
					thermal_throttled = 1;
				pr_warn("msm_thermal: Mid Thermal Throttling Ended! temp: %lu\n", temp);
			}
		//max trip point
		} else if ((temp >= allowed_max_high) &&
			   (cpu_policy->max > allowed_max_freq)) {
			update_policy = 1;
			max_freq = allowed_max_freq;
			thermal_throttled = 3;
			pr_warn("msm_thermal: Thermal Throttled (max)! temp: %lu\n", temp);
		//max clr point
		} else if ((temp < allowed_max_low) &&
			   (thermal_throttled > 2)) {
			if (cpu_policy->max < cpu_policy->cpuinfo.max_freq) {
				max_freq = allowed_mid_freq;
				update_policy = 1;
				/* wait until 2nd core is unthrottled */
				if (cpu == 1)
					thermal_throttled = 2;
				pr_warn("msm_thermal: Max Thermal Throttling Ended! temp: %lu\n", temp);
			}
		}

		if (update_policy)
			update_cpu_max_freq(cpu_policy, cpu, max_freq);

		cpufreq_cpu_put(cpu_policy);
	}

reschedule:
	if (enabled)
		schedule_delayed_work(&check_temp_work,
				msecs_to_jiffies(check_interval_ms));
}

static int __cpuinit msm_thermal_cpu_callback(struct notifier_block *nfb,
		unsigned long action, void *hcpu)
{
	unsigned int cpu = (unsigned long)hcpu;

	if (action == CPU_UP_PREPARE || action == CPU_UP_PREPARE_FROZEN) {
		if (core_control_enabled &&
			(msm_thermal_info.core_control_mask & BIT(cpu)) &&
			(cpus_offlined & BIT(cpu))) {
			pr_info(
			"%s: Preventing cpu%d from coming online.\n",
				KBUILD_MODNAME, cpu);
			return NOTIFY_BAD;
		}
	}


	return NOTIFY_OK;
}

static struct notifier_block __refdata msm_thermal_cpu_notifier = {
	.notifier_call = msm_thermal_cpu_callback,
};

/**
 * We will reset the cpu frequencies limits here. The core online/offline
 * status will be carried over to the process stopping the msm_thermal, as
 * we dont want to online a core and bring in the thermal issues.
 */
static void __cpuinit disable_msm_thermal(void)
{
	int cpu = 0;
	struct cpufreq_policy *cpu_policy = NULL;

	/* make sure check_temp is no longer running */
	cancel_delayed_work(&check_temp_work);
	flush_scheduled_work();

	if (limited_max_freq == MSM_CPUFREQ_NO_LIMIT)
		return;

	for_each_possible_cpu(cpu) {
		cpu_policy = cpufreq_cpu_get(cpu);
		if (cpu_policy) {
			if (cpu_policy->max < cpu_policy->cpuinfo.max_freq)
				update_cpu_max_freq(cpu_policy, cpu,
						    cpu_policy->
						    cpuinfo.max_freq);
			cpufreq_cpu_put(cpu_policy);
		}
	}
}

static int __cpuinit set_enabled(const char *val, const struct kernel_param *kp)
{
	int ret = 0;

	ret = param_set_bool(val, kp);
	if (!enabled)
		disable_msm_thermal();
	else
		pr_info("%s: no action for enabled = %d\n",
				KBUILD_MODNAME, enabled);

	pr_info("%s: enabled = %d\n", KBUILD_MODNAME, enabled);

	return ret;
}

static struct kernel_param_ops module_ops = {
	.set = set_enabled,
	.get = param_get_bool,
};

module_param_cb(enabled, &module_ops, &enabled, 0644);
MODULE_PARM_DESC(enabled, "enforce thermal limit on cpu");

static int __init msm_thermal_init(void)
{
	int ret = 0;

	enabled = 1;
	core_control_enabled = 1;
	INIT_DELAYED_WORK(&check_temp_work, check_temp);
	schedule_delayed_work(&check_temp_work, 0);

	register_cpu_notifier(&msm_thermal_cpu_notifier);

	return ret;
}
fs_initcall(msm_thermal_init);
