/*
 * Copyright (c) 2011, 2012 Synaptics Incorporated
 * Copyright (c) 2011 Unixphere
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#define COMMS_DEBUG 0

#define IRQ_DEBUG 0

#if COMMS_DEBUG || IRQ_DEBUG
#define DEBUG
#endif

#include <linux/kernel.h>
#include <linux/lockdep.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/pm.h>
#include <linux/gpio.h>
#include <linux/rmi.h>
#include "rmi_driver.h"

#include <linux/err.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <hsad/config_interface.h>
#include "../touch_info.h"

#define RMI_PAGE_SELECT_REGISTER 0xff
#define RMI_I2C_PAGE(addr) (((addr) >> 8) & 0xff)

static char *phys_proto_name = "i2c";

struct rmi_i2c_data {
	struct mutex page_mutex;
	int page;
	int enabled;
	int irq;
	int irq_flags;
	struct rmi_phys_device *phys;
};

static irqreturn_t rmi_i2c_irq_thread(int irq, void *p)
{
	struct rmi_phys_device *phys = p;
	struct rmi_device *rmi_dev = phys->rmi_dev;
	struct rmi_driver *driver = rmi_dev->driver;
	struct rmi_device_platform_data *pdata = phys->dev->platform_data;

#if IRQ_DEBUG
	dev_dbg(phys->dev, "ATTN gpio, value: %d.\n",
			gpio_get_value(pdata->attn_gpio));
#endif
	if (gpio_get_value(pdata->attn_gpio) == pdata->attn_polarity) {
		phys->info.attn_count++;
		if (driver && driver->irq_handler && rmi_dev)
			driver->irq_handler(rmi_dev, irq);
	}

	return IRQ_HANDLED;
}

/*
 * rmi_set_page - Set RMI page
 * @phys: The pointer to the rmi_phys_device struct
 * @page: The new page address.
 *
 * RMI devices have 16-bit addressing, but some of the physical
 * implementations (like SMBus) only have 8-bit addressing. So RMI implements
 * a page address at 0xff of every page so we can reliable page addresses
 * every 256 registers.
 *
 * The page_mutex lock must be held when this function is entered.
 *
 * Returns zero on success, non-zero on failure.
 */
static int rmi_set_page(struct rmi_phys_device *phys, unsigned int page)
{
	struct i2c_client *client = to_i2c_client(phys->dev);
	struct rmi_i2c_data *data = phys->data;
	char txbuf[2] = {RMI_PAGE_SELECT_REGISTER, page};
	int retval;

#if COMMS_DEBUG
	dev_dbg(&client->dev, "RMI4 I2C writes 3 bytes: %02x %02x\n",
		txbuf[0], txbuf[1]);
#endif
	phys->info.tx_count++;
	phys->info.tx_bytes += sizeof(txbuf);
	retval = i2c_master_send(client, txbuf, sizeof(txbuf));
	if (retval != sizeof(txbuf)) {
		phys->info.tx_errs++;
		dev_err(&client->dev,
			"%s: set page failed: %d.", __func__, retval);
		return (retval < 0) ? retval : -EIO;
	}
	data->page = page;
	return 0;
}

static int rmi_i2c_write_block(struct rmi_phys_device *phys, u16 addr, u8 *buf,
			       int len)
{
	struct i2c_client *client = to_i2c_client(phys->dev);
	struct rmi_i2c_data *data = phys->data;
	u8 txbuf[len + 1];
	int retval;
#if	COMMS_DEBUG
	char debug_buf[len*3 + 1];
	int i, n;
#endif

	txbuf[0] = addr & 0xff;
	memcpy(txbuf + 1, buf, len);

	mutex_lock(&data->page_mutex);

	if (RMI_I2C_PAGE(addr) != data->page) {
		retval = rmi_set_page(phys, RMI_I2C_PAGE(addr));
		if (retval < 0)
			goto exit;
	}

#if COMMS_DEBUG
	n = 0;
	for (i=0; i < len; i++)
		n = snprintf(debug_buf+n, 4, "%02x ", buf[i]);
	dev_dbg(&client->dev, "RMI4 I2C writes %d bytes at %#06x: %s\n",
		len, addr, debug_buf);
#endif

	phys->info.tx_count++;
	phys->info.tx_bytes += sizeof(txbuf);
	retval = i2c_master_send(client, txbuf, sizeof(txbuf));
	if (retval < 0)
		phys->info.tx_errs++;
	else
		retval--; /* don't count the address byte */

exit:
	mutex_unlock(&data->page_mutex);
	return retval;
}

