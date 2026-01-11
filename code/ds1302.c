#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/device.h>

// ========= GPIO PIN =========
#define GPIO_CLK	17
#define GPIO_DAT	18
#define GPIO_RST	19

// ========= ADDRESS =========
#define ADDR_SECONDS		0x80
#define ADDR_MINUTES		0x82
#define ADDR_HOURS			0x84
#define ADDR_DATE			0x86
#define ADDR_MONTH			0x88
#define ADDR_DAYOFWEEK		0x8A
#define ADDR_YEAR			0x8C

#define DEVICE_NAME 	"ds1302"

#define BLINK_PERIOD_MS 1000

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Driver Developer");
MODULE_DESCRIPTION("ds1302 driver");

static dev_t device_number;
static struct cdev ds1302_cdev;
static struct class *ds1302_class = NULL;
static struct device *ds1302_device = NULL;
static struct timer_list ds1302_timer;

typedef struct {
	uint8_t seconds;
	uint8_t minutes;
	uint8_t hours;
	uint8_t date;
	uint8_t month;
	uint8_t dayofweek;
	uint8_t year;
}t_ds1302;

static t_ds1302 ds_time;

static unsigned char bcd2dec(unsigned char byte)
{
	uint8_t high, low;

	low = byte & 0x0F;
	high = (byte >> 4) * 10;

	return (high + low);
}

static unsigned char dec2bcd(unsigned char byte)
{
	unsigned char high, low;

	high = (byte / 10) << 4;
	low = byte % 10;

	return (high + low);
}

static void ds1302_DataLine_Input(void)
{
	gpio_direction_input(GPIO_DAT);
}

static void ds1302_DataLine_Output(void)
{
	gpio_direction_output(GPIO_DAT, 0);
}

static void ds1302_clock(void)
{
	gpio_set_value(GPIO_CLK, 1);
	udelay(2);
	gpio_set_value(GPIO_CLK, 0);
	udelay(2);
}

static void ds1302_tx(uint8_t tx)
{
	ds1302_DataLine_Output();

	for(int i = 0; i < 8; i++)
	{
		if(tx & (1 << i))
		{
			gpio_set_value(GPIO_DAT, 1);
		}
		else
		{
			gpio_set_value(GPIO_DAT, 0);
		}
		ds1302_clock();
	}
}

static void ds1302_rx(uint8_t *data8)
{
	uint8_t temp = 0;

	ds1302_DataLine_Input();

	for(int i = 0; i < 8; i++)
	{
		if(gpio_get_value(GPIO_DAT))
		{
			temp |= 1 << i;
		}
		if(i != 7)
		{
			ds1302_clock();
		}
	}
	*data8 = temp;
}

static void ds1302_write_byte(uint8_t addr, uint8_t data)
{
	gpio_set_value(GPIO_RST, 1);
	ds1302_tx(addr);
	ds1302_tx(dec2bcd(data));
	gpio_set_value(GPIO_RST, 0);
}

static uint8_t ds1302_read_byte(uint8_t addr)
{
	uint8_t data8bits = 0;

	gpio_set_value(GPIO_RST, 1);
	ds1302_tx(addr + 1);
	ds1302_rx(&data8bits);
	gpio_set_value(GPIO_RST, 0);

	return bcd2dec(data8bits);
}

static void ds1302_init_time_date(void)
{
	ds1302_write_byte(ADDR_SECONDS, ds_time.seconds);
	ds1302_write_byte(ADDR_MINUTES, ds_time.minutes);
	ds1302_write_byte(ADDR_HOURS, ds_time.hours);
	ds1302_write_byte(ADDR_DATE, ds_time.date);
	ds1302_write_byte(ADDR_MONTH, ds_time.month);
	ds1302_write_byte(ADDR_DAYOFWEEK, ds_time.dayofweek);
	ds1302_write_byte(ADDR_YEAR, ds_time.year);
}

static void ds1302_read_time(void)
{
	ds_time.seconds = ds1302_read_byte(ADDR_SECONDS);
	ds_time.minutes = ds1302_read_byte(ADDR_MINUTES);
	ds_time.hours = ds1302_read_byte(ADDR_HOURS);
}

static void ds1302_read_date(void)
{
	ds_time.date = ds1302_read_byte(ADDR_DATE);
	ds_time.month = ds1302_read_byte(ADDR_MONTH);
	ds_time.dayofweek = ds1302_read_byte(ADDR_DAYOFWEEK);
	ds_time.year = ds1302_read_byte(ADDR_YEAR);
}

static ssize_t ds1302_read(struct file *file, char __user *buf, size_t len, loff_t *offset)
{
	char time_str[64];
	int str_len;

	ds1302_read_time();
	ds1302_read_date();

	str_len = snprintf(time_str, sizeof(time_str), "%04d-%02d-%02d %02d:%02d:%02d\n",
					ds_time.year+2000,
					ds_time.month,
					ds_time.date,
					ds_time.hours,
					ds_time.minutes,
					ds_time.seconds);

	if(copy_to_user(buf, time_str, str_len))
	{
		return -EFAULT;
	}

	*offset += str_len;
	return str_len;
}

