#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/jiffies.h>
#include <linux/delay.h> 

#define DEVICE_NAME		"rotary"

#define S1_GPIO		23
#define S2_GPIO		24
#define SW_GPIO		25

#define DEBOUNCE_MS		20
#define EVENT_BUF_SIZE	10

static dev_t device_number;
static struct cdev rotary_cdev;
static struct class *rotary_class;

static int interrupt_num_s1;
static int interrupt_num_sw;

static unsigned long last_interrupt_time_s1 = 0;
static unsigned long last_interrupt_time_sw = 0;

static char event_buffer[EVENT_BUF_SIZE][16];
static int event_head = 0;
static int event_tail = 0;

static int data_ready = 0;
static DECLARE_WAIT_QUEUE_HEAD(rotary_wait_queue);

static void add_event(const char *event)
{
	int next = (event_head + 1) % EVENT_BUF_SIZE;

	if(next == event_tail)
	{
		printk(KERN_WARNING "Buffer Full\n");
		return;
	}

	strncpy(event_buffer[event_head], event, 15);
	event_buffer[event_head][15] = '\0';
	event_head = next;

	data_ready = 1;
	wake_up_interruptible(&rotary_wait_queue);
}

static irqreturn_t rotary_handler(int irq, void *dev_id)
{
		unsigned long current_time = jiffies;
		unsigned long debounce_jiffies = msecs_to_jiffies(DEBOUNCE_MS);

		if(time_before(current_time, last_interrupt_time_s1 + debounce_jiffies))
		{
			return IRQ_HANDLED;
		}
		last_interrupt_time_s1 = current_time;

		udelay(500);

		int val_s1 = gpio_get_value(S1_GPIO);
		int val_s2 = gpio_get_value(S2_GPIO);

		if(val_s1 == 0)
		{
			if(val_s2 == 1)
			{
				add_event("CW\n");
				printk(KERN_INFO "Rotary : CW\n");
			}
			else
			{
				add_event("CCW\n");
				printk(KERN_INFO "Rotary : CCW\n");
			}
		}
		
		return IRQ_HANDLED;
}

static irqreturn_t button_handler(int irq, void *dev_id)
{
	unsigned long current_time = jiffies;
	unsigned long debounce_jiffies = msecs_to_jiffies(50);

	if(time_before(current_time, last_interrupt_time_sw + debounce_jiffies))
	{
		return IRQ_HANDLED;
	}
	last_interrupt_time_sw = current_time;

	add_event("CLICK\n");
	printk(KERN_INFO "Rotary : Click\n");

	return IRQ_HANDLED;
}

static ssize_t rotary_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	int len;

	if(event_head == event_tail)
	{
		if(file->f_flags & O_NONBLOCK)
		{
			return -EAGAIN;
		}

		wait_event_interruptible(rotary_wait_queue, data_ready != 0);
		data_ready = 0;
	}
	
	len = strlen(event_buffer[event_tail]);

	if(copy_to_user(buf, event_buffer[event_tail], len))
	{
		printk(KERN_ERR "ERROR : copy_to_user\n");
		return -EFAULT;
	}

	event_tail = (event_tail + 1) % EVENT_BUF_SIZE;

	return len;
}

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.read = rotary_read
};

static int __init rotary_init(void)
{
	int ret;
	printk(KERN_INFO "====== rotary initializeing ======\n");
    ret = alloc_chrdev_region(&device_number, 0, 1, DEVICE_NAME);
	if (ret == -1) 
	{
		printk(KERN_ERR "ERROR: alloc_chardev_regin ........\n");
    	return ret;
	}

	cdev_init(&rotary_cdev, &fops);
	if ((ret = cdev_add(&rotary_cdev, device_number, 1)) == -1)
	{
		printk(KERN_ERR "ERROR: cdev_add  ........\n");
    	unregister_chrdev_region(device_number, 1);
    	return ret;
	}

	rotary_class = class_create(THIS_MODULE, DEVICE_NAME);
	if (IS_ERR(rotary_class))
	{
		cdev_del(&rotary_cdev);
    	unregister_chrdev_region(device_number, 1);
    	return PTR_ERR(rotary_class);
	}
	device_create(rotary_class, NULL, device_number, NULL, DEVICE_NAME);

	if (gpio_request(S1_GPIO, "my_rotary") ||
		gpio_request(S2_GPIO, "my_rotary") ||
		gpio_request(SW_GPIO, "my_rotary")) {
		printk(KERN_ERR "ERROR: gpio_request  ........\n");
		return -1;
	}

	gpio_direction_input(S1_GPIO);
  	gpio_direction_input(S2_GPIO);
	gpio_direction_input(SW_GPIO);

	interrupt_num_s1 = gpio_to_irq(S1_GPIO);
	ret = request_irq(interrupt_num_s1, rotary_handler, IRQF_TRIGGER_FALLING,
					"my_rotary_irq_S1", NULL);
	if (ret)
	{
    	printk(KERN_ERR "ERROR: request_irq_s1  ........\n");
    	return -ret;
	}

	interrupt_num_sw = gpio_to_irq(SW_GPIO);
	ret = request_irq(interrupt_num_sw, button_handler, IRQF_TRIGGER_FALLING,
					"my_rotary_irq_sw", NULL);
	if(ret)
	{
		printk(KERN_ERR "ERROR : request_irq_sw .........\n");
		return -ret;
	}
	printk(KERN_INFO "rotary driver init success ........\n");
  	return 0;
}

static void __exit rotary_exit(void) 
{
  free_irq(interrupt_num_s1, NULL);
  free_irq(interrupt_num_sw, NULL);

  gpio_free(S1_GPIO);
  gpio_free(S2_GPIO);
  gpio_free(SW_GPIO);

  device_destroy(rotary_class, device_number);
  class_destroy(rotary_class);
  cdev_del(&rotary_cdev);
  unregister_chrdev_region(device_number, 1);

  printk(KERN_INFO "rotary_driver_exit");
}

module_init(rotary_init);
module_exit(rotary_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("LHJ");
MODULE_DESCRIPTION("rotary driver");