static int rmi_i2c_write(struct rmi_phys_device *phys, u16 addr, u8 data)
{
	int retval = rmi_i2c_write_block(phys, addr, &data, 1);
	return (retval < 0) ? retval : 0;
}

static int rmi_i2c_read_block(struct rmi_phys_device *phys, u16 addr, u8 *buf,
			      int len)
{
	struct i2c_client *client = to_i2c_client(phys->dev);
	struct rmi_i2c_data *data = phys->data;
	u8 txbuf[1] = {addr & 0xff};
	int retval;
#if	COMMS_DEBUG
	char debug_buf[len*3 + 1];
	char *temp = debug_buf;
	int i, n;
#endif

	mutex_lock(&data->page_mutex);

	if (RMI_I2C_PAGE(addr) != data->page) {
		retval = rmi_set_page(phys, RMI_I2C_PAGE(addr));
		if (retval < 0)
			goto exit;
	}

#if COMMS_DEBUG
	dev_dbg(&client->dev, "RMI4 I2C writes 1 bytes: %02x\n", txbuf[0]);
#endif
	phys->info.tx_count++;
	phys->info.tx_bytes += sizeof(txbuf);
	retval = i2c_master_send(client, txbuf, sizeof(txbuf));
	if (retval != sizeof(txbuf)) {
		phys->info.tx_errs++;
		retval = (retval < 0) ? retval : -EIO;
		goto exit;
	}

	retval = i2c_master_recv(client, buf, len);

	phys->info.rx_count++;
	phys->info.rx_bytes += len;
	if (retval < 0)
		phys->info.rx_errs++;
#if COMMS_DEBUG
	else {
		n = 0;
		for (i=0; i < len; i++) {
			n = sprintf(temp, " %02x", buf[i]);
			temp += n;
		}
		dev_dbg(&client->dev, "RMI4 I2C read %d bytes at %#06x:%s\n",
			len, addr, debug_buf);
	}
#endif

exit:
	mutex_unlock(&data->page_mutex);
	return retval;
}

static int rmi_i2c_read(struct rmi_phys_device *phys, u16 addr, u8 *buf)
{
	int retval = rmi_i2c_read_block(phys, addr, buf, 1);
	return (retval < 0) ? retval : 0;
}

static int acquire_attn_irq(struct rmi_i2c_data *data)
{
	return request_threaded_irq(data->irq, NULL, rmi_i2c_irq_thread,
			data->irq_flags, dev_name(data->phys->dev), data->phys);
}

static int enable_device(struct rmi_phys_device *phys)
{
	int retval = 0;

	struct rmi_i2c_data *data = phys->data;

	if (data->enabled)
		return 0;

	retval = acquire_attn_irq(data);
	if (retval)
		goto error_exit;

	data->enabled = true;
	dev_dbg(phys->dev, "Physical device enabled.\n");
	return 0;

error_exit:
	dev_err(phys->dev, "Failed to enable physical device. Code=%d.\n",
		retval);
	return retval;
}

static void disable_device(struct rmi_phys_device *phys)
{
	struct rmi_i2c_data *data = phys->data;

	if (!data->enabled)
		return;

	disable_irq(data->irq);
	free_irq(data->irq, data->phys);

	dev_dbg(phys->dev, "Physical device disabled.\n");
	data->enabled = false;
}

