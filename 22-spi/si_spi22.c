/*
 * Lesson 22：一个最小的设备树 SPI 温度驱动模板
 *
 * 假想的 SPI 从设备协议：
 *   - SPI mode 0；
 *   - 读寄存器时，把寄存器地址最高位置 1；
 *   - 发送 1 字节命令后读取 1 字节数据；
 *   - 温度寄存器 0x00 返回有符号 8 位摄氏度值。
 *
 * 这只是教学假设，真实 SPI 芯片必须按照数据手册修改命令格式。
 *
 * SPI 和 I2C 的一个重要区别：
 *   - I2C 使用设备地址，例如 0x44；
 *   - SPI 通常没有设备地址，使用片选 CS 区分设备；
 *   - 设备树中的 reg = <0> 表示使用 CS0。
 *
 * 用户空间接口：
 *   /dev/spi_temp
 */

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>

#define SI_SPI22_TEMP_REG   0x00
#define SI_SPI22_READ_BIT   BIT(7)

struct si_spi22_data {
	struct spi_device *spi;
	struct miscdevice miscdev;
	struct mutex lock;
	char output[32];
};

/*
 * 读取一个 SPI 寄存器。
 *
 * spi_write_then_read() 会在一次 SPI 事务中：
 *   1. 拉低片选；
 *   2. 发送命令字节；
 *   3. 读取数据字节；
 *   4. 释放片选。
 */
static int si_spi22_read_temperature(struct si_spi22_data *data,
					     int *temperature)
{
	u8 command = SI_SPI22_TEMP_REG | SI_SPI22_READ_BIT;
	u8 value;
	int ret;

	ret = spi_write_then_read(data->spi, &command, sizeof(command),
					 &value, sizeof(value));
	if (ret)
		return ret;

	*temperature = (s8)value;
	return 0;
}

static ssize_t si_spi22_read(struct file *file, char __user *buf,
				     size_t count, loff_t *ppos)
{
	struct miscdevice *miscdev = file->private_data;
	struct si_spi22_data *data;
	int temperature;
	int length;
	int ret;

	data = container_of(miscdev, struct si_spi22_data, miscdev);

	if (*ppos != 0)
		return 0;

	mutex_lock(&data->lock);

	ret = si_spi22_read_temperature(data, &temperature);
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

static const struct file_operations si_spi22_fops = {
	.owner = THIS_MODULE,
	.read = si_spi22_read,
	.llseek = no_llseek,
};

static int si_spi22_probe(struct spi_device *spi)
{
	struct si_spi22_data *data;
	int ret;

	/* 没有 spi-cpol/spi-cpha 时，SPI 默认就是 mode 0。 */
	if (!spi->max_speed_hz)
		spi->max_speed_hz = 1000000;

	ret = spi_setup(spi);
	if (ret) {
		dev_err(&spi->dev, "spi_setup failed: %d\n", ret);
		return ret;
	}

	data = devm_kzalloc(&spi->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->spi = spi;
	mutex_init(&data->lock);

	data->miscdev.minor = MISC_DYNAMIC_MINOR;
	data->miscdev.name = "spi_temp";
	data->miscdev.fops = &si_spi22_fops;
	data->miscdev.mode = 0444;

	/* SPI 设备的私有数据通过 spi_set_drvdata() 保存。 */
	spi_set_drvdata(spi, data);

	ret = misc_register(&data->miscdev);
	if (ret) {
		dev_err(&spi->dev,
			"failed to register /dev/spi_temp: %d\n", ret);
		return ret;
	}

	dev_info(&spi->dev,
		 "lesson22 SPI device: cs=%u speed=%uHz mode=%u\n",
		 (unsigned int)spi->chip_select,
		 (unsigned int)spi->max_speed_hz,
		 (unsigned int)spi->mode);

	return 0;
}

static int si_spi22_remove(struct spi_device *spi)
{
	struct si_spi22_data *data = spi_get_drvdata(spi);

	misc_deregister(&data->miscdev);

	return 0;
}

static const struct of_device_id si_spi22_of_match[] = {
	{ .compatible = "si,lesson22-spi-temp" },
	{ }
};
MODULE_DEVICE_TABLE(of, si_spi22_of_match);

static const struct spi_device_id si_spi22_id[] = {
	{ "si-spi22-temp", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, si_spi22_id);

static struct spi_driver si_spi22_driver = {
	.driver = {
		.name = "si-spi22-temp",
		.of_match_table = si_spi22_of_match,
	},
	.probe = si_spi22_probe,
	.remove = si_spi22_remove,
	.id_table = si_spi22_id,
};

module_spi_driver(si_spi22_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SI kernel API lesson");
MODULE_DESCRIPTION("Lesson 22: device-tree matched SPI temperature driver");
