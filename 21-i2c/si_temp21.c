/*
 * Lesson 21: 一个最小的设备树 I2C 温度驱动模板
 *
 * 假想的从设备：
 *   I2C 地址：0x44
 *   温度寄存器：0x00
 *   寄存器值：有符号 8 位摄氏度，例如 25 表示 25°C
 *
 * 这个寄存器协议只是教学假设。真实芯片必须按照数据手册修改
 * si_temp21_read_temperature()，例如 SHT31 就不是这种寄存器读法。
 *
 * 用户空间接口：
 *   /dev/temp
 *   cat /dev/temp
 *
 * 数据流：
 *   cat /dev/temp
 *       -> misc 字符设备的 read()
 *       -> i2c_smbus_read_byte_data()
 *       -> I2C 从设备地址 0x44、寄存器 0x00
 */

#include <linux/i2c.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define SI_TEMP21_I2C_ADDR       0x44
#define SI_TEMP21_TEMP_REG       0x00
#define SI_TEMP21_DEVICE_NAME    "temp"

struct si_temp21_data {
	struct i2c_client *client;
	struct miscdevice miscdev;
	struct mutex lock;
	char output[32];
};

/*
 * 读取假想温度寄存器。
 *
 * i2c_smbus_read_byte_data() 的含义是：
 *   发送寄存器地址，然后读取一个字节。
 */
static int si_temp21_read_temperature(struct si_temp21_data *data,
					      int *temperature)
{
	int ret;

	ret = i2c_smbus_read_byte_data(data->client, SI_TEMP21_TEMP_REG);
	if (ret < 0)
		return ret;

	/* 这个教学设备假设温度是有符号 8 位整数。 */
	*temperature = (s8)ret;
	return 0;
}

/*
 * cat /dev/temp 最终会调用这里。
 * misc 子系统在 open() 时会把 file->private_data 设置为 miscdevice，
 * 因此通过 container_of() 可以找回我们的 si_temp21_data。
 */
static ssize_t si_temp21_read(struct file *file, char __user *buf,
				      size_t count, loff_t *ppos)
{
	struct miscdevice *miscdev = file->private_data;
	struct si_temp21_data *data;
	int temperature;
	int length;
	int ret;

	data = container_of(miscdev, struct si_temp21_data, miscdev);

	/* 让 cat 只读取一次；第二次 read() 返回 EOF。 */
	if (*ppos != 0)
		return 0;

	mutex_lock(&data->lock);

	ret = si_temp21_read_temperature(data, &temperature);
	if (ret < 0)
		goto out_unlock;

	length = scnprintf(data->output, sizeof(data->output),
				   "%d\n", temperature);
	ret = simple_read_from_buffer(buf, count, ppos,
					      data->output, length);

out_unlock:
	mutex_unlock(&data->lock);
	return ret;
}

static const struct file_operations si_temp21_fops = {
	.owner = THIS_MODULE,
	.read = si_temp21_read,
	.llseek = no_llseek,
};

static int si_temp21_probe(struct i2c_client *client,
				   const struct i2c_device_id *id)
{
	struct si_temp21_data *data;
	int ret;

	/* 本驱动使用 SMBus 的“写寄存器地址、读一个字节”操作。 */
	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_BYTE_DATA)) {
		dev_err(&client->dev,
			"adapter does not support SMBus byte-data transfers\n");
		return -EOPNOTSUPP;
	}

	/* devm_ 内存由设备模型自动释放，remove() 中不需要 kfree。 */
	data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	mutex_init(&data->lock);

	data->miscdev.minor = MISC_DYNAMIC_MINOR;
	data->miscdev.name = SI_TEMP21_DEVICE_NAME;
	data->miscdev.fops = &si_temp21_fops;
	data->miscdev.mode = 0444;

	/* 让其他驱动回调能够通过 client 找到私有数据。 */
	i2c_set_clientdata(client, data);

	ret = misc_register(&data->miscdev);
	if (ret) {
		dev_err(&client->dev,
			"failed to register /dev/%s: %d\n",
			SI_TEMP21_DEVICE_NAME, ret);
		return ret;
	}

	dev_info(&client->dev,
		 "lesson21 temperature device at I2C address 0x%02x\n",
		 client->addr);
	return 0;
}

static int si_temp21_remove(struct i2c_client *client)
{
	struct si_temp21_data *data = i2c_get_clientdata(client);

	misc_deregister(&data->miscdev);
	return 0;
}

/* 设备树匹配：compatible 必须和 DTS 节点一致。 */
static const struct of_device_id si_temp21_of_match[] = {
	{ .compatible = "si,lesson21-temp" },
	{ }
};
MODULE_DEVICE_TABLE(of, si_temp21_of_match);

/* 传统非设备树匹配表；设备树平台运行时主要使用上面的 of_match。 */
static const struct i2c_device_id si_temp21_id[] = {
	{ "si-temp21", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, si_temp21_id);

static struct i2c_driver si_temp21_driver = {
	.driver = {
		.name = "si-temp21",
		.of_match_table = si_temp21_of_match,
	},
	.probe = si_temp21_probe,
	.remove = si_temp21_remove,
	.id_table = si_temp21_id,
};

module_i2c_driver(si_temp21_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SI kernel API lesson");
MODULE_DESCRIPTION("Lesson 21: device-tree matched I2C temperature driver");
