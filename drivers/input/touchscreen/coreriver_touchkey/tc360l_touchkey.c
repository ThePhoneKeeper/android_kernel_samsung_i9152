/*
 *
 * Copyright 2005 Phil Blundell
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/sched.h>
#include <linux/pm.h>
#include <linux/sysctl.h>
#include <linux/proc_fs.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/input.h>


#include <linux/gpio.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/earlysuspend.h>
#include <linux/io.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>

#include <linux/i2c/coreriver_touchkey.h>

#if defined (TK_HAS_FIRMWARE_UPDATE)
#include "issp_extern.h"
#endif

#define TOUCHKEY_LDO_EN 130

#define TOUCHKEY_INT 4

#define TOUCHKEY_SCL 1
#define TOUCHKEY_SDA 0

#define I2C_RETRY_CNT	2

static int touchkey_keycode[3] = { 0, KEY_MENU, KEY_BACK,};

static const int touchkey_count = sizeof(touchkey_keycode) / sizeof(int);

#if defined(TK_HAS_AUTOCAL)
static u8 home_sensitivity;
static u8 search_sensitivity;
static u16 raw_data0;
static u16 raw_data1;
static u16 raw_data2;
static u16 raw_data3;
static u8 idac0;
static u8 idac1;
static u8 idac2;
static u8 idac3;
static u8 touchkey_threshold;

static int touchkey_autocalibration(struct touchkey_i2c *tkey_i2c);
#endif


#if defined(SEC_TKEY_EVENT_DEBUG)
static bool g_debug_tkey = 1;
#else
static bool g_debug_tkey = 0;
#endif


static int touchkey_i2c_check(struct touchkey_i2c *tkey_i2c);

static int menu_sensitivity;
static int back_sensitivity;

static int touchkey_enable;
static int current_intensity = 0;
static int touchkey_suspended;


MODULE_DEVICE_TABLE(i2c, sec_touchkey_id);

static int touchkey_led_status;

static int touchkey_debug_count;
static char touchkey_debug[104];


static void set_touchkey_debug(char value)
{
	if (touchkey_debug_count == 100)
		touchkey_debug_count = 0;

	touchkey_debug[touchkey_debug_count] = value;
	touchkey_debug_count++;
}

static int i2c_touchkey_write(struct i2c_client *client,
		u8 *wbuf, unsigned int len)
{
	int err = 0;
	int retry = 3;
	struct i2c_msg msg[1];

	while (retry--) {

		msg->addr = client->addr;
		msg->flags = I2C_M_WR;
		msg->len = len;
		msg->buf = wbuf;
		err = i2c_transfer(client->adapter, msg, 1);

		if (err >= 0)
			return 0;

		printk(KERN_DEBUG "[TouchKey] %s %d i2c transfer error\n",
		       __func__, __LINE__);
		mdelay(10);
	}
	return err;
}


#if defined(CONFIG_S2VE_TOUCHKEY_REV05)

static struct regulator *touchkey_regulator_2_8=NULL;
static int IsTouchkeyPowerOn;


static int Touchkey_PowerOnOff(int on)
{
	int ret;

	printk("[TouchKey] %s, onoff=%d\n",__func__, on);
	
	if(on)
	{
		if(touchkey_regulator_2_8 == NULL)
		{
			touchkey_regulator_2_8 = regulator_get(NULL, "gpldo5_uc");
			if(IS_ERR(touchkey_regulator_2_8)){
				printk("[TouchKey] can not get TSP VDD 2.8V\n"); /*touch, touch key power*/
				touchkey_regulator_2_8 = NULL;
				return;
				}	
				
			ret = regulator_set_voltage(touchkey_regulator_2_8,2800000,2800000);
			printk("[TouchKey] %s --> regulator_set_voltage ret = %d \n",__func__, ret);
		}

		ret = regulator_enable(touchkey_regulator_2_8);
		printk("[TouchKey] --> 2.8v regulator_enable ret = %d \n", ret);
		IsTouchkeyPowerOn=1;
	}
	else
	{
		ret = regulator_force_disable(touchkey_regulator_2_8);
		IsTouchkeyPowerOn=0;
		printk("[TouchKey] --> 2.8v regulator_disable ret = %d \n", ret);
	}

	return 0;
}
#endif


void touchkey_led_on(struct touchkey_i2c *tkey_i2c, bool on)
{
	int ret, i;
	uint8_t cmdbuf[1]={0,};

	if (on) 
	{
		gpio_set_value(TOUCHKEY_LDO_EN, 1);

		msleep(1);
	
		for (i = 0; i < I2C_RETRY_CNT; i++)
		{
			cmdbuf[0] = 0x01;//cmd
			ret = i2c_master_send(tkey_i2c->client, cmdbuf, 1);

			if (ret >= 0)
			{
				printk("[TouchKey] %s : on\n", __func__);
				break; // i2c success
			}
		}

	}
	else
	{

		gpio_set_value(TOUCHKEY_LDO_EN, 0);
	
	}

	if(ret<0)
		printk("[TouchKey] %s : failed\n", __func__);	 


}


