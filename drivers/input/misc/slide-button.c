
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/bitops.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/irq.h>
#include <linux/gpio.h>

#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/of_irq.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/of_gpio.h>

#define SLIDE_BUTTON_NODE "/dev/input/slider"
#define SLIDE_BUTTON_NAME "slide_button"
#define KEY_SLIDE_BUTTON_OFF 250
#define KEY_SLIDE_BUTTON_ON 251

struct slide_button_desc {
    int32_t gpio;
    int irq;
    bool wakeup;
    int prev_state;
    int debounce_interval;
    struct input_dev *input;
};

static const struct of_device_id slide_button_of_match[] = {
    { .compatible = "slide_button", },
    { },
};

static bool slide_button_get_state(struct slide_button_desc *sb) {
    return !!gpio_get_value_cansleep(sb->gpio);
}

static void slide_button_report(struct slide_button_desc *sb, bool state) {
    int keycode = slide_button_get_state(sb) 
                    ? KEY_SLIDE_BUTTON_ON
                    : KEY_SLIDE_BUTTON_OFF;
    
    input_report_key(sb->input, keycode, 1);
    input_sync(sb->input);
    input_report_key(sb->input, keycode, 0);
    input_sync(sb->input);
}

static irqreturn_t slide_button_irq_trigger_handler(int irq, void *data) {
    struct slide_button_desc *sb = data;
	bool state = slide_button_get_state(sb);
    if (state != sb->prev_state) {
        pr_debug("%s: slider is now %s\n", __func__, state ? "on" : "off");
        slide_button_report(sb, state);
        sb->prev_state = state;
    }

    return IRQ_HANDLED;
}

static int slide_button_parse_dt(struct device *dev, 
                                 struct slide_button_desc *sb) {
    int ret = -1;
    struct device_node *np = dev->of_node;

    sb->wakeup = of_property_read_bool(np, "slide-button,wakeup");

    sb->gpio = of_get_named_gpio_flags(np, "slide-button,gpios", 0, NULL);
    if (gpio_is_valid(sb->gpio))
        ret = gpio_request(sb->gpio, "slide_button-irq");

    if (of_property_read_u32(np, "debounce-interval",
                             &sb->debounce_interval)) {
        pr_err("%s: failed to read debounce-interval, set to 15\n", __func__);
        sb->debounce_interval = 15;
    }

    return ret;
}

static int slide_button_probe(struct platform_device *pdev) {
    int ret = -1;
    struct slide_button_desc *sb = NULL;

    pr_debug("%s: enter\n", __func__);
    sb = devm_kzalloc(&pdev->dev, sizeof(struct slide_button_desc), GFP_KERNEL);
    if (!sb) {
        pr_err("%s: failed to allocate device\n", __func__);
        return -ENOMEM;
    }
    dev_set_drvdata(&pdev->dev, sb);

    ret = slide_button_parse_dt(&pdev->dev, sb);
    if (ret < 0) {
        pr_err("%s: failed to parse dt\n", __func__);
        return -EIO;
    }

    sb->input = devm_input_allocate_device(&pdev->dev);
    if (!sb->input) {
        pr_err("%s: failed to allocate input device\n", __func__);
        return -ENOMEM;
    }

    sb->input->phys = SLIDE_BUTTON_NODE;
    sb->input->name = SLIDE_BUTTON_NAME;
    sb->input->id.bustype = BUS_HOST;

    set_bit(EV_KEY, sb->input->evbit);
    set_bit(KEY_SLIDE_BUTTON_OFF, sb->input->keybit);
    set_bit(KEY_SLIDE_BUTTON_ON, sb->input->keybit);

    ret = input_register_device(sb->input);
    if (ret) {
        pr_err("%s: failed to register input device\n", __func__);
        goto err_free_dev;
    }

    sb->irq = gpio_to_irq(sb->gpio);
    if (sb->irq < 0) {
        pr_err("%s: gpio_to_irq failed\n", __func__);
        ret = sb->irq;
        goto err_free_dev;
    }
    pr_debug("%s: irq: %d\n", __func__, sb->irq);

    ret = devm_request_threaded_irq(&pdev->dev, sb->irq, NULL,
                                    slide_button_irq_trigger_handler,
                                    IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
                                    "slide_button", sb);
    if (ret) {
        pr_err("%s: failed to request irq\n", __func__);
        goto err_free_irq;
    }

    device_init_wakeup(&pdev->dev, sb->wakeup);
    enable_irq_wake(sb->irq);
    return 0;

err_free_irq:
    disable_irq_wake(sb->irq);
    free_irq(sb->irq, slide_button_irq_trigger_handler);
err_free_dev:
    input_unregister_device(sb->input);
    return ret;
}

static int slide_button_remove(struct platform_device *pdev)
{
    struct slide_button_desc *sb = dev_get_drvdata(&pdev->dev);
    if (!sb || !sb->irq) return 0;
    disable_irq_wake(sb->irq);
    device_init_wakeup(&pdev->dev, false);
    gpio_free(sb->gpio);
    return 0;
}

static struct platform_driver slide_button_device_driver = {
    .probe	        = slide_button_probe,
    .remove	        = slide_button_remove,
	.driver	        = {
        .name           = "slide_button",
        .of_match_table = of_match_ptr(slide_button_of_match),
    }
};

static int __init slide_button_init(void)
{
    pr_debug("%s: enter\n", __func__);
    return platform_driver_register(&slide_button_device_driver);
}

static void __exit slide_button_exit(void)
{
    platform_driver_unregister(&slide_button_device_driver);
}

module_init(slide_button_init);
module_exit(slide_button_exit);

MODULE_LICENSE("GPL");