static int __devinit rmi_i2c_probe(struct i2c_client *client,
				  const struct i2c_device_id *id)
{
	struct rmi_phys_device *rmi_phys;
	struct rmi_i2c_data *data;
	struct rmi_device_platform_data *pdata = client->dev.platform_data;
	int error;
	int tk_regulator_vout;
        bool tk_regulator_flag = 1;

	/* 
	 * move the action of VDD_power_on to LCD because TP&LCD share the LDO
	 *
	 * */
    #if 0
#ifdef CONFIG_P2_TP_TK_CMD_FEATURE 
    struct regulator *tk_vdd;
    printk("CONFIG_P2_TP_TK_CMD_FEATURE i2c_probe\n");
    pdata->vdd = regulator_get(&client->dev, SYNAPTICS_VDD);
    if (IS_ERR(pdata->vdd)) {
        dev_err(&client->dev, "%s: failed to get synaptics vdd\n", __func__);
        return  -EINVAL;
    }
    error = regulator_enable(pdata->vdd);
    if (error < 0) {
        dev_err(&client->dev, "%s: failed to enable synaptics vdd\n", __func__);
        return -EINVAL;
    }

    tk_regulator_vout = get_touchkey_regulator_vout();
    if(E_TOUCHKEY_REGULATOR_VOUT17==tk_regulator_vout )
    {
        printk("rmi_i2c_probe so340010_V17\n");
        tk_vdd = regulator_get(&client->dev, "so340010_V17");
        if (IS_ERR(tk_vdd)) {
           dev_err(&client->dev, "%s: failed to get synaptics_tk vdd\n", __func__);
           tk_regulator_flag = 0;
        }
    }
    else if(E_TOUCHKEY_REGULATOR_VOUT13==tk_regulator_vout)
    {
        printk("rmi_i2c_probe so340010_V13\n");
        tk_vdd = regulator_get(&client->dev, "so340010_V13");
        if (IS_ERR(tk_vdd)) {
           dev_err(&client->dev, "%s: failed to get synaptics_tk vdd\n", __func__);
           tk_regulator_flag = 0;
        }
    }
    else
    {
        dev_err(&client->dev, "%s:get_touchkey_regulator_vout failed tk_regulator_vout=%d\n", __func__,tk_regulator_vout);
        tk_regulator_flag = 0;
    }

    if(1==tk_regulator_flag)
    {
        error = regulator_set_voltage(tk_vdd,2850000,2850000);
        if(error < 0){
            dev_err(&client->dev, "%s: failed to set synaptics_tk vdd\n", __func__);
            return -EINVAL;
        }
        error = regulator_enable(tk_vdd);
        if (error < 0) {
            dev_err(&client->dev, "%s: failed to enable synaptics_tk vdd\n", __func__);
            return -EINVAL;
        }
    }
#else
	pdata->vdd = regulator_get(&client->dev, SYNAPTICS_VDD);
	if (IS_ERR(pdata->vdd)) {
		dev_err(&client->dev, "%s: failed to get synaptics vdd\n", __func__);
		return  -EINVAL;
	}

	error = regulator_set_voltage(pdata->vdd,2850000,2850000);
	if(error < 0){
		dev_err(&client->dev, "%s: failed to set synaptics vdd\n", __func__);
		return  -EINVAL;
	}

	error = regulator_enable(pdata->vdd);
	if (error < 0) {
		dev_err(&client->dev, "%s: failed to enable synaptics vdd\n", __func__);
		return -EINVAL;
	}
#endif
    msleep(5);
	/* VBUS power on */
	pdata->vbus = regulator_get(&client->dev, SYNAPTICS_VBUS);
	if (IS_ERR(pdata->vbus)) {
		dev_err(&client->dev, "%s: failed to get synaptics vbus\n", __func__);
		return -EINVAL;
	}

	error = regulator_set_voltage(pdata->vbus,1800000,1800000);
	if(error < 0){
		dev_err(&client->dev, "%s: failed to set synaptics vbus\n", __func__);
		return -EINVAL;
	}

	error = regulator_enable(pdata->vbus);
	if (error < 0) {
		dev_err(&client->dev, "%s: failed to enable synaptics vbus\n", __func__);
		return -EINVAL;
	}
    #endif
    msleep(10);

	if (!pdata) {
		dev_err(&client->dev, "no platform data\n");
		return -EINVAL;
	}
	dev_info(&client->dev, "Probing %s at %#02x (IRQ %d).\n",
		pdata->sensor_name ? pdata->sensor_name : "-no name-",
		client->addr, pdata->attn_gpio);
	error = set_touch_chip_info(TOUCH_INFO_RMI3250);
	if (error) {
		dev_err(&client->dev, "set_touch_chip_info error\n");
	}

	if (pdata->gpio_config) {
		dev_info(&client->dev, "Configuring GPIOs.\n");
		error = pdata->gpio_config(pdata->gpio_data, true);
		if (error < 0) {
			dev_err(&client->dev, "Failed to configure GPIOs, code: %d.\n",
				error);
			return error;
		}
		dev_info(&client->dev, "Done with GPIO configuration.\n");
	}

	error = i2c_check_functionality(client->adapter, I2C_FUNC_I2C);
	if (!error) {
		dev_err(&client->dev, "i2c_check_functionality error %d.\n",
			error);
		return error;
	}

	rmi_phys = kzalloc(sizeof(struct rmi_phys_device), GFP_KERNEL);
	if (!rmi_phys)
		return -ENOMEM;

	data = kzalloc(sizeof(struct rmi_i2c_data), GFP_KERNEL);
	if (!data) {
		error = -ENOMEM;
		goto err_phys;
	}

	data->enabled = true;	/* We plan to come up enabled. */
	data->irq = gpio_to_irq(pdata->attn_gpio);
	if (pdata->level_triggered) {
		data->irq_flags = IRQF_ONESHOT |
			((pdata->attn_polarity == RMI_ATTN_ACTIVE_HIGH) ?
			IRQF_TRIGGER_HIGH : IRQF_TRIGGER_LOW);
	} else {
		data->irq_flags =
			(pdata->attn_polarity == RMI_ATTN_ACTIVE_HIGH) ?
			IRQF_TRIGGER_RISING : IRQF_TRIGGER_FALLING;
	}
	data->phys = rmi_phys;

	rmi_phys->data = data;
	rmi_phys->dev = &client->dev;

	rmi_phys->write = rmi_i2c_write;
	rmi_phys->write_block = rmi_i2c_write_block;
	rmi_phys->read = rmi_i2c_read;
	rmi_phys->read_block = rmi_i2c_read_block;
	rmi_phys->enable_device = enable_device;
	rmi_phys->disable_device = disable_device;

	rmi_phys->info.proto = phys_proto_name;
	
	mutex_init(&data->page_mutex);

	/* Setting the page to zero will (a) make sure the PSR is in a
	 * known state, and (b) make sure we can talk to the device.
	 */
	msleep(100);
	error = rmi_set_page(rmi_phys, 0);
	if (error) {
		dev_err(&client->dev, "Failed to set page select to 0.\n");
		goto err_data;
	}

	error = rmi_register_phys_device(rmi_phys);
	if (error) {
		dev_err(&client->dev,
			"failed to register physical driver at 0x%.2X.\n",
			client->addr);
		goto err_gpio;
	}
	i2c_set_clientdata(client, rmi_phys);

	if (pdata->attn_gpio > 0) {
		error = acquire_attn_irq(data);
		if (error < 0) {
			dev_err(&client->dev,
				"request_threaded_irq failed %d\n",
				pdata->attn_gpio);
			goto err_unregister;
		}
	}

#if defined(CONFIG_RMI4_DEV)
	error = gpio_export(pdata->attn_gpio, false);
	if (error) {
		dev_warn(&client->dev,
			 "WARNING: Failed to export ATTN gpio!\n");
		error = 0;
	} else {
		error = gpio_export_link(&(rmi_phys->rmi_dev->dev), "attn",
					pdata->attn_gpio);
		if (error) {
			dev_warn(&(rmi_phys->rmi_dev->dev),
				 "WARNING: Failed to symlink ATTN gpio!\n");
			error = 0;
		} else {
			dev_info(&(rmi_phys->rmi_dev->dev),
				"%s: Exported ATTN GPIO %d.", __func__,
				pdata->attn_gpio);
		}
	}
#endif /* CONFIG_RMI4_DEV */

	dev_info(&client->dev, "registered rmi i2c driver at %#04x.\n",
			client->addr);
	return 0;

err_unregister:
	rmi_unregister_phys_device(rmi_phys);
err_gpio:
	if (pdata->gpio_config)
		pdata->gpio_config(pdata->gpio_data, false);
err_data:
	kfree(data);
err_phys:
	kfree(rmi_phys);
	return error;
}