static int i2c_touchkey_read(struct i2c_client *client,
		u8 reg, u8 *rbuf, unsigned int len)
{
	int err = 0;
	int retry = 3;
	struct i2c_msg msg[1];
	uint8_t start_reg;

	

	if ((client == NULL) ||
#if defined(CONFIG_S2VE_TOUCHKEY_REV05)
	(touchkey_suspended == 1)
#else
	!(touchkey_enable == 1)
#endif
	) {
		printk(KERN_ERR "[TouchKey] touchkey is not enabled. %d\n",
		       __LINE__);
		return -ENODEV;
	}
	
	while (retry--) {

		msg->addr = client->addr;
		msg->flags = I2C_M_RD;
		msg->len = len;
		msg->buf = rbuf;
		err = i2c_transfer(client->adapter, msg, 1);

		if (err >= 0)
			return 0;

		printk(KERN_ERR "[TouchKey] %s %d i2c transfer error\n",
		       __func__, __LINE__);
		mdelay(10);
	}
	return err;

}


#if defined(TK_HAS_AUTOCAL)
static int touchkey_autocalibration(struct touchkey_i2c *tkey_i2c)
{
	u8 data[6] = { 0, };
	int count = 0;
	int ret = 0;
	unsigned short retry = 0;

	while (retry < 3) {
		ret = i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 4);
		if (ret < 0) {
			printk(KERN_ERR "[TouchKey]i2c read fail.\n");
			return ret;
		}
		printk(KERN_DEBUG
				"[TouchKey] data[0]=%x data[1]=%x data[2]=%x data[3]=%x\n",
				data[0], data[1], data[2], data[3]);

		/* Send autocal Command */
		data[0] = 0x50;
		data[3] = 0x01;

		count = i2c_touchkey_write(tkey_i2c->client, data, 4);

		msleep(100);

		/* Check autocal status */
		ret = i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 6);

		if ((data[5] & TK_BIT_AUTOCAL)) {
			printk(KERN_DEBUG "[Touchkey] autocal Enabled\n");
			break;
		} else
			printk(KERN_DEBUG
			       "[Touchkey] autocal disabled, retry %d\n",
			       retry);

		retry = retry + 1;
	}

	if (retry == 3)
		printk(KERN_DEBUG "[Touchkey] autocal failed\n");

	return count;
}
#endif


static ssize_t set_touchkey_autocal_testmode(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t size)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	int count = 0;
	u8 set_data;
	int on_off;

	if (sscanf(buf, "%d\n", &on_off) == 1) {
		printk(KERN_ERR "[TouchKey] Test Mode : %d\n", on_off);

		if (on_off == 1) {
			set_data = 0x40;
			count = i2c_touchkey_write(tkey_i2c->client,
					&set_data, 1);
		} else {

			touchkey_led_on(tkey_i2c, 0);
			msleep(50);
			touchkey_led_on(tkey_i2c, 1);
			msleep(50);

#if defined(TK_HAS_AUTOCAL)
			touchkey_autocalibration(tkey_i2c);
#endif
		}
	} else {
		printk(KERN_ERR "[TouchKey] touch_led_brightness Error\n");
	}

	return count;
}

#if defined(TK_HAS_AUTOCAL)
static ssize_t touchkey_raw_data0_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	u8 data[26] = { 0, };
	int ret;

	printk(KERN_DEBUG "called %s\n", __func__);
	ret = i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 26);


	printk(KERN_DEBUG "called %s data[16] =%d,data[17] = %d\n", __func__,
	       data[16], data[17]);
	raw_data0 = ((0x00FF & data[14]) << 8) | data[15];
	
	return sprintf(buf, "%d\n", raw_data0);
}

static ssize_t touchkey_raw_data1_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	u8 data[26] = { 0, };
	int ret;

	printk(KERN_DEBUG "called %s\n", __func__);
	ret = i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 26);

	printk(KERN_DEBUG "called %s data[14] =%d,data[15] = %d\n", __func__,
			   data[14], data[15]);
	raw_data1 = ((0x00FF & data[16]) << 8) | data[17];

	return sprintf(buf, "%d\n", raw_data1);
}

static ssize_t touchkey_raw_data2_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	u8 data[26] = { 0, };
	int ret;

	printk(KERN_DEBUG "called %s\n", __func__);
	ret = i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 26);
	printk(KERN_DEBUG "called %s data[22] =%d,data[23] = %d\n", __func__,
	       data[14], data[15]);
	raw_data2 = ((0x00FF & data[14]) << 8) | data[15];

	return sprintf(buf, "%d\n", raw_data2);
}

static ssize_t touchkey_raw_data3_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	u8 data[26] = { 0, };
	int ret;

	printk(KERN_DEBUG "called %s\n", __func__);
	ret = i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 26);

	printk(KERN_DEBUG "called %s data[24] =%d,data[25] = %d\n", __func__,
	       data[16], data[17]);
	raw_data3 = ((0x00FF & data[16]) << 8) | data[17];

	return sprintf(buf, "%d\n", raw_data3);
}

static ssize_t touchkey_idac0_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	u8 data[10];
	int ret;

	printk(KERN_DEBUG "called %s\n", __func__);
	ret = i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 10);
	printk(KERN_DEBUG "called %s data[6] =%d\n", __func__, data[6]);
	idac0 = data[6];
	return sprintf(buf, "%d\n", idac0);
}

