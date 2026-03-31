/*
 * Copyright (c) 2025 Core Devices LLC
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT sifli_sf32lb_gpio

#include <stdint.h>

#include <zephyr/arch/cpu.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_utils.h>
#include <zephyr/drivers/clock_control/sf32lb.h>
#include <zephyr/irq.h>
#include <zephyr/sys/math_extras.h>

#include <register.h>

#define GPIO1_DIRX   offsetof(GPIO1_TypeDef, DIR0)
#define GPIO1_DORX   offsetof(GPIO1_TypeDef, DOR0)
#define GPIO1_DOSRX  offsetof(GPIO1_TypeDef, DOSR0)
#define GPIO1_DOCRX  offsetof(GPIO1_TypeDef, DOCR0)
#define GPIO1_DOERX  offsetof(GPIO1_TypeDef, DOER0)
#define GPIO1_DOESRX offsetof(GPIO1_TypeDef, DOESR0)
#define GPIO1_DOECRX offsetof(GPIO1_TypeDef, DOECR0)
#define GPIO1_IESRX  offsetof(GPIO1_TypeDef, IESR0)
#define GPIO1_IECRX  offsetof(GPIO1_TypeDef, IECR0)
#define GPIO1_ISRX   offsetof(GPIO1_TypeDef, ISR0)
#define GPIO1_ITSRX  offsetof(GPIO1_TypeDef, ITSR0)
#define GPIO1_ITCRX  offsetof(GPIO1_TypeDef, ITCR0)
#define GPIO1_IPHCRX offsetof(GPIO1_TypeDef, IPHCR0)
#define GPIO1_IPLCRX offsetof(GPIO1_TypeDef, IPLCR0)
#define GPIO1_IPHSRX offsetof(GPIO1_TypeDef, IPHSR0)
#define GPIO1_IPLSRX offsetof(GPIO1_TypeDef, IPLSR0)

#define PINMUX_PAD_XXYY_PE      BIT(4)
#define PINMUX_PAD_XXYY_PS_PUP  BIT(5)
#define PINMUX_PAD_XXYY_IE      BIT(6)
#define PINMUX_PAD_XXYY_SR_SLOW BIT(8)

struct gpio_sf32lb_config {
	struct gpio_driver_config common;
	uintptr_t gpio;
	uintptr_t pinmux;
	struct sf32lb_clock_dt_spec parent_clk;
};

struct gpio_sf32lb_data {
	struct gpio_driver_data common;
	sys_slist_t callbacks;
	gpio_port_pins_t od;
};

static inline int gpio_sf32lb_configure(const struct device *port, gpio_pin_t pin,
					gpio_flags_t flags)
{
	const struct gpio_sf32lb_config *config = port->config;
	struct gpio_sf32lb_data *data = port->data;
	uint32_t val;

	if ((flags & GPIO_OUTPUT) != 0U) {
		/* disable ISR */
		sys_write32(BIT(pin), config->gpio + GPIO1_IECRX);

		if ((flags & GPIO_SINGLE_ENDED) != 0U) {
			if ((flags & GPIO_LINE_OPEN_DRAIN) == 0U) {
				return -ENOTSUP;
			}

			data->od |= BIT(pin);

			/* disable O */
			sys_write32(BIT(pin), config->gpio + GPIO1_DOCRX);

			/* set initial state (OE) */
			if ((flags & GPIO_OUTPUT_INIT_HIGH) != 0U) {
				sys_write32(BIT(pin), config->gpio + GPIO1_DOESRX);
			} else if ((flags & GPIO_OUTPUT_INIT_LOW) != 0U) {
				sys_write32(BIT(pin), config->gpio + GPIO1_DOECRX);
			}
		} else {
			data->od &= ~BIT(pin);

			/* enable OE */
			sys_write32(BIT(pin), config->gpio + GPIO1_DOESRX);

			/* set initial state (O) */
			if ((flags & GPIO_OUTPUT_INIT_HIGH) != 0U) {
				sys_write32(BIT(pin), config->gpio + GPIO1_DOSRX);
			} else if ((flags & GPIO_OUTPUT_INIT_LOW) != 0U) {
				sys_write32(BIT(pin), config->gpio + GPIO1_DOCRX);
			}
		}
	} else if ((flags & GPIO_INPUT) != 0U) {
		data->od &= ~BIT(pin);

		/* disable OE */
		sys_write32(BIT(pin), config->gpio + GPIO1_DOECRX);
	} else {
		return -ENOTSUP;
	}

	/* configure pad settings in PINMUX */
	val = PINMUX_PAD_XXYY_SR_SLOW;

	/* Always enable IE — SiFli pads require IE for proper output drive.
	 * RT-Thread HAL_PIN_Set always sets IE even for output pins. */
	val |= PINMUX_PAD_XXYY_IE;

	if ((flags & GPIO_PULL_UP) != 0U) {
		val |= PINMUX_PAD_XXYY_PE | PINMUX_PAD_XXYY_PS_PUP;
	} else if ((flags & GPIO_PULL_DOWN) != 0U) {
		val |= PINMUX_PAD_XXYY_PE;
	}

	sys_write32(val, config->pinmux + (pin * 4U));

	return 0;
}