static ssize_t ds1302_write(struct file *file, const char __user *buf, size_t len, loff_t *offset)
{
	char cmd[32];

	if(len > sizeof(cmd) - 1)	return -EINVAL;
	if(copy_from_user(cmd, buf, len))	return -EFAULT;

	cmd[len] = '\0';

	if(len >= 12)
	{
		char temp[3];
		temp[2] = '\0';

		memcpy(temp, cmd, 2);
		ds_time.year = simple_strtoul(temp, NULL, 10);

		memcpy(temp, cmd+2, 2);
		ds_time.month = simple_strtoul(temp, NULL, 10);

		memcpy(temp, cmd+4, 2);
		ds_time.date = simple_strtoul(temp, NULL, 10);

		memcpy(temp, cmd+6, 2);
		ds_time.hours = simple_strtoul(temp, NULL, 10);

		memcpy(temp, cmd+8, 2);
		ds_time.minutes = simple_strtoul(temp, NULL, 10);

		memcpy(temp, cmd+10, 2);
		ds_time.seconds = simple_strtoul(temp, NULL, 10);

		ds_time.dayofweek = 0;

		printk(KERN_INFO "시간 설정");
		printk(KERN_INFO "DS1302 : %04d-%02d-%02d %02d:%02d:%02d\n",
                   ds_time.year+2000, ds_time.month, ds_time.date,
                   ds_time.hours, ds_time.minutes, ds_time.seconds);

		ds1302_init_time_date();

	}
	return len;
}

static void ds1302_timer_callback(struct timer_list *data)
{
	ds1302_read_time();
	ds1302_read_date();


	printk(KERN_INFO "DS1302: %04d-%02d-%02d %02d:%02d:%02d\n",
		   ds_time.year + 2000,
		   ds_time.month,
		   ds_time.date,
		   ds_time.hours,
		   ds_time.minutes,
		   ds_time.seconds);

	mod_timer(&ds1302_timer, jiffies + msecs_to_jiffies(BLINK_PERIOD_MS));
}

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.read = ds1302_read,
	.write = ds1302_write,
};

static int __init ds1302_init(void)
{
	int ret;
	printk(KERN_INFO "====== ds1302 initializeing ======\n");
  	ret = alloc_chrdev_region(&device_number, 0, 1, DEVICE_NAME);
  	if (ret == -1) 
	{
    	printk(KERN_ERR "ERROR: alloc_chardev_regin ........\n");
    	return ret;
	}

	cdev_init(&ds1302_cdev, &fops);
  	if ((ret = cdev_add(&ds1302_cdev, device_number, 1)) == -1)
	{
		printk(KERN_ERR "ERROR: cdev_add  ........\n");
    	unregister_chrdev_region(device_number, 1);
   		return ret;
	}

	ds1302_class = class_create(THIS_MODULE, DEVICE_NAME);
	if (IS_ERR(ds1302_class))
	{
    	cdev_del(&ds1302_cdev);
    	unregister_chrdev_region(device_number, 1);
    	return PTR_ERR(ds1302_class);
	}
	ds1302_device = device_create(ds1302_class, NULL, device_number, NULL, DEVICE_NAME);

	if (gpio_request(GPIO_CLK, "my_ds1302") ||
		gpio_request(GPIO_DAT, "my_ds1302") ||
		gpio_request(GPIO_RST, "my_ds1302")) {
	printk(KERN_ERR "ERROR: gpio_request  ........\n");
    return -1;
	}

	gpio_direction_output(GPIO_CLK, 0);
	gpio_direction_output(GPIO_DAT, 0);
	gpio_direction_output(GPIO_RST, 0);

	ds_time.year = 25;
    ds_time.month = 12;
    ds_time.date = 24;
    ds_time.dayofweek = 3;
    ds_time.hours = 14;
    ds_time.minutes = 30;
    ds_time.seconds = 0;

	ds1302_init_time_date();

	timer_setup(&ds1302_timer, ds1302_timer_callback, 0);
	mod_timer(&ds1302_timer, jiffies + msecs_to_jiffies(BLINK_PERIOD_MS));

	printk(KERN_INFO "ds1302 driver init success ........\n");
	return 0;
}

static void __exit ds1302_exit(void)
{
	del_timer(&ds1302_timer);

	gpio_free(GPIO_CLK);
	gpio_free(GPIO_DAT);
	gpio_free(GPIO_RST);

	device_destroy(ds1302_class, device_number);
	class_destroy(ds1302_class);
	cdev_del(&ds1302_cdev);
	unregister_chrdev_region(device_number, 1);

	printk(KERN_INFO "ds1302_driver_exit");
}

module_init(ds1302_init);
module_exit(ds1302_exit);
