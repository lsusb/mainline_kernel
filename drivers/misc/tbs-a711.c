#include <linux/wait.h>
#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>
#include <linux/slab.h>

#define DRIVER_NAME "tbs_a711"

#define A711_IOCTL_RESET _IO('A', 0)
#define A711_IOCTL_POWERUP _IO('A', 1)
#define A711_IOCTL_POWERDN _IO('A', 2)
#define A711_IOCTL_STATUS _IOR('A', 3, int)

enum {
	A711_REQ_NONE = 0,
	A711_REQ_RESET,
	A711_REQ_PWDN,
	A711_REQ_PWUP,
};

struct a711_dev {
	struct device *dev;

	struct gpio_desc *enable_gpio;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *wakeup_gpio;
	struct regulator *regulator;
	int wakeup_irq;
	u32 reset_duration;
	struct cdev cdev;
	dev_t major;

	// change
	spinlock_t lock;
	wait_queue_head_t waitqueue;
	int got_wakeup;
	int is_open;
	struct work_struct work;
	int last_request;
	int is_enabled;
};

static void a711_reset(struct a711_dev* a711)
{
	struct device *dev = a711->dev;

	if (!a711->is_enabled)
		return;

	if (!a711->reset_gpio) {
		dev_err(dev, "reset is not configured for this device");
		return;
	}

	dev_info(dev, "resetting");

	gpiod_set_value(a711->reset_gpio, 1);
	msleep(a711->reset_duration);
	gpiod_set_value(a711->reset_gpio, 0);
}

static void a711_power_down(struct a711_dev* a711)
{
	struct device *dev = a711->dev;

	if (!a711->is_enabled)
		return;

	dev_info(dev, "powering down");

	gpiod_set_value(a711->enable_gpio, 0);
	if (a711->regulator)
		regulator_disable(a711->regulator);
	else
		gpiod_set_value(a711->reset_gpio, 1);
	a711->is_enabled = 0;
}

static void a711_power_up(struct a711_dev* a711)
{
	struct device *dev = a711->dev;
	int ret;

	if (a711->is_enabled)
		return;

	dev_info(dev, "powering up");

	// power up
	if (a711->regulator) {
		ret = regulator_enable(a711->regulator);
		if (ret < 0) {
			dev_err(dev, "can't enable power supply err=%d", ret);
			return;
		}
	}

	gpiod_set_value(a711->enable_gpio, 1);
	gpiod_set_value(a711->reset_gpio, 1);
	msleep(a711->reset_duration);
	gpiod_set_value(a711->reset_gpio, 0);
	a711->is_enabled = 1;
}

static struct class* a711_class;

static void a711_work_handler(struct work_struct *work)
{
	struct a711_dev *a711 = container_of(work, struct a711_dev, work);
	int last_request;

	spin_lock(&a711->lock);
	last_request = a711->last_request;
	a711->last_request = 0;
	spin_unlock(&a711->lock);

	if (last_request == A711_REQ_RESET) {
		a711_reset(a711);
	} else if (last_request == A711_REQ_PWDN) {
		a711_power_down(a711);
	} else if (last_request == A711_REQ_PWUP) {
		a711_power_up(a711);
	}
}

static bool a711_has_wakeup(struct a711_dev* a711)
{
	bool got_wakeup;
	spin_lock(&a711->lock);
	got_wakeup = a711->got_wakeup;
	spin_unlock(&a711->lock);
	return got_wakeup;
}

static ssize_t a711_read(struct file *fp, char __user *buf, size_t len,
			 loff_t *off)
{
	struct a711_dev* a711 = fp->private_data;
	int ret;
	char tmp_buf[1] = {1};
	int got_wakeup;
	int non_blocking = fp->f_flags & O_NONBLOCK;

	// first handle non-blocking path
	if (non_blocking && !a711_has_wakeup(a711))
		return -EWOULDBLOCK;

	// wait for availability of wakeup
	ret = wait_event_interruptible(a711->waitqueue,
				       a711_has_wakeup(a711));
	if (ret)
		return ret;

	spin_lock(&a711->lock);

	got_wakeup = a711->got_wakeup;
	a711->got_wakeup = 0;

	if (!got_wakeup) {
		ret = -EIO;
	} else {
		if (copy_to_user(buf, tmp_buf, 1))
			ret = -EFAULT;
		else
			ret = 1;
	}

	spin_unlock(&a711->lock);
	return ret;
}

