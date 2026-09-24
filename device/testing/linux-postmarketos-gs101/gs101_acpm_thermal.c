// SPDX-License-Identifier: GPL-2.0-only
/* Read-only GS101 thermal sensor access through ACPM firmware. */

#include <linux/firmware/samsung/exynos-acpm-protocol.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>

#define GS101_TMU_READ_TEMP	0x02
#define GS101_TMU_CHANNEL	9
#define GS101_TMU_COUNT		6

struct gs101_tmu_msg {
	u16 ctx;
	u16 fw_use;
	u8 type;
	s8 ret;
	u8 tzid;
	u8 temp;
	u8 stat;
	u8 reserved[7];
} __packed;

struct gs101_thermal;

struct gs101_sensor {
	struct gs101_thermal *thermal;
	u8 id;
};

struct gs101_thermal {
	struct acpm_handle *acpm;
	u32 channel;
	struct gs101_sensor sensors[GS101_TMU_COUNT];
};

static int gs101_thermal_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct gs101_sensor *sensor = thermal_zone_device_priv(tz);
	struct gs101_tmu_msg msg = {
		.type = GS101_TMU_READ_TEMP,
		.tzid = sensor->id,
	};
	struct acpm_xfer xfer = {
		.txd = (u32 *)&msg,
		.rxd = (u32 *)&msg,
		.txlen = sizeof(msg),
		.rxlen = sizeof(msg),
		.acpm_chan_id = sensor->thermal->channel,
	};
	int ret;

	ret = acpm_do_xfer(sensor->thermal->acpm, &xfer);
	if (ret)
		return ret;
	if (msg.ret < 0 || msg.tzid != sensor->id)
		return -EIO;

	*temp = msg.temp * 1000;
	return 0;
}

static const struct thermal_zone_device_ops gs101_thermal_ops = {
	.get_temp = gs101_thermal_get_temp,
};

static int gs101_thermal_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gs101_thermal *thermal;
	struct device_node *acpm_np;
	struct thermal_zone_device *tz;
	unsigned int i;
	int ret;

	thermal = devm_kzalloc(dev, sizeof(*thermal), GFP_KERNEL);
	if (!thermal)
		return -ENOMEM;

	acpm_np = of_parse_phandle(dev->of_node, "samsung,acpm", 0);
	if (!acpm_np)
		return dev_err_probe(dev, -EINVAL, "missing samsung,acpm\n");
	thermal->acpm = devm_acpm_get_by_node(dev, acpm_np);
	of_node_put(acpm_np);
	if (IS_ERR(thermal->acpm))
		return dev_err_probe(dev, PTR_ERR(thermal->acpm), "ACPM unavailable\n");

	thermal->channel = GS101_TMU_CHANNEL;
	of_property_read_u32(dev->of_node, "samsung,acpm-channel",
			     &thermal->channel);

	for (i = 0; i < GS101_TMU_COUNT; i++) {
		thermal->sensors[i].thermal = thermal;
		thermal->sensors[i].id = i;
		tz = devm_thermal_of_zone_register(dev, i, &thermal->sensors[i],
						   &gs101_thermal_ops);
		if (IS_ERR(tz)) {
			ret = PTR_ERR(tz);
			if (ret != -ENODEV)
				return dev_err_probe(dev, ret,
						     "failed to register sensor %u\n", i);
		}
	}

	dev_info(dev, "registered six read-only ACPM thermal sensors\n");
	return 0;
}

static const struct of_device_id gs101_thermal_of_match[] = {
	{ .compatible = "google,gs101-acpm-thermal" },
	{ }
};
MODULE_DEVICE_TABLE(of, gs101_thermal_of_match);

static struct platform_driver gs101_thermal_driver = {
	.probe = gs101_thermal_probe,
	.driver = {
		.name = "gs101-acpm-thermal",
		.of_match_table = gs101_thermal_of_match,
	},
};
module_platform_driver(gs101_thermal_driver);

MODULE_DESCRIPTION("Read-only Google GS101 ACPM thermal sensor driver");
MODULE_LICENSE("GPL");