static int gpio_sf32lb_port_get_raw(const struct device *port, uint32_t *value)
{
	const struct gpio_sf32lb_config *config = port->config;

	*value = sys_read32(config->gpio + GPIO1_DIRX);

	return 0;
}

static int gpio_sf32lb_port_set_masked_raw(const struct device *port, gpio_port_pins_t mask,
					   gpio_port_value_t value)
{
	const struct gpio_sf32lb_config *config = port->config;
	struct gpio_sf32lb_data *data = port->data;
	gpio_port_pins_t pp_mask, od_mask;
	uint32_t val;

	pp_mask = mask & ~data->od;
	if (pp_mask != 0U) {
		val = sys_read32(config->gpio + GPIO1_DORX);
		val = (val & ~pp_mask) | (value & pp_mask);
		sys_write32(val, config->gpio + GPIO1_DORX);
	}

	od_mask = mask & data->od;
	if (od_mask != 0U) {
		val = sys_read32(config->gpio + GPIO1_DOERX);
		val = (val & ~od_mask) | (value & od_mask);
		sys_write32(val, config->gpio + GPIO1_DOERX);
	}

	return 0;
}

static int gpio_sf32lb_port_set_bits_raw(const struct device *port, gpio_port_pins_t pins)
{
	const struct gpio_sf32lb_config *config = port->config;
	struct gpio_sf32lb_data *data = port->data;
	gpio_port_pins_t pp_pins, od_pins;

	pp_pins = pins & ~data->od;
	sys_write32(pp_pins, config->gpio + GPIO1_DOSRX);

	od_pins = pins & data->od;
	sys_write32(od_pins, config->gpio + GPIO1_DOESRX);

	return 0;
}

static int gpio_sf32lb_port_clear_bits_raw(const struct device *port, gpio_port_pins_t pins)
{
	const struct gpio_sf32lb_config *config = port->config;
	struct gpio_sf32lb_data *data = port->data;
	gpio_port_pins_t pp_pins, od_pins;

	pp_pins = pins & ~data->od;
	sys_write32(pp_pins, config->gpio + GPIO1_DOCRX);

	od_pins = pins & data->od;
	sys_write32(od_pins, config->gpio + GPIO1_DOECRX);

	return 0;
}

static int gpio_sf32lb_port_toggle_bits(const struct device *port, gpio_port_pins_t pins)
{
	const struct gpio_sf32lb_config *config = port->config;
	struct gpio_sf32lb_data *data = port->data;
	gpio_port_pins_t pp_pins, od_pins;
	uint32_t val;

	pp_pins = pins & ~data->od;
	if (pp_pins != 0U) {
		val = sys_read32(config->gpio + GPIO1_DORX);
		val ^= pp_pins;
		sys_write32(val, config->gpio + GPIO1_DORX);
	}

	od_pins = pins & data->od;
	if (od_pins != 0U) {
		val = sys_read32(config->gpio + GPIO1_DOERX);
		val ^= od_pins;
		sys_write32(val, config->gpio + GPIO1_DOERX);
	}

	return 0;
}