static ssize_t a711_write(struct file *fp, const char __user *buf,
			 size_t len, loff_t *off)
{
	struct a711_dev* a711 = fp->private_data;
	int ret;
	char tmp_buf[1];
	int update = 0;

	if (len == 0)
		return 0;

	ret = copy_from_user(tmp_buf, buf, 1);
	if (ret)
		return -EFAULT;

	spin_lock(&a711->lock);

	switch (tmp_buf[0]) {
	case 'r':
		a711->last_request = A711_REQ_RESET;
		break;
	case 'u':
		a711->last_request = A711_REQ_PWUP;
		break;
	case 'd':
		a711->last_request = A711_REQ_PWDN;
		break;
	default:
		a711->last_request = A711_REQ_NONE;
		break;
	}

	update = a711->last_request != 0;

	spin_unlock(&a711->lock);

	if (update)
		schedule_work(&a711->work);

	return 1;
}

static unsigned int a711_poll(struct file *fp, poll_table *wait)
{
	struct a711_dev* a711 = fp->private_data;
	int ret = 0;

	poll_wait(fp, &a711->waitqueue, wait);

	spin_lock(&a711->lock);
	if (a711->got_wakeup)
		ret |= POLLIN | POLLRDNORM;
	spin_unlock(&a711->lock);

	return ret;
}

static long a711_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	struct a711_dev* a711 = fp->private_data;
	unsigned long flags;
	int ret = -ENOSYS;
	void __user *parg = (void __user *)arg;
	int powered;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	spin_lock_irqsave(&a711->lock, flags);

	switch (cmd) {
	case A711_IOCTL_RESET:
		a711->last_request = A711_REQ_RESET;
		ret = 0;
		break;
	case A711_IOCTL_POWERUP:
		a711->last_request = A711_REQ_PWUP;
		ret = 0;
		break;
	case A711_IOCTL_POWERDN:
		a711->last_request = A711_REQ_PWDN;
		ret = 0;
		break;
	case A711_IOCTL_STATUS:
		powered = a711->is_enabled || a711->last_request == A711_REQ_PWUP;
		spin_unlock_irqrestore(&a711->lock, flags);

		if (copy_to_user(parg, &powered, sizeof powered))
			return -EFAULT;

		return 0;
	}

	spin_unlock_irqrestore(&a711->lock, flags);

	if (ret == 0)
		schedule_work(&a711->work);

	return ret;
}

static int a711_release(struct inode *ip, struct file *fp)
{
	struct a711_dev* a711 = fp->private_data;

	spin_lock(&a711->lock);
	a711->is_open = 0;
	spin_unlock(&a711->lock);

	return 0;
}

static int a711_open(struct inode *ip, struct file *fp)
{
	struct a711_dev* a711 = container_of(ip->i_cdev, struct a711_dev, cdev);

	fp->private_data = a711;

	spin_lock(&a711->lock);
	if (a711->is_open) {
		spin_unlock(&a711->lock);
		return -EBUSY;
	}

	a711->is_open = 1;
	spin_unlock(&a711->lock);

	nonseekable_open(ip, fp);
	return 0;
}

static const struct file_operations a711_fops = {
	.owner		= THIS_MODULE,
	.read		= a711_read,
	.write		= a711_write,
	.poll		= a711_poll,
	.unlocked_ioctl	= a711_ioctl,
	.open		= a711_open,
	.release	= a711_release,
	.llseek		= noop_llseek,
};

static irqreturn_t a711_wakeup_isr(int irq, void *dev_id)
{
	struct a711_dev *a711 = dev_id;
	unsigned long flags;

	spin_lock_irqsave(&a711->lock, flags);
	a711->got_wakeup = 1;
	spin_unlock_irqrestore(&a711->lock, flags);

	wake_up_interruptible(&a711->waitqueue);

	return IRQ_HANDLED;
}

