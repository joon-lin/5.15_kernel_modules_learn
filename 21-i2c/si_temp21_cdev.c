/*
 * Lesson 21：同一个 I2C 温度设备的手动 cdev 版本
 *
 * 与 si_temp21.c 的区别：
 *   - si_temp21.c 使用 misc_register()；
 *   - 本文件手动完成字符设备注册流程：
 *       alloc_chrdev_region()
 *       cdev_init()/cdev_add()
 *       class_create()
 *       device_create()
 *
 * 用户空间接口：
 *   /dev/temp_cdev
 *
 * 假设的设备协议仍然是：I2C 地址 0x44，寄存器 0x00 返回有符号 8 位温度。
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define SI_TEMP21_CDEV_TEMP_REG  0x00

struct si_temp21_cdev_data {
	struct i2c_client *client;
	struct mutex lock;

	/* 手动字符设备注册所需要的对象。 */
	dev_t devt;
	struct cdev cdev;
	struct class *class;
	struct device *device;

	char output[32];
};

static int si_temp21_cdev_read_temperature(
		struct si_temp21_cdev_data *data, int *temperature)
{
	int ret;

	ret = i2c_smbus_read_byte_data(data->client,
					       SI_TEMP21_CDEV_TEMP_REG);
	if (ret < 0)
		return ret;

	*temperature = (s8)ret;
	return 0;
}

/*
 * cdev 不会像 misc 那样自动把私有数据放入 file->private_data。
 *
 * open() 收到的 inode->i_cdev 就是我们自己的 cdev 成员，
 * 再通过 container_of() 找回整个 si_temp21_cdev_data，
 * 最后手动保存到 file->private_data。
 */
static int si_temp21_cdev_open(struct inode *inode, struct file *file)
{
	struct si_temp21_cdev_data *data;

	data = container_of(inode->i_cdev,
			   struct si_temp21_cdev_data, cdev);
	file->private_data = data;

	return 0;
}

static ssize_t si_temp21_cdev_read(struct file *file, char __user *buf,
					   size_t count, loff_t *ppos)
{
	struct si_temp21_cdev_data *data = file->private_data;
	int temperature;
	int length;
	int ret;

	if (*ppos != 0)
		return 0;

	mutex_lock(&data->lock);

	ret = si_temp21_cdev_read_temperature(data, &temperature);
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

static const struct file_operations si_temp21_cdev_fops = {
	.owner = THIS_MODULE,
	.open = si_temp21_cdev_open,
	.read = si_temp21_cdev_read,
	.llseek = no_llseek,
};

static int si_temp21_cdev_probe(struct i2c_client *client,
					const struct i2c_device_id *id)
{
	struct si_temp21_cdev_data *data;
	int ret;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_BYTE_DATA))
		return -EOPNOTSUPP;

	data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	mutex_init(&data->lock);
	i2c_set_clientdata(client, data);

	/* 1. 分配字符设备号。 */
	ret = alloc_chrdev_region(&data->devt, 0, 1, "si_temp21_cdev");
	if (ret)
		return ret;

	/* 2. 初始化并添加 cdev。 */
	cdev_init(&data->cdev, &si_temp21_cdev_fops);
	data->cdev.owner = THIS_MODULE;

	ret = cdev_add(&data->cdev, data->devt, 1);
	if (ret)
		goto err_unregister_chrdev;

	/* 3. 创建 /sys/class/si_temp21_cdev/。 */
	data->class = class_create(THIS_MODULE, "si_temp21_cdev");
	if (IS_ERR(data->class)) {
		ret = PTR_ERR(data->class);
		goto err_cdev_del;
	}

	/* 4. 创建设备对象，udev 会据此创建 /dev/temp_cdev。 */
	data->device = device_create(data->class, &client->dev,
				     data->devt, data, "temp_cdev");
	if (IS_ERR(data->device)) {
		ret = PTR_ERR(data->device);
		goto err_class_destroy;
	}

	dev_info(&client->dev,
		 "cdev temperature device at I2C address 0x%02x\n",
		 client->addr);
	return 0;

err_class_destroy:
	class_destroy(data->class);
err_cdev_del:
	cdev_del(&data->cdev);
err_unregister_chrdev:
	unregister_chrdev_region(data->devt, 1);
	return ret;
}

static int si_temp21_cdev_remove(struct i2c_client *client)
{
	struct si_temp21_cdev_data *data = i2c_get_clientdata(client);

	device_destroy(data->class, data->devt);
	class_destroy(data->class);
	cdev_del(&data->cdev);
	unregister_chrdev_region(data->devt, 1);

	return 0;
}

static const struct of_device_id si_temp21_cdev_of_match[] = {
	{ .compatible = "si,lesson21-temp-cdev" },
	{ }
};
MODULE_DEVICE_TABLE(of, si_temp21_cdev_of_match);

static const struct i2c_device_id si_temp21_cdev_id[] = {
	{ "si-temp21-cdev", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, si_temp21_cdev_id);

static struct i2c_driver si_temp21_cdev_driver = {
	.driver = {
		.name = "si-temp21-cdev",
		.of_match_table = si_temp21_cdev_of_match,
	},
	.probe = si_temp21_cdev_probe,
	.remove = si_temp21_cdev_remove,
	.id_table = si_temp21_cdev_id,
};

module_i2c_driver(si_temp21_cdev_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SI kernel API lesson");
MODULE_DESCRIPTION("Lesson 21: I2C driver with manually registered cdev");