static int __devexit rmi_i2c_remove(struct i2c_client *client)
{
	struct rmi_phys_device *phys = i2c_get_clientdata(client);
	struct rmi_device_platform_data *pd = client->dev.platform_data;

	disable_device(phys);
	rmi_unregister_phys_device(phys);
	kfree(phys->data);
	kfree(phys);

	if (pd->gpio_config)
		pd->gpio_config(&pd->gpio_data, false);

	return 0;
}

static const struct i2c_device_id rmi_id[] = {
	{ "rmi", 0 },
	{ "rmi_i2c", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, rmi_id);

static struct i2c_driver rmi_i2c_driver = {
	.driver = {
		.owner	= THIS_MODULE,
		.name	= "rmi_i2c"
	},
	.id_table	= rmi_id,
	.probe		= rmi_i2c_probe,
	.remove		= __devexit_p(rmi_i2c_remove),
};

static int __init rmi_i2c_init(void)
{
	return i2c_add_driver(&rmi_i2c_driver);
}

static void __exit rmi_i2c_exit(void)
{
	i2c_del_driver(&rmi_i2c_driver);
}

module_init(rmi_i2c_init);
module_exit(rmi_i2c_exit);

MODULE_AUTHOR("Christopher Heiny <cheiny@synaptics.com>");
MODULE_DESCRIPTION("RMI I2C driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(RMI_DRIVER_VERSION);