static ssize_t touchkey_idac1_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	u8 data[10];
	int ret;

	printk(KERN_DEBUG "called %s\n", __func__);
	ret = i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 10);
	printk(KERN_DEBUG "called %s data[7] = %d\n", __func__, data[7]);
	idac1 = data[7];
	return sprintf(buf, "%d\n", idac1);
}

static ssize_t touchkey_idac2_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	u8 data[10];
	int ret;


	printk(KERN_DEBUG "called %s\n", __func__);
	ret = i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 10);
	printk(KERN_DEBUG "called %s data[8] =%d\n", __func__, data[8]);
	idac2 = data[8];
	return sprintf(buf, "%d\n", idac2);
}

static ssize_t touchkey_idac3_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	u8 data[10];
	int ret;

	printk(KERN_DEBUG "called %s\n", __func__);
	ret = i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 10);
	printk(KERN_DEBUG "called %s data[9] = %d\n", __func__, data[9]);
	idac3 = data[9];
	return sprintf(buf, "%d\n", idac3);
}

static ssize_t touchkey_threshold_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	u8 data[10];
	int ret;

	printk(KERN_DEBUG "called %s\n", __func__);
	ret = i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 10);
	printk(KERN_DEBUG "called %s data[4] = %d\n", __func__, data[4]);
	touchkey_threshold = data[4];
	return sprintf(buf, "%d\n", touchkey_threshold);
}
#endif

#if defined(TK_HAS_FIRMWARE_UPDATE)
static int touchkey_firmware_update(struct touchkey_i2c *tkey_i2c)
{
	int retry = 3;
	int ret = 0;
	char data[3];

	disable_irq(tkey_i2c->irq);


	ret = i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 3);
	if (ret < 0) {
		printk(KERN_DEBUG
		"[TouchKey] i2c read fail. do not excute firm update.\n");
		data[1] = 0;
		data[2] = 0;
	}

	printk(KERN_ERR "%s F/W version: 0x%x, Module version:0x%x\n", __func__,
	data[1], data[2]);

	tkey_i2c->firmware_ver = data[1];
	tkey_i2c->module_ver = data[2];


	if ((tkey_i2c->firmware_ver < TK_FIRMWARE_VER) &&
	    (tkey_i2c->module_ver <= TK_MODULE_VER)) {

		printk(KERN_DEBUG "[TouchKey] firmware auto update excute\n");

		tkey_i2c->update_status = TK_UPDATE_DOWN;

		while (retry--) {
			if (ISSP_main(tkey_i2c) == 0) {
				printk(KERN_DEBUG
				       "[TouchKey]firmware update succeeded\n");
				tkey_i2c->update_status = TK_UPDATE_PASS;
				msleep(50);
				break;
			}
			msleep(50);
			printk(KERN_DEBUG
			       "[TouchKey] firmware update failed. retry\n");
		}
		if (retry <= 0) {
			touchkey_led_on(tkey_i2c, 0);			
			tkey_i2c->update_status = TK_UPDATE_FAIL;
			printk(KERN_DEBUG
			       "[TouchKey] firmware update failed.\n");
		}
		ret = touchkey_i2c_check(tkey_i2c);
		if (ret < 0) {
			printk(KERN_DEBUG
				"[TouchKey] i2c read fail.\n");
			return TK_UPDATE_FAIL;
		}

		printk(KERN_DEBUG "[TouchKey] firm ver = %d, module ver = %d\n",
			tkey_i2c->firmware_ver, tkey_i2c->module_ver);
	} else {
		printk(KERN_DEBUG
		       "[TouchKey] firmware auto update do not excute\n");
		printk(KERN_DEBUG
		       "[TouchKey] firmware_ver(banary=%d, current=%d)\n",
		       TK_FIRMWARE_VER, tkey_i2c->firmware_ver);
		printk(KERN_DEBUG
		       "[TouchKey] module_ver(banary=%d, current=%d)\n",
		       TK_MODULE_VER, tkey_i2c->module_ver);
	}
	enable_irq(tkey_i2c->irq);
	return TK_UPDATE_PASS;
}
#endif

extern int get_tsp_status(void);