static int a711_probe(struct platform_device *pdev)
{
	struct a711_dev *a711;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device *sdev;
	const char* cdev_name = NULL;
	int ret;

	a711 = devm_kzalloc(dev, sizeof(*a711), GFP_KERNEL);
	if (!a711)
		return -ENOMEM;

	a711->dev = dev;
	platform_set_drvdata(pdev, a711);
	init_waitqueue_head(&a711->waitqueue);
	spin_lock_init(&a711->lock);
	INIT_WORK(&a711->work, &a711_work_handler);

	// get device name and reset time from device tree
	ret = of_property_read_u32_index(np, "reset-duration-ms", 0,
					 &a711->reset_duration);
	if (ret) {
		a711->reset_duration = 10;
	}

	ret = of_property_read_string(np, "char-device-name", &cdev_name);
	if (ret) {
		dev_err(dev, "char-device-name is not configured");
		return -EINVAL;
	}

	a711->enable_gpio = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_HIGH);
	if (IS_ERR(a711->enable_gpio)) {
		dev_err(dev, "can't get enable gpio err=%ld",
			PTR_ERR(a711->enable_gpio));
		return PTR_ERR(a711->enable_gpio);
	}

	a711->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(a711->reset_gpio)) {
		dev_err(dev, "can't get reset gpio err=%ld",
			PTR_ERR(a711->reset_gpio));
		return PTR_ERR(a711->reset_gpio);
	}

	a711->wakeup_gpio = devm_gpiod_get_optional(dev, "wakeup", GPIOD_IN);
	if (IS_ERR(a711->wakeup_gpio)) {
		dev_err(dev, "can't get wakeup gpio err=%ld",
			PTR_ERR(a711->wakeup_gpio));
		return PTR_ERR(a711->wakeup_gpio);
	}

	a711->wakeup_irq = gpiod_to_irq(a711->wakeup_gpio);
	if (a711->wakeup_irq > 0) {
		ret = devm_request_irq(dev, a711->wakeup_irq,
				       a711_wakeup_isr,
				       IRQF_TRIGGER_RISING |
				       IRQF_TRIGGER_FALLING,
				       "a711-wakeup", a711);
		if (ret) {
			dev_err(dev, "error requesting wakeup-irq: %d", ret);
			return ret;
		}
	}

	a711->regulator = devm_regulator_get_optional(dev, "power");
	if (IS_ERR(a711->regulator)) {
		ret = PTR_ERR(a711->regulator);
                if (ret != -ENODEV) {
			dev_err(dev, "can't get power supply err=%d", ret);
			return ret;
		}

		a711->regulator = NULL;
	}

	// create char device
	ret = alloc_chrdev_region(&a711->major, 0, 1, "a711");
	if (ret) {
		dev_err(dev, "can't allocate chrdev region");
		goto err_disable_regulator;
	}

	cdev_init(&a711->cdev, &a711_fops);
	a711->cdev.owner = THIS_MODULE;
	ret = cdev_add(&a711->cdev, a711->major, 1);
	if (ret) {
		dev_err(dev, "can't add cdev");
		goto err_unreg_chrev_region;
	}

	sdev = device_create(a711_class, dev, a711->major, a711, cdev_name);
	if (IS_ERR(sdev)) {
		ret = PTR_ERR(sdev);
		goto err_del_cdev;
	}

	gpiod_set_value(a711->enable_gpio, 0);
	if (!a711->regulator)
		gpiod_set_value(a711->reset_gpio, 1);

	dev_info(dev, "initialized TBS A711 platform driver");

	return 0;

err_del_cdev:
	cdev_del(&a711->cdev);
err_unreg_chrev_region:
	unregister_chrdev(a711->major, "a711");
err_disable_regulator:
	cancel_work_sync(&a711->work);
	return ret;
}

static int a711_remove(struct platform_device *pdev)
{
	struct a711_dev *a711 = platform_get_drvdata(pdev);

	a711_power_down(a711);

	cancel_work_sync(&a711->work);

	device_destroy(a711_class, a711->major);
	cdev_del(&a711->cdev);
	unregister_chrdev(a711->major, "a711");

	if (a711->wakeup_irq > 0)
		devm_free_irq(a711->dev, a711->wakeup_irq, a711);

	return 0;
}

static const struct of_device_id a711_of_match[] = {
	{ .compatible = "custom,power-manager" },
	{},
};
MODULE_DEVICE_TABLE(of, a711_of_match);

static struct platform_driver a711_platform_driver = {
	.probe = a711_probe,
	.remove = a711_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = a711_of_match,
	},
};

static int __init a711_driver_init(void)
{
	int ret;

	a711_class = class_create(THIS_MODULE, "a711");

	ret = platform_driver_register(&a711_platform_driver);
	if (ret)
		class_destroy(a711_class);

	return ret;
}

static void __exit a711_driver_exit(void)
{
	platform_driver_unregister(&a711_platform_driver);
	class_destroy(a711_class);
}

module_init(a711_driver_init);
module_exit(a711_driver_exit);

MODULE_VERSION("1.0.0");
MODULE_DESCRIPTION("TBS A711 Tablet Platform Driver");
MODULE_AUTHOR("Ondrej Jirman <megous@megous.com>");
MODULE_LICENSE("GPL v2");
