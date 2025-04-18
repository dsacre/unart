// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * unart - definitely not a real UART
 * Copyright (c) 2025 Dominic Sacré <dominic@sacre.net>
 */
#include "unart.h"

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/gpio/machine.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/stringify.h>

struct unart_module_params unart_params = {
	.gpiochip = NULL,
	.rx_gpio = -1,
	.tx_gpio = -1,
	.rx_skew = UNART_DEFAULT_RX_SKEW,
	.rx_debug = false,
};

module_param_named(gpiochip, unart_params.gpiochip, charp, 0444);
module_param_named(rx_gpio, unart_params.rx_gpio, int, 0444);
module_param_named(tx_gpio, unart_params.tx_gpio, int, 0444);
module_param_named(rx_skew, unart_params.rx_skew, int, 0444);
module_param_named(rx_debug, unart_params.rx_debug, bool, 0644);

MODULE_PARM_DESC(gpiochip, "GPIO chip for RX and TX");
MODULE_PARM_DESC(rx_gpio, "GPIO index for RX");
MODULE_PARM_DESC(tx_gpio, "GPIO index for TX");

/**
 * Offset where sampling the RX line takes place. This is a percentage within
 * the expected time frame of one bit, so a value of 50 would represent the
 * center.
 * The default is below 50 to allow for a certain amount of IRQ jitter. But
 * note that deviating too far from the center makes RX susceptible to clock
 * drift and bad signal quality.
 */
MODULE_PARM_DESC(rx_skew, "sample offset for RX (0-100, default "
			  __stringify(UNART_DEFAULT_RX_SKEW)")");
/**
 * Repurpose the TX line to measure the timing of RX sampling.
 * When enabled, all TX data is ignored. Instead the TX line is toggled every
 * time the RX line is sampled, in order to assess how well sampling lines up
 * with the signal.
 */
MODULE_PARM_DESC(rx_debug, "toggle TX line when RX is sampled");


static struct platform_device *manual_pdev;

/**
 * Add an unart device based on module parameters, if specified.
 */
static int unart_manual_device_init(void)
{
	const char *gpiochip = unart_params.gpiochip;
	int rx_gpio = unart_params.rx_gpio;
	int tx_gpio = unart_params.tx_gpio;

	if (!gpiochip && rx_gpio == -1 && tx_gpio == -1) {
		return 0;
	} else if (!gpiochip || rx_gpio == -1 || tx_gpio == -1) {
		pr_err("unart: Incomplete manual device configuration\n");
		return -EINVAL;
	}

	pr_info("unart: Registering manual device instance: "
		"gpiochip=%s, rx_gpio=%d, tx_gpio=%d\n",
		gpiochip, rx_gpio, tx_gpio);

	manual_pdev = platform_device_alloc("unart", PLATFORM_DEVID_NONE);
	if (!manual_pdev)
		return -ENOMEM;

	// Add a lookup table just for the duration of platform_device_add().
	static struct gpiod_lookup_table lookup = {
		.dev_id = "unart",
		.table = { {/*rx*/}, {/*tx*/}, {/*sentinel*/} }
	};
	lookup.table[0] = GPIO_LOOKUP(gpiochip, rx_gpio, "rx", 0);
	lookup.table[1] = GPIO_LOOKUP(gpiochip, tx_gpio, "tx", 0);

	gpiod_add_lookup_table(&lookup);

	int err = platform_device_add(manual_pdev);
	if (err) {
		pr_err("unart: Failed to add manual device instance\n");
		platform_device_put(manual_pdev);
		gpiod_remove_lookup_table(&lookup);
		return err;
	}

	gpiod_remove_lookup_table(&lookup);
	return 0;
}


static int unart_probe(struct platform_device *pdev)
{
	int err;

	struct unart *unart = devm_kzalloc(&pdev->dev, sizeof(*unart), GFP_KERNEL);
	if (!unart)
		return -ENOMEM;

	platform_set_drvdata(pdev, unart);

	err = unart_rx_setup(pdev, &unart->rx);
	if (err)
		return err;

	err = unart_tx_setup(pdev, &unart->tx);
	if (err)
		return err;

	err = unart_tty_device_setup(pdev, unart);
	if (err)
		return err;

	return 0;
}

static const struct of_device_id unart_of_match[] = {
	{ .compatible = "dsacre,unart" },
	{ /*sentinel*/ }
};
MODULE_DEVICE_TABLE(of, unart_of_match);

static struct platform_driver unart_driver = {
	.probe = unart_probe,
	.driver = {
		.name = "unart",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(unart_of_match),
	}
};

static int unart_init(void)
{
	int err;

	err = unart_tty_register_driver();
	if (err)
		return err;

	err = platform_driver_register(&unart_driver);
	if (err) {
		unart_tty_unregister_driver();
		return err;
	}

	err = unart_manual_device_init();
	if (err) {
		platform_driver_unregister(&unart_driver);
		unart_tty_unregister_driver();
		return err;
	}

	return 0;
}

static void unart_exit(void)
{
	if (manual_pdev)
		platform_device_unregister(manual_pdev);

	platform_driver_unregister(&unart_driver);
	unart_tty_unregister_driver();
}

module_init(unart_init);
module_exit(unart_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dominic Sacré");
MODULE_DESCRIPTION("definitely not a real UART");