#ifndef TEST_JIG_MODE
static irqreturn_t touchkey_interrupt(int irq, void *dev_id)
{
	struct touchkey_i2c *tkey_i2c = dev_id;
	u8 data[3];
	int ret;
	int retry = 10;
	int keycode_type = 0;
	int pressed;

	set_touchkey_debug('a');

	printk("[TouchKey] %s", __func__);


	retry = 3;
	while (retry--) {
		ret = i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 3);
		if (!ret)
			break;
		else {
			printk(KERN_DEBUG
			       "[TouchKey] i2c read failed, ret:%d, retry: %d\n",
			       ret, retry);
			continue;
		}
	}
	if (ret < 0)
		return IRQ_HANDLED;

	set_touchkey_debug(data[0]);

	keycode_type = (data[0] & TK_BIT_KEYCODE);
	pressed = !(data[0] & TK_BIT_PRESS_EV);

	printk("[TouchKey] keycode_type:%d \n", keycode_type);

	if (keycode_type <= 0 || keycode_type >= touchkey_count) {
		printk(KERN_DEBUG "[Touchkey] keycode_type err\n");
		return IRQ_HANDLED;
	}

	if (pressed)
		set_touchkey_debug('P');

	if (get_tsp_status() && pressed)
		printk(KERN_DEBUG "[TouchKey] touchkey pressed but don't send event because touch is pressed.\n");
	else {
		input_report_key(tkey_i2c->input_dev, touchkey_keycode[keycode_type], pressed);
		input_sync(tkey_i2c->input_dev);

	 printk("[TouchKey] keycode:%d pressed:%d\n",  touchkey_keycode[keycode_type], pressed);

		if (g_debug_tkey == true) {
			printk(KERN_DEBUG "[TouchKey] keycode[%d]=%d pressed:%d\n",
			keycode_type, touchkey_keycode[keycode_type], pressed);
		} else {
			printk(KERN_DEBUG "[TouchKey] pressed:%d\n", pressed);
		}

	}
	set_touchkey_debug('A');
	return IRQ_HANDLED;
}
#else
static irqreturn_t touchkey_interrupt(int irq, void *dev_id)
{
	struct touchkey_i2c *tkey_i2c = dev_id;
	u8 data[18];
	int ret;
	int retry = 10;
	int keycode_type = 0;
	int pressed;

	printk("[TouchKey] %s", __func__);

	set_touchkey_debug('a');


	retry = 3;
	while (retry--) {
		ret = i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 18);
		if (!ret)
			break;
		else {
			printk(KERN_DEBUG
			       "[TouchKey] i2c read failed, ret:%d, retry: %d\n",
			       ret, retry);
			continue;
		}
	}
	if (ret < 0)
		return IRQ_HANDLED;

	menu_sensitivity = data[13];
	back_sensitivity = data[11];

	set_touchkey_debug(data[0]);

	keycode_type = (data[0] & TK_BIT_KEYCODE);
	pressed = !(data[0] & TK_BIT_PRESS_EV);

	if (keycode_type <= 0 || keycode_type >= touchkey_count) {
		printk(KERN_DEBUG "[Touchkey] keycode_type err\n");
		return IRQ_HANDLED;
	}

	if (pressed)
		set_touchkey_debug('P');

	if (get_tsp_status() && pressed)
		printk(KERN_DEBUG "[TouchKey] touchkey pressed"
		       " but don't send event because touch is pressed.\n");
	else {
		input_report_key(touchkey_driver->input_dev,
				 touchkey_keycode[keycode_type], pressed);
		input_sync(touchkey_driver->input_dev);
		/* printk(KERN_DEBUG "[TouchKey] keycode:%d pressed:%d\n",
		   touchkey_keycode[keycode_index], pressed); */
	}

	if (keycode_type == 2)
		printk(KERN_DEBUG "back key sensitivity = %d\n",
		       back_sensitivity);
	if (keycode_type == 4)
		printk(KERN_DEBUG "menu key sensitivity = %d\n",
		       menu_sensitivity);

	set_touchkey_debug('A');
	return IRQ_HANDLED;
}
#endif

#ifdef CONFIG_HAS_EARLYSUSPEND
static int sec_touchkey_early_suspend(struct early_suspend *h)
{
	struct touchkey_i2c *tkey_i2c =
		container_of(h, struct touchkey_i2c, early_suspend);
	int ret;
	int i;

	gpio_direction_output(TOUCHKEY_INT, 0);
	
	gpio_set_value(TOUCHKEY_SDA, 1); 
	gpio_set_value(TOUCHKEY_SCL, 1); 	

	disable_irq(tkey_i2c->irq);
	printk("[TouchKey] disable_irq...\n");
	
	ret = cancel_work_sync(&tkey_i2c->update_work);
	if (ret) {
		printk(KERN_DEBUG "[Touchkey] enable_irq ret=%d\n", ret);
		enable_irq(tkey_i2c->irq);
	}

	/* release keys */
	for (i = 1; i < touchkey_count; ++i) {
		input_report_key(tkey_i2c->input_dev,
				 touchkey_keycode[i], 0);
	}
	input_sync(tkey_i2c->input_dev);


#if defined(CONFIG_S2VE_TOUCHKEY_REV05)

	set_touchkey_debug('S');
	printk(KERN_DEBUG "[TouchKey] sec_touchkey_early_suspend\n");
	touchkey_suspended = 1;
	if(IsTouchkeyPowerOn==1)
		Touchkey_PowerOnOff(0);

#else	

	touchkey_enable = 0;
	set_touchkey_debug('S');
	printk(KERN_DEBUG "[TouchKey] sec_touchkey_early_suspend\n");

#endif	

	return 0;
}

static int sec_touchkey_late_resume(struct early_suspend *h)
{
	struct touchkey_i2c *tkey_i2c =
		container_of(h, struct touchkey_i2c, early_suspend);
#ifdef TEST_JIG_MODE
	unsigned char get_touch = 0x40;
#endif

	set_touchkey_debug('R');
	printk(KERN_DEBUG "[TouchKey] sec_touchkey_late_resume\n");

	gpio_direction_input(TOUCHKEY_INT);
	
#if defined(CONFIG_S2VE_TOUCHKEY_REV05)
	touchkey_suspended = 0;
#else

	touchkey_enable = 1;

#if defined(TK_HAS_AUTOCAL)
	touchkey_autocalibration(tkey_i2c);
#endif

#ifdef TEST_JIG_MODE
	i2c_touchkey_write(tkey_i2c->client, &get_touch, 1);
#endif

	enable_irq(tkey_i2c->irq);
	printk("[TouchKey] enable_irq...\n");

#endif
	return 0;
}
#endif

