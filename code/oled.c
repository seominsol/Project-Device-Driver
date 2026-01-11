#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include "font.h"

struct ssd1306_data {
    struct i2c_client *client;
    struct gpio_desc *reset_gpio;
    dev_t dev_num;
    struct cdev cdev;
    struct class *class;
};

/* --- I2C 하드웨어 제어 --- */

static int ssd1306_write_cmd(struct ssd1306_data *data, u8 cmd) {
    u8 buf[2] = {0x00, cmd};
    return i2c_master_send(data->client, (char *)buf, 2);
}

static int ssd1306_write_data(struct ssd1306_data *data, u8 val) {
    u8 buf[2] = {0x40, val};
    return i2c_master_send(data->client, (char *)buf, 2);
}

static void ssd1306_set_pos(struct ssd1306_data *data, u8 page, u8 col) {
    ssd1306_write_cmd(data, 0xB0 + page);
    ssd1306_write_cmd(data, (col & 0x0F));
    ssd1306_write_cmd(data, 0x10 | (col >> 4));
}

static void ssd1306_clear(struct ssd1306_data *data) {
    int p, c;
    for (p = 0; p < 8; p++) {
        ssd1306_set_pos(data, p, 0);
        for (c = 0; c < 128; c++) ssd1306_write_data(data, 0x00);
    }
}
static void ssd1306_write_string(struct ssd1306_data *data, const char *str, u8 page, u8 col) {
    ssd1306_set_pos(data, page, col);
    while (*str) {
        u8 c = (u8)*str++;
        int i;
        for (i = 0; i < 5; i++) {
            u8 bits = ssd1306_font[c][i];
            ssd1306_write_data(data, bits);
        }
        ssd1306_write_data(data, 0x00); // 글자 간 간격(1픽셀)
    }
}

/* --- 파일 오퍼레이션 --- */

static int oled_open(struct inode *inode, struct file *file) {
    file->private_data = container_of(inode->i_cdev, struct ssd1306_data, cdev);
    return 0;
}

static ssize_t oled_write(struct file *file, const char __user *buf, 
                          size_t count, loff_t *ppos) {
    struct ssd1306_data *data = file->private_data;
    char kbuf[256];
    size_t len = min(count, (size_t)255);

    if (copy_from_user(kbuf, buf, len)) return -EFAULT;
    kbuf[len] = '\0';

    // "CLEAR" 명령 처리
    if (strncmp(kbuf, "CLEAR", 5) == 0) {
        ssd1306_clear(data);
        return count;
    }

    // 특수 포맷: "DATE:2025-12-28\nTIME:14:30:25\nTEMP:25\nHUMI:60"
    if (strncmp(kbuf, "DATE:", 5) == 0) {
        char date[16] = {0};
        char time[16] = {0};
        char temp[16] = {0};
        char humi[16] = {0};
        
        char *ptr = kbuf;
        char *next_line;
        
        // DATE 파싱
        if ((next_line = strchr(ptr, '\n')) != NULL) {
            size_t date_len = min((size_t)(next_line - ptr - 5), (size_t)15);
            strncpy(date, ptr + 5, date_len);
            ptr = next_line + 1;
        }
        
        // TIME 파싱
        if (strncmp(ptr, "TIME:", 5) == 0 && (next_line = strchr(ptr, '\n')) != NULL) {
            size_t time_len = min((size_t)(next_line - ptr - 5), (size_t)15);
            strncpy(time, ptr + 5, time_len);
            ptr = next_line + 1;
        }
        
        // TEMP 파싱
        if (strncmp(ptr, "TEMP:", 5) == 0 && (next_line = strchr(ptr, '\n')) != NULL) {
            size_t temp_len = min((size_t)(next_line - ptr - 5), (size_t)15);
            strncpy(temp, ptr + 5, temp_len);
            ptr = next_line + 1;
        }
        
        // HUMI 파싱
        if (strncmp(ptr, "HUMI:", 5) == 0) {
            size_t humi_len = min(strlen(ptr + 5), (size_t)15);
            strncpy(humi, ptr + 5, humi_len);
        }
        
        
        // 오른쪽 상단 온습도
        if (temp[0]) {
            char temp_str[16];

            ssd1306_set_pos(data, 0, 70);
            for (int i = 0; i < 58; i++) ssd1306_write_data(data, 0x00);
            snprintf(temp_str, sizeof(temp_str), "T:%sC", temp);
            ssd1306_write_string(data, temp_str, 0, 70);
        }
        
        if (humi[0]) {
            char humi_str[16];

            ssd1306_set_pos(data, 1, 70);
            for (int i = 0; i < 58; i++) ssd1306_write_data(data, 0x00);
            snprintf(humi_str, sizeof(humi_str), "H:%s%%", humi);
            ssd1306_write_string(data, humi_str, 1, 70);
        }
        
        // 왼쪽 중앙 날짜/시간
        if (date[0]) {
            ssd1306_set_pos(data, 3, 0);
            for (int i = 0; i < 70; i++) ssd1306_write_data(data, 0x00);
            ssd1306_write_string(data, date, 3, 0);
        }
        
        if (time[0]) {
            ssd1306_set_pos(data, 4, 0);
            for (int i = 0; i < 70; i++) ssd1306_write_data(data, 0x00);
            ssd1306_write_string(data, time, 4, 0);
        }
        
        return count;
    }

    // 일반 텍스트: 줄바꿈 처리
    {
        char *ptr = kbuf;
        char *line;
        u8 page = 0;
        
        ssd1306_clear(data);
        
        while ((line = strsep(&ptr, "\n")) != NULL && page < 8) {
            if (*line != '\0') {
                ssd1306_write_string(data, line, page, 0);
                page++;
            }
        }
    }

    return count;
}
static struct file_operations oled_fops = {
    .owner = THIS_MODULE,
    .open = oled_open,
    .write = oled_write,
};

