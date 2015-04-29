/*
 * Copyright (c) 2015 Samsung Electronics Co., Ltd.
 *	      http://www.samsung.com/
 *
 * EXYNOS - SROM Controller support
 * Author: Pankaj Dubey <pankaj.dubey@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include "exynos-srom.h"

static void __iomem *exynos_srom_base;

static unsigned long exynos_srom_offsets[] = {
	/* SROM side */
	S5P_SROM_BW,
	S5P_SROM_BC0,
	S5P_SROM_BC1,
	S5P_SROM_BC2,
	S5P_SROM_BC3,
};

/**
 * struct exynos_srom_reg_dump: register dump of SROM Controller registers.
 * @offset: srom register offset from the controller base address.
 * @value: the value to be register at offset.
 */
struct exynos_srom_reg_dump {
	u32     offset;
	u32     value;
};

static struct exynos_srom_reg_dump *exynos_srom_regs;

static struct exynos_srom_reg_dump *exynos_srom_alloc_reg_dump(
		const unsigned long *rdump,
		unsigned long nr_rdump)
{
	struct exynos_srom_reg_dump *rd;
	unsigned int i;

	rd = kcalloc(nr_rdump, sizeof(*rd), GFP_KERNEL);
	if (!rd)
		return NULL;

	for (i = 0; i < nr_rdump; ++i)
		rd[i].offset = rdump[i];

	return rd;
}

static void exynos_srom_save(void __iomem *base,
				    struct exynos_srom_reg_dump *rd,
				    unsigned int num_regs)
{
	for (; num_regs > 0; --num_regs, ++rd)
		rd->value = readl(base + rd->offset);

}

static void exynos_srom_restore(void __iomem *base,
				      const struct exynos_srom_reg_dump *rd,
				      unsigned int num_regs)
{
	for (; num_regs > 0; --num_regs, ++rd)
		writel(rd->value, base + rd->offset);

}

static const struct of_device_id of_exynos_srom_ids[] = {
	{
		.compatible	= "samsung,exynos-srom",
	},
	{},
};

static int exynos_srom_probe(struct platform_device *pdev)
{
	struct device_node *np;
	struct device *dev = &pdev->dev;

	np = dev->of_node;
	exynos_srom_base = of_iomap(np, 0);

	if (!exynos_srom_base)
		return PTR_ERR(exynos_srom_base);

	exynos_srom_regs = exynos_srom_alloc_reg_dump(exynos_srom_offsets,
			sizeof(exynos_srom_offsets));

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int exynos_srom_suspend(struct device *dev)
{
	exynos_srom_save(exynos_srom_base, exynos_srom_regs,
				ARRAY_SIZE(exynos_srom_offsets));

	return 0;
}

static int exynos_srom_resume(struct device *dev)
{
	exynos_srom_restore(exynos_srom_base, exynos_srom_regs,
				ARRAY_SIZE(exynos_srom_offsets));

	return 0;
}

static const struct dev_pm_ops exynos_srom_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(exynos_srom_suspend, exynos_srom_resume)
};

#define DEV_PM_OPS	(&exynos_srom_dev_pm_ops)
#else
#define DEV_PM_OPS	NULL
#endif /* CONFIG_PM_SLEEP */

static struct platform_driver exynos_srom_driver = {
	.probe = exynos_srom_probe,
	.driver = {
		.name = "exynos-srom",
		.of_match_table = of_exynos_srom_ids,
		.pm = DEV_PM_OPS,
	},
};

static int __init exynos_srom_init(void)
{
	return platform_driver_register(&exynos_srom_driver);
}
device_initcall(exynos_srom_init);