static int touchkey_i2c_check(struct touchkey_i2c *tkey_i2c)
{
	char data[3] = { 0, };
	int ret = 0;

	printk("[TouchKey] %s\n", __func__);	

	ret = i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 3);
	if (ret < 0) {
		printk(KERN_ERR "[TouchKey] module version read fail\n");
		return ret;
	}

	tkey_i2c->firmware_ver = data[1];
	tkey_i2c->module_ver = data[2];

	return ret;
}


static ssize_t touch_version_read(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	char data[3] = { 0, };
	int count=0;

	printk("[TouchKey] %s\n", __func__);	


	i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 3);

	count = sprintf(buf, "0x%x\n", data[1]);

	printk(KERN_DEBUG "[TouchKey] touch_version_read 0x%x\n", data[1]);
	printk(KERN_DEBUG "[TouchKey] module_version_read 0x%x\n", data[2]);

	return count;
}

#if defined(TK_HAS_FIRMWARE_UPDATE)

void touchkey_update_func(struct work_struct *work)
{

	struct touchkey_i2c *tkey_i2c =
		container_of(work, struct touchkey_i2c, update_work);
	int retry = 3;

	char data[3];
	i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 3);
	printk(KERN_DEBUG "[%s] F/W version: 0x%x, Module version:0x%x\n",
	       __func__, data[1], data[2]);

	tkey_i2c->update_status = TK_UPDATE_DOWN;
	printk(KERN_DEBUG "[TouchKey] %s start\n", __func__);
	touchkey_enable = 0;
	while (retry--) {
		if (ISSP_main(tkey_i2c) == 0) {
			printk(KERN_DEBUG
			       "[TouchKey] touchkey_update succeeded\n");
			msleep(50);
			touchkey_enable = 1;
#if defined(TK_HAS_AUTOCAL)
			touchkey_autocalibration(tkey_i2c);
#endif
			tkey_i2c->update_status = TK_UPDATE_PASS;
			enable_irq(tkey_i2c->irq);
			return;
		}
		touchkey_led_on(tkey_i2c, 0);		
	}
	enable_irq(tkey_i2c->irq);
	tkey_i2c->update_status = TK_UPDATE_FAIL;
	printk(KERN_DEBUG "[TouchKey] touchkey_update failed\n");

	return;
}

#endif

static ssize_t touch_update_write(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t size)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
		printk(KERN_DEBUG "[TouchKey] touchkey firmware update\n");

		if (*buf == 'S') {
			disable_irq(tkey_i2c->irq);
			schedule_work(&tkey_i2c->update_work);
		}
		return size;
}

static ssize_t touch_update_read(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	int count = 0;

	printk(KERN_DEBUG
	       "[TouchKey] touch_update_read: update_status %d\n",
	       tkey_i2c->update_status);

	if (tkey_i2c->update_status == TK_UPDATE_PASS)
		count = sprintf(buf, "PASS\n");
	else if (tkey_i2c->update_status == TK_UPDATE_DOWN)
		count = sprintf(buf, "Downloading\n");
	else if (tkey_i2c->update_status == TK_UPDATE_FAIL)
		count = sprintf(buf, "Fail\n");

	return count;
}

static ssize_t touchkey_led_control(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t size)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	int user_intensity;
	int ret;
	static const int ledCmd[] = {TK_CMD_LED_ON, TK_CMD_LED_OFF};

	ret = sscanf(buf, "%d", &user_intensity);


#if defined(CONFIG_S2VE_TOUCHKEY_REV05)

	printk("[TouchKey] user_intensity=%d, current_intensity=%d\n", user_intensity, current_intensity);

	if(user_intensity>=100 && current_intensity == 0)
	{
		if(IsTouchkeyPowerOn==0)
		{
		Touchkey_PowerOnOff(1);
			msleep(40);
		}
		touchkey_led_on(tkey_i2c, 1);

		enable_irq(tkey_i2c->irq);
		printk("[TouchKey] enable_irq...\n");
		
	}
	else if(user_intensity==0 && current_intensity != 0)
	{
		touchkey_led_on(tkey_i2c, 0);
	}
	current_intensity = user_intensity;


#else

	printk("[TouchKey] touchkey_enable=%d, user_intensity=%d \n",touchkey_enable, user_intensity);

	if(touchkey_enable==0)
    {
		user_intensity=0;
    }

	printk("[TouchKey] %s, %d \n",__func__, user_intensity);

	if(user_intensity>=100)
		touchkey_led_on(tkey_i2c, 1);
	else
	{
	  if(touchkey_enable) touchkey_led_on(tkey_i2c, 0);
	  else printk("%s, Touchkey already turned off. \n",__func__);
	}

#endif


	return size;
}

static ssize_t touchkey_menu_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);

	u8 data[14] = { 0, };
	int ret, i;

	printk("[TouchKey] %s\n", __func__);
	ret = i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 14);

#if 0
    for(i=0;i<14;i++)
		printk("[TouchKey] data[%d]=%d\n",i, data[i]);