static int gpio_sf32lb_pin_interrupt_configure(const struct device *port, gpio_pin_t pin,
					       enum gpio_int_mode mode, enum gpio_int_trig trig)
{
	const struct gpio_sf32lb_config *config = port->config;

	if (mode == GPIO_INT_MODE_DISABLED) {
		sys_write32(BIT(pin), config->gpio + GPIO1_IECRX);
	} else if ((mode == GPIO_INT_MODE_EDGE) || (mode == GPIO_INT_MODE_LEVEL)) {
		if (mode == GPIO_INT_MODE_EDGE) {
			sys_write32(BIT(pin), config->gpio + GPIO1_ITSRX);
		} else {
			sys_write32(BIT(pin), config->gpio + GPIO1_ITCRX);
		}

		switch (trig) {
		case GPIO_INT_TRIG_LOW:
			sys_write32(BIT(pin), config->gpio + GPIO1_IPHCRX);
			sys_write32(BIT(pin), config->gpio + GPIO1_IPLSRX);
			break;
		case GPIO_INT_TRIG_HIGH:
			sys_write32(BIT(pin), config->gpio + GPIO1_IPHSRX);
			sys_write32(BIT(pin), config->gpio + GPIO1_IPLCRX);
			break;
		case GPIO_INT_TRIG_BOTH:
			sys_write32(BIT(pin), config->gpio + GPIO1_IPHSRX);
			sys_write32(BIT(pin), config->gpio + GPIO1_IPLSRX);
			break;
		default:
			return -ENOTSUP;
		}

		sys_write32(BIT(pin), config->gpio + GPIO1_IESRX);
	} else {
		return -ENOTSUP;
	}

	return 0;
}

static int gpio_sf32lb_manage_callback(const struct device *dev, struct gpio_callback *callback,
				       bool set)
{
	struct gpio_sf32lb_data *data = dev->data;

	return gpio_manage_callback(&data->callbacks, callback, set);
}

static DEVICE_API(gpio, gpio_sf32lb_api) = {
	.pin_configure = gpio_sf32lb_configure,
	.port_get_raw = gpio_sf32lb_port_get_raw,
	.port_set_masked_raw = gpio_sf32lb_port_set_masked_raw,
	.port_set_bits_raw = gpio_sf32lb_port_set_bits_raw,
	.port_clear_bits_raw = gpio_sf32lb_port_clear_bits_raw,
	.port_toggle_bits = gpio_sf32lb_port_toggle_bits,
	.pin_interrupt_configure = gpio_sf32lb_pin_interrupt_configure,
	.manage_callback = gpio_sf32lb_manage_callback,
};

/* Forward declaration — defined after all instances so DEVICE_DT_INST_GET works. */
static void gpio_sf32lb_isr(const void *arg);

/*
 * Per-instance macro: defines config/data structs and an init function.
 *
 * IRQ registration uses CONFIG_DYNAMIC_INTERRUPTS so IRQ_CONNECT is a
 * pure runtime call (no compile-time .intList entry). This allows multiple
 * instances sharing the same parent IRQ (e.g. gpioa_00_31 and gpioa_32_44
 * both on IRQ 84) to each call IRQ_CONNECT with the same handler function
 * (gpio_sf32lb_isr). The second call is a no-op since it writes the same
 * function pointer to the same IRQ slot. irq_enable() is idempotent.
 */