/* --- I2C Probe & Remove --- */

static int ssd1306_probe(struct i2c_client *client, const struct i2c_device_id *id) {
    struct ssd1306_data *data;
    struct device *dev = &client->dev;

    data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
    if (!data) return -ENOMEM;

    data->client = client;
    data->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
    i2c_set_clientdata(client, data);

    if (data->reset_gpio) {
        gpiod_set_value(data->reset_gpio, 0); msleep(50);
        gpiod_set_value(data->reset_gpio, 1); msleep(50);
    }

    ssd1306_write_cmd(data, 0xAE);
    ssd1306_write_cmd(data, 0x8D);
    ssd1306_write_cmd(data, 0x14);
    ssd1306_write_cmd(data, 0xA1);
    ssd1306_write_cmd(data, 0xC8);
    ssd1306_write_cmd(data, 0xAF);

    ssd1306_clear(data);
    ssd1306_set_pos(data, 3, 30);
    //ssd1306_write_string(data, "I2C OLED Ready");


    alloc_chrdev_region(&data->dev_num, 0, 1, "oled_dev");
    cdev_init(&data->cdev, &oled_fops);
    cdev_add(&data->cdev, data->dev_num, 1);

    data->class = class_create(THIS_MODULE, "oled_class");
    device_create(data->class, NULL, data->dev_num, NULL, "oled");

    dev_info(dev, "SSD1306 I2C OLED Ready: /dev/oled\n");
    return 0;
}

/* [핵심 수정] 반환형을 int -> void로 변경 */
static void ssd1306_remove(struct i2c_client *client) {
    struct ssd1306_data *data = i2c_get_clientdata(client);
    
    ssd1306_clear(data);
    ssd1306_write_cmd(data, 0xAE);
    
    device_destroy(data->class, data->dev_num);
    class_destroy(data->class);
    cdev_del(&data->cdev);
    unregister_chrdev_region(data->dev_num, 1);
    
    /* return 0; 삭제 */
}

static const struct of_device_id ssd1306_dt_ids[] = {
    { .compatible = "solomon,ssd1306" },
    { }
};
MODULE_DEVICE_TABLE(of, ssd1306_dt_ids);

static const struct i2c_device_id ssd1306_id[] = {
    { "ssd1306", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, ssd1306_id);

static struct i2c_driver ssd1306_driver = {
    .driver = {
        .name = "ssd1306_oled",
        .of_match_table = ssd1306_dt_ids,
    },
    .probe = ssd1306_probe,
    .remove = ssd1306_remove, /* 이제 void 타입을 반환하므로 호환됨 */
    .id_table = ssd1306_id,
};

module_i2c_driver(ssd1306_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("minsol");