#endif

	menu_sensitivity = data[10] << 8 | data[11];
	return sprintf(buf, "%d\n", menu_sensitivity);
}

static ssize_t touchkey_back_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);

	u8 data[14] = { 0, };
	int ret, i;

	printk("[TouchKey] %s\n", __func__);
	
	ret = i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 14);

#if 0
    for(i=0;i<14;i++)
		printk("[TouchKey] data[%d]=%d\n",i, data[i]);
#endif
	back_sensitivity = data[12] << 8 | data[13];
	return sprintf(buf, "%d\n", back_sensitivity);
}


#if defined(TK_HAS_AUTOCAL)
static ssize_t autocalibration_enable(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t size)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	int data;

	sscanf(buf, "%d\n", &data);

	if (data == 1)
		touchkey_autocalibration(tkey_i2c);

	return size;
}

static ssize_t autocalibration_status(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	u8 data[6];
	int ret;
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);

	printk(KERN_DEBUG "[Touchkey] %s\n", __func__);

	ret = i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 6);
	if ((data[5] & TK_BIT_AUTOCAL))
		return sprintf(buf, "Enabled\n");
	else
		return sprintf(buf, "Disabled\n");

}
#endif

static ssize_t touch_sensitivity_control(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t size)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	unsigned char data = 0x40;
	printk("[TouchKey] %s\n", __func__);
	i2c_touchkey_write(tkey_i2c->client, &data, 1);
	return size;
}

static ssize_t set_touchkey_firm_version_show(struct device *dev,
					      struct device_attribute *attr,
					      char *buf)
{
	return sprintf(buf, "0x%x\n", TK_FIRMWARE_VER);
}
#if defined(TK_HAS_FIRMWARE_UPDATE)
static ssize_t set_touchkey_update_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	int count = 0;
	int retry = 3;

	tkey_i2c->update_status = TK_UPDATE_DOWN;

	disable_irq(tkey_i2c->irq);

#ifdef TEST_JIG_MODE
	unsigned char get_touch = 0x40;
#endif

	while (retry--) {
		if (ISSP_main(tkey_i2c) == 0) {
			printk(KERN_ERR
			       "[TouchKey]Touchkey_update succeeded\n");
			tkey_i2c->update_status = TK_UPDATE_PASS;
			count = 1;
			msleep(50);
			break;
		}
		printk(KERN_ERR "touchkey_update failed... retry...\n");
	}
	if (retry <= 0) {
		/* disable ldo11 */
		touchkey_led_on(tkey_i2c, 0);
		count = 0;
		printk(KERN_ERR "[TouchKey]Touchkey_update fail\n");
		tkey_i2c->update_status = TK_UPDATE_FAIL;
		enable_irq(tkey_i2c->irq);
		return count;
	}

#ifdef TEST_JIG_MODE
	i2c_touchkey_write(tkey_i2c->client, &get_touch, 1);
#endif

	enable_irq(tkey_i2c->irq);

	return count;

}

static ssize_t set_touchkey_firm_status_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	int count = 0;

	printk(KERN_DEBUG
	       "[TouchKey] touch_update_read: update_status %d\n",
	       tkey_i2c->update_status);

	if (tkey_i2c->update_status == TK_UPDATE_PASS)
		count = sprintf(buf, "PASS\n");
	else if (tkey_i2c->update_status == TK_UPDATE_DOWN)
		count = sprintf(buf, "Downloading\n");
	else if (tkey_i2c->update_status == TK_UPDATE_FAIL)
		count = sprintf(buf, "Fail\n");

	return count;
}

#endif
static ssize_t set_touchkey_firm_version_read_show(struct device *dev,
						   struct device_attribute
						   *attr, char *buf)
{
	struct touchkey_i2c *tkey_i2c = dev_get_drvdata(dev);
	char data[3] = { 0, };
	int count=0;

	printk("[TouchKey] %s\n", __func__);


	i2c_touchkey_read(tkey_i2c->client, KEYCODE_REG, data, 3);
	count = sprintf(buf, "0x%x\n", data[1]);

	printk("[TouchKey] touch_version_read 0x%x\n", data[1]);
	printk("[TouchKey] module_version_read 0x%x\n", data[2]);

	return count;
}


static DEVICE_ATTR(recommended_version, S_IRUGO | S_IWUSR | S_IWGRP,
		   touch_version_read, NULL);
static DEVICE_ATTR(updated_version, S_IRUGO | S_IWUSR | S_IWGRP,
		   touch_update_read, touch_update_write);
static DEVICE_ATTR(brightness, 0777 /*S_IRUGO | S_IWUSR | S_IWGRP*/, NULL,
		   touchkey_led_control);
static DEVICE_ATTR(touchkey_menu, S_IRUGO | S_IWUSR | S_IWGRP,
		   touchkey_menu_show, NULL);
static DEVICE_ATTR(touchkey_back, S_IRUGO | S_IWUSR | S_IWGRP,
		   touchkey_back_show, NULL);

static DEVICE_ATTR(touch_sensitivity, S_IRUGO | S_IWUSR | S_IWGRP, NULL,
		   touch_sensitivity_control);

#if defined(TK_HAS_FIRMWARE_UPDATE)
static DEVICE_ATTR(touchkey_firm_update, S_IRUGO | S_IWUSR | S_IWGRP,
	set_touchkey_update_show, NULL);