#define GPIO_SF32LB_DEFINE(n)                                                                      \
	static const struct gpio_sf32lb_config gpio_sf32lb_config##n = {                          \
		.common = GPIO_COMMON_CONFIG_FROM_DT_INST(n),                                      \
		.gpio = DT_INST_REG_ADDR(n),                                                       \
		.pinmux = DT_REG_ADDR_BY_IDX(DT_INST_PHANDLE(n, sifli_pinmuxs),                  \
					      DT_INST_PHA(n, sifli_pinmuxs, port)) +          \
			  DT_INST_PHA(n, sifli_pinmuxs, offset),                                   \
		.parent_clk = SF32LB_CLOCK_DT_SPEC_GET_OR(DT_INST_PARENT(n),                     \
				((struct sf32lb_clock_dt_spec){.dev = NULL, .id = 0})),            \
	};                                                                                         \
                                                                                                   \
	static struct gpio_sf32lb_data gpio_sf32lb_data##n;                                        \
                                                                                                   \
	static int gpio_sf32lb_init_##n(const struct device *dev)                                 \
	{                                                                                          \
		const struct gpio_sf32lb_config *cfg = dev->config;                                \
                                                                                                   \
		if (cfg->parent_clk.dev != NULL) {                                                 \
			if (!sf32lb_clock_is_ready_dt(&cfg->parent_clk)) {                         \
				return -ENODEV;                                                    \
			}                                                                          \
			(void)sf32lb_clock_control_on_dt(&cfg->parent_clk);                        \
		}                                                                                   \
                                                                                                   \
		/* Use irq_connect_dynamic to avoid duplicate Z_ISR_DECLARE entries    \
		 * when multiple child instances share the same parent IRQ (e.g.       \
		 * gpioa_00_31 and gpioa_32_56 both on IRQ 84). IRQ_CONNECT emits a   \
		 * static .intList entry which causes gen_isr_tables.py to fail.       \
		 */                                                                    \
		irq_connect_dynamic(DT_IRQN(DT_INST_PARENT(n)),                       \
				    DT_IRQ(DT_INST_PARENT(n), priority),               \
				    gpio_sf32lb_isr, NULL, 0);                         \
		irq_enable(DT_IRQN(DT_INST_PARENT(n)));                                           \
                                                                                                   \
		return 0;                                                                          \
	}                                                                                          \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, gpio_sf32lb_init_##n, NULL, &gpio_sf32lb_data##n,                \
			      &gpio_sf32lb_config##n, PRE_KERNEL_1, CONFIG_GPIO_INIT_PRIORITY,     \
			      &gpio_sf32lb_api);

DT_INST_FOREACH_STATUS_OKAY(GPIO_SF32LB_DEFINE)

/*
 * Single shared ISR: iterates ALL enabled gpio instances and dispatches
 * callbacks for any with pending interrupt bits. Defined after the instances
 * so DEVICE_DT_INST_GET is valid.
 *
 * Each instance reads its own ISR register; instances whose parent IRQ did
 * not fire will have val == 0 and exit immediately. This avoids the need for
 * per-parent sibling arrays and removes the duplicate IRQ_CONNECT issue that
 * arises when multiple children share one parent IRQ.
 */
#define GPIO_SF32LB_ISR_DISPATCH(n)                                                                \
	{                                                                                          \
		const struct device *_dev = DEVICE_DT_INST_GET(n);                                \
		const struct gpio_sf32lb_config *_cfg = _dev->config;                             \
		struct gpio_sf32lb_data *_data = _dev->data;                                       \
		uint32_t _val = sys_read32(_cfg->gpio + GPIO1_ISRX);                              \
                                                                                                   \
		if (_val != 0U) {                                                                  \
			uint8_t _min = u32_count_trailing_zeros(_cfg->common.port_pin_mask);       \
			uint8_t _max = 32 - u32_count_leading_zeros(_cfg->common.port_pin_mask);   \
                                                                                                   \
			for (uint8_t _i = _min; _i < _max; _i++) {                                \
				if ((_val & BIT(_i)) != 0U) {                                      \
					gpio_fire_callbacks(&_data->callbacks, _dev, BIT(_i));     \
				}                                                                  \
			}                                                                          \
			sys_write32(_val, _cfg->gpio + GPIO1_ISRX);                                \
		}                                                                                   \
	}

static void gpio_sf32lb_isr(const void *arg)
{
	ARG_UNUSED(arg);
	DT_INST_FOREACH_STATUS_OKAY(GPIO_SF32LB_ISR_DISPATCH)
}