static DEVICE_ATTR(touchkey_firm_update_status, S_IRUGO | S_IWUSR | S_IWGRP,
	set_touchkey_firm_status_show, NULL);
#endif	
static DEVICE_ATTR(touchkey_firm_version_phone, S_IRUGO | S_IWUSR | S_IWGRP,
	set_touchkey_firm_version_show, NULL);
static DEVICE_ATTR(touchkey_firm_version_panel, S_IRUGO | S_IWUSR | S_IWGRP,
		   set_touchkey_firm_version_read_show, NULL);


static DEVICE_ATTR(touchkey_autocal_start, S_IRUGO | S_IWUSR | S_IWGRP, NULL,
		   set_touchkey_autocal_testmode);


#if defined(TK_HAS_AUTOCAL)
static DEVICE_ATTR(touchkey_raw_data0, S_IRUGO, touchkey_raw_data0_show, NULL);
static DEVICE_ATTR(touchkey_raw_data1, S_IRUGO, touchkey_raw_data1_show, NULL);
static DEVICE_ATTR(touchkey_raw_data2, S_IRUGO, touchkey_raw_data2_show, NULL);
static DEVICE_ATTR(touchkey_raw_data3, S_IRUGO, touchkey_raw_data3_show, NULL);
static DEVICE_ATTR(touchkey_idac0, S_IRUGO, touchkey_idac0_show, NULL);
static DEVICE_ATTR(touchkey_idac1, S_IRUGO, touchkey_idac1_show, NULL);
static DEVICE_ATTR(touchkey_idac2, S_IRUGO, touchkey_idac2_show, NULL);
static DEVICE_ATTR(touchkey_idac3, S_IRUGO, touchkey_idac3_show, NULL);
static DEVICE_ATTR(touchkey_threshold, S_IRUGO, touchkey_threshold_show, NULL);
static DEVICE_ATTR(autocal_enable, S_IRUGO | S_IWUSR | S_IWGRP, NULL,
		   autocalibration_enable);
static DEVICE_ATTR(autocal_stat, S_IRUGO | S_IWUSR | S_IWGRP,
		   autocalibration_status, NULL);
#endif

static struct attribute *touchkey_attributes[] = {
	&dev_attr_recommended_version.attr,
	&dev_attr_updated_version.attr,
	&dev_attr_brightness.attr,
	&dev_attr_touchkey_menu.attr,
	&dev_attr_touchkey_back.attr,
	&dev_attr_touch_sensitivity.attr,
#if defined(TK_HAS_FIRMWARE_UPDATE)
	&dev_attr_touchkey_firm_update.attr,
	&dev_attr_touchkey_firm_update_status.attr,
#endif	
	&dev_attr_touchkey_firm_version_phone.attr,
	&dev_attr_touchkey_firm_version_panel.attr,
	&dev_attr_touchkey_autocal_start.attr,
#if defined(TK_HAS_AUTOCAL)
	&dev_attr_touchkey_raw_data0.attr,
	&dev_attr_touchkey_raw_data1.attr,
	&dev_attr_touchkey_raw_data2.attr,
	&dev_attr_touchkey_raw_data3.attr,
	&dev_attr_touchkey_idac0.attr,
	&dev_attr_touchkey_idac1.attr,
	&dev_attr_touchkey_idac2.attr,
	&dev_attr_touchkey_idac3.attr,
	&dev_attr_touchkey_threshold.attr,
	&dev_attr_autocal_enable.attr,
	&dev_attr_autocal_stat.attr,
#endif
	NULL,
};

static struct attribute_group touchkey_attr_group = {
	.attrs = touchkey_attributes,
};

static irqreturn_t touchkey_irq_handler(int irq, void *dev_id)
{

	printk("[Touchkey] %s, %d\n", __func__, __LINE__ );

	return IRQ_WAKE_THREAD;
}



extern struct class *sec_class;
static int i2c_touchkey_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
//	struct touchkey_platform_data *pdata = client->dev.platform_data;
	struct touchkey_i2c *tkey_i2c;

	struct input_dev *input_dev;
	int err = 0;
	int i;
	int ret;

	printk("[TouchKey] i2c_touchkey_probe\n");

/*	if (pdata == NULL) {
		printk(KERN_ERR "%s: no pdata\n", __func__);
		return -ENODEV;
	}*/

	/*Check I2C functionality */
	ret = i2c_check_functionality(client->adapter, I2C_FUNC_I2C);
	if (ret == 0) {
		printk(KERN_ERR "[Touchkey] No I2C functionality found\n");
		ret = -ENODEV;
		return ret;
	}

	/*Obtain kernel memory space for touchkey i2c */
	tkey_i2c = kzalloc(sizeof(struct touchkey_i2c), GFP_KERNEL);
	if (NULL == tkey_i2c) {
		printk(KERN_ERR "[Touchkey] failed to allocate tkey_i2c.\n");
		return -ENOMEM;
	}

	input_dev = input_allocate_device();

	if (!input_dev) {
		printk(KERN_ERR"[Touchkey] failed to allocate input device\n");
		kfree(tkey_i2c);
		return -ENOMEM;
	}

	input_dev->name = "sec_touchkey";
	input_dev->phys = "sec_touchkey/input0";
	input_dev->id.bustype = BUS_HOST;
	input_dev->dev.parent = &client->dev;

	/*tkey_i2c*/
//	tkey_i2c->pdata = pdata;
	tkey_i2c->input_dev = input_dev;
	tkey_i2c->client = client;

	i2c_set_clientdata(client, tkey_i2c);
	
	tkey_i2c->irq = client->irq;
	tkey_i2c->name = "sec_touchkey";

	set_bit(EV_SYN, input_dev->evbit);
	set_bit(EV_LED, input_dev->evbit);
	set_bit(LED_MISC, input_dev->ledbit);
	set_bit(EV_KEY, input_dev->evbit);

	for (i = 1; i < touchkey_count; i++)
		set_bit(touchkey_keycode[i], input_dev->keybit);

	input_set_drvdata(input_dev, tkey_i2c);

	printk("[TouchKey]: tkey_i2c->irq : %d\n", tkey_i2c->irq);


	ret = input_register_device(input_dev);
	if (ret) {
		printk(KERN_ERR"[Touchkey] failed to register input device\n");
		input_free_device(input_dev);
		kfree(tkey_i2c);
		return err;
	}
#if defined(TK_HAS_FIRMWARE_UPDATE)
	INIT_WORK(&tkey_i2c->update_work, touchkey_update_func);
#endif

#if defined(CONFIG_S2VE_TOUCHKEY_REV05)
	Touchkey_PowerOnOff(1);
#else
	touchkey_enable = 1;

	touchkey_led_on(tkey_i2c, 1);
	msleep(50);
#endif

	/*sysfs*/
	tkey_i2c->dev = device_create(sec_class, NULL, 0, NULL, "sec_touchkey");

	if (IS_ERR(tkey_i2c->dev)) {
		printk("Failed to create device(tkey_i2c->dev)!\n");
		input_unregister_device(input_dev);
	} else {
		dev_set_drvdata(tkey_i2c->dev, tkey_i2c);
		ret = sysfs_create_group(&tkey_i2c->dev->kobj,
					&touchkey_attr_group);
		if (ret) {
			printk(	"[TouchKey]: failed to create sysfs group\n");
		}
	}

	ret = request_threaded_irq(tkey_i2c->irq, NULL /*touchkey_irq_handler*/, touchkey_interrupt,
				IRQF_DISABLED | IRQF_TRIGGER_FALLING /* | IRQF_ONESHOT */, tkey_i2c->name, tkey_i2c);
	if (ret < 0) {
		printk("[Touchkey]: failed to request irq(%d) - %d\n", tkey_i2c->irq, ret);
		input_unregister_device(input_dev);
		return -EBUSY;
	}

#ifdef CONFIG_HAS_EARLYSUSPEND
	tkey_i2c->early_suspend.suspend =
		(void *)sec_touchkey_early_suspend;
	tkey_i2c->early_suspend.resume =
		(void *)sec_touchkey_late_resume;
	register_early_suspend(&tkey_i2c->early_suspend);
#endif


#if defined(TK_HAS_FIRMWARE_UPDATE)
	ret = touchkey_firmware_update(tkey_i2c);
	if (ret < 0) {
		printk(KERN_ERR	"[Touchkey]: failed firmware updating process (%d)\n", ret);
		input_unregister_device(input_dev);
		return -EBUSY;
	}
#endif

#if defined(TK_HAS_AUTOCAL)
	touchkey_autocalibration(tkey_i2c);
#endif
	printk("[Touchkey]: %s Probe done! \n", __func__);
	set_touchkey_debug('K');
	return 0;
}


static const struct i2c_device_id sec_touchkey_id[] = {
	{"sec_touchkey", 0},
	{}
};

struct i2c_driver touchkey_i2c_driver = {
	.driver = {
		.name = "sec_touchkey",
	},
	.id_table = sec_touchkey_id,
	.probe = i2c_touchkey_probe,
};

static int __init touchkey_init(void)
{
	int ret = 0;
	int rc;

#ifdef TEST_JIG_MODE
	unsigned char get_touch = 0x40;
#endif

	printk(KERN_INFO "[TouchKey] %s\n", __func__ );

	rc = gpio_request(TOUCHKEY_INT, "touchkey_int");
	if (rc < 0) {
		printk(KERN_ERR "unable to request GPIO pin %d\n", TOUCHKEY_INT);
	}
	
	gpio_direction_input(TOUCHKEY_INT);

	gpio_direction_output( TOUCHKEY_SCL , 1 );

	gpio_direction_output( TOUCHKEY_SDA , 1 );

	ret = i2c_add_driver(&touchkey_i2c_driver);

	if (ret) 
	{
		printk(KERN_ERR "[TouchKey] registration failed, module not inserted.ret= %d\n", ret);
	}
#ifdef TEST_JIG_MODE
	i2c_touchkey_write(tkey_i2c->client, &get_touch, 1);
#endif
	return ret;
}

static void __exit touchkey_exit(void)
{
	printk("[TouchKey] %s\n", __func__);
	i2c_del_driver(&touchkey_i2c_driver);
}

late_initcall(touchkey_init);
module_exit(touchkey_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("@@@");
MODULE_DESCRIPTION("touch keypad");

