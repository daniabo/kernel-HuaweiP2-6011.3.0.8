/*
 * Copyright (c) 2012 Synaptics Incorporated
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

#define DEBUG

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/ihex.h>
#include <linux/kernel.h>
#include<linux/moduleparam.h>
#include <linux/rmi.h>
#include <linux/time.h>
#include "rmi_driver.h"
#include "rmi_f01.h"
#include "rmi_f34.h"
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/namei.h>
#include <linux/vmalloc.h>
#include <hsad/config_interface.h>

#define SKIP_767   1
#define NO_SKIP_767  0

#define HAS_BSR_MASK 0x20

#define CHECKSUM_OFFSET 0
#define BOOTLOADER_VERSION_OFFSET 0x07
#define IMAGE_SIZE_OFFSET 0x08
#define CONFIG_SIZE_OFFSET 0x0C
#define PRODUCT_ID_OFFSET 0x10
#define PRODUCT_ID_SIZE 10
#define PRODUCT_INFO_OFFSET 0x1E
#define PRODUCT_INFO_SIZE 2
#define CONFIG_ID_OFFSET 0xB100

#define F01_RESET_MASK 0x01

#define ENABLE_WAIT_US (300 * 1000)

#define TP_BOARDLOADER_ID_FIX 1321841
#define F01_BUID_ID_OFFSET 18  /*the reg addr of bootloader packet id*/
/** Image file V5, Option 0
 */
struct image_header {
	u32 checksum;
	unsigned int image_size;
	unsigned int config_size;
	unsigned char options;
	unsigned char bootloader_version;
	u8 product_id[RMI_PRODUCT_ID_LENGTH + 1];
	unsigned char product_info[PRODUCT_INFO_SIZE];
	u32 config_id;
};

/*add for fw_update begin */
static u32 ArchSwap32(u32 D) {
        return((D<<24)|((D<<8)&0x00FF0000)|((D>>8)&0x0000FF00)|(D>>24));
}
/*add for fw_update end */

static u32 extract_u32(const u8 *ptr)
{
	return (u32)ptr[0] +
		(u32)ptr[1] * 0x100 +
		(u32)ptr[2] * 0x10000 +
		(u32)ptr[3] * 0x1000000;
}

struct reflash_data {
	struct rmi_device *rmi_dev;
	struct pdt_entry *f01_pdt;
	union f01_basic_queries f01_queries;
	u8 product_id[RMI_PRODUCT_ID_LENGTH+1];
	struct pdt_entry *f34_pdt;
	u8 bootloader_id[2];
	union f34_query_regs f34_queries;
	union f34_control_status f34_controls;
	const u8 *firmware_data;
	const u8 *config_data;
	u32 config_id;//add for fw_update
};

/* If this parameter is true, we will update the firmware regardless of
 * the versioning info.
 */
static bool force = 1;
module_param(force, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(param, "Force reflash of RMI4 devices");

/* If this parameter is not NULL, we'll use that name for the firmware image,
 * instead of getting it from the F01 queries.
 */
static char *img_name = "TM2451-001";   // for p2
static char *fw_water_proof = "TM2561-001";   // for D2-docomo
static char *bt_img_name = "TM2450-000";
module_param(img_name, charp, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(param, "Name of the RMI4 firmware image");

#define RMI4_IMAGE_FILE_REV1_OFFSET 30
#define RMI4_IMAGE_FILE_REV2_OFFSET 31
#define IMAGE_FILE_CHECKSUM_SIZE 4
#define FIRMWARE_IMAGE_AREA_OFFSET 0x100

static void extract_header(const u8 *data, int pos, struct image_header *header)
{
	header->checksum = extract_u32(&data[pos + CHECKSUM_OFFSET]);
	header->bootloader_version = data[pos + BOOTLOADER_VERSION_OFFSET];
	header->image_size = extract_u32(&data[pos + IMAGE_SIZE_OFFSET]);
	header->config_size = extract_u32(&data[pos + CONFIG_SIZE_OFFSET]);
	memcpy(header->product_id, &data[pos + PRODUCT_ID_OFFSET],
	       RMI_PRODUCT_ID_LENGTH);
	header->product_id[PRODUCT_ID_SIZE] = 0;
	/*add add for fw_update begin */
	header->config_id = extract_u32(&data[pos + CONFIG_ID_OFFSET]);
	/*add add for fw_update begin */
}

static int rescan_pdt(struct reflash_data *data)
{
	int retval;
	bool f01_found;
	bool f34_found;
	struct pdt_entry pdt_entry;
	int i;
	struct rmi_device *rmi_dev = data->rmi_dev;
	struct pdt_entry *f34_pdt = data->f34_pdt;
	struct pdt_entry *f01_pdt = data->f01_pdt;

	/* Per spec, once we're in reflash we only need to look at the first
	 * PDT page for potentially changed F01 and F34 information.
	 */

	for (i = PDT_START_SCAN_LOCATION; i >= PDT_END_SCAN_LOCATION;
			i -= sizeof(pdt_entry)) {
		retval = rmi_read_block(rmi_dev, i, (u8 *)&pdt_entry,
					sizeof(pdt_entry));
		if (retval != sizeof(pdt_entry)) {
			dev_err(&rmi_dev->dev,
				"Read PDT entry at %#06x failed: %d.\n",
				i, retval);
			return retval;
		}

		if (RMI4_END_OF_PDT(pdt_entry.function_number))
			break;

		if (pdt_entry.function_number == 0x01) {
			memcpy(f01_pdt, &pdt_entry, sizeof(pdt_entry));
			f01_found = true;
		} else if (pdt_entry.function_number == 0x34) {
			memcpy(f34_pdt, &pdt_entry, sizeof(pdt_entry));
			f34_found = true;
		}
	}

	if (!f01_found) {
		dev_err(&rmi_dev->dev, "Failed to find F01 PDT entry.\n");
		retval = -ENODEV;
	} else if (!f34_found) {
		dev_err(&rmi_dev->dev, "Failed to find F34 PDT entry.\n");
		retval = -ENODEV;
	} else
		retval = 0;

	return retval;
}

static int read_f34_controls(struct reflash_data *data)
{
	int retval;

	retval = rmi_read(data->rmi_dev, data->f34_controls.address,
			  data->f34_controls.regs);
	if (retval < 0)
		return retval;
	//dev_info(&data->rmi_dev->dev, "Last F34 status byte: %#04x\n", data->f34_controls.regs[0]);
	return 0;
}

static int read_f01_status(struct reflash_data *data,
			   union f01_device_status *device_status)
{
	int retval;

	retval = rmi_read(data->rmi_dev, data->f01_pdt->data_base_addr,
			  device_status->regs);
	if (retval < 0)
		return retval;
	dev_info(&data->rmi_dev->dev, "Last F01 status byte: %#04x\n", device_status->regs[0]);
	return 0;
}

#define MIN_SLEEP_TIME_US 50
#define MAX_SLEEP_TIME_US 100

/* Wait until the status is idle and we're ready to continue */
static int wait_for_idle(struct reflash_data *data, int timeout_ms)
{
	int timeout_count = ((timeout_ms * 1000) / MAX_SLEEP_TIME_US) + 1;
	int count = 0;
	union f34_control_status *controls = &data->f34_controls;
	int retval;

	do {
		if (count || timeout_count == 1)
			usleep_range(MIN_SLEEP_TIME_US, MAX_SLEEP_TIME_US);
		retval = read_f34_controls(data);
		count++;
		if (retval < 0)
			continue;
		else if (IS_IDLE(controls))
			return 0;
	} while (count < timeout_count);

	dev_err(&data->rmi_dev->dev,
		"ERROR: Timeout waiting for idle status, last status: %#04x.\n",
		controls->regs[0]);
	dev_err(&data->rmi_dev->dev, "Command: %#04x\n", controls->command);
	dev_err(&data->rmi_dev->dev, "Status:  %#04x\n", controls->status);
	dev_err(&data->rmi_dev->dev, "Enabled: %d\n",
			controls->program_enabled);
	dev_err(&data->rmi_dev->dev, "Idle:    %d\n", IS_IDLE(controls));
	return -ETIMEDOUT;
}


static int read_f01_queries(struct reflash_data *data)
{
	int retval;
	u16 addr = data->f01_pdt->query_base_addr;

	retval = rmi_read_block(data->rmi_dev, addr, data->f01_queries.regs,
				ARRAY_SIZE(data->f01_queries.regs));
	if (retval < 0) {
		dev_err(&data->rmi_dev->dev,
			"Failed to read F34 queries (code %d).\n", retval);
		return retval;
	}
	addr += ARRAY_SIZE(data->f01_queries.regs);

	retval = rmi_read_block(data->rmi_dev, addr, data->product_id,
				RMI_PRODUCT_ID_LENGTH);
	if (retval < 0) {
		dev_err(&data->rmi_dev->dev,
			"Failed to read product ID (code %d).\n", retval);
		return retval;
	}
	data->product_id[RMI_PRODUCT_ID_LENGTH] = 0;
	dev_info(&data->rmi_dev->dev, "F01 Product id:   %s\n",
			data->product_id);
	dev_info(&data->rmi_dev->dev, "F01 product info: %#04x %#04x\n",
			data->f01_queries.productinfo_1,
			data->f01_queries.productinfo_2);

	return 0;
}

static int read_f34_queries(struct reflash_data *data)
{
	int retval;
	u8 id_str[3];

	retval = rmi_read_block(data->rmi_dev, data->f34_pdt->query_base_addr,
				data->bootloader_id, 2);
	if (retval < 0) {
		dev_err(&data->rmi_dev->dev,
			"Failed to read F34 bootloader_id (code %d).\n",
			retval);
		return retval;
	}
	retval = rmi_read_block(data->rmi_dev, data->f34_pdt->query_base_addr+2,
			data->f34_queries.regs,
			ARRAY_SIZE(data->f34_queries.regs));
	if (retval < 0) {
		dev_err(&data->rmi_dev->dev,
			"Failed to read F34 queries (code %d).\n", retval);
		return retval;
	}
	data->f34_queries.block_size =
			le16_to_cpu(data->f34_queries.block_size);
	data->f34_queries.fw_block_count =
			le16_to_cpu(data->f34_queries.fw_block_count);
	data->f34_queries.config_block_count =
			le16_to_cpu(data->f34_queries.config_block_count);
	id_str[0] = data->bootloader_id[0];
	id_str[1] = data->bootloader_id[1];
	id_str[2] = 0;
#ifdef DEBUG
	dev_info(&data->rmi_dev->dev, "Got F34 data->f34_queries.\n");
	dev_info(&data->rmi_dev->dev, "F34 bootloader id: %s (%#04x %#04x)\n",
		 id_str, data->bootloader_id[0], data->bootloader_id[1]);
	dev_info(&data->rmi_dev->dev, "F34 has config id: %d\n",
		 data->f34_queries.has_config_id);
	dev_info(&data->rmi_dev->dev, "F34 unlocked:      %d\n",
		 data->f34_queries.unlocked);
	dev_info(&data->rmi_dev->dev, "F34 regMap:        %d\n",
		 data->f34_queries.reg_map);
	dev_info(&data->rmi_dev->dev, "F34 block size:    %d\n",
		 data->f34_queries.block_size);
	dev_info(&data->rmi_dev->dev, "F34 fw blocks:     %d\n",
		 data->f34_queries.fw_block_count);
	dev_info(&data->rmi_dev->dev, "F34 config blocks: %d\n",
		 data->f34_queries.config_block_count);
#endif

	data->f34_controls.address = data->f34_pdt->data_base_addr +
			F34_BLOCK_DATA_OFFSET + data->f34_queries.block_size;

	return 0;
}

static int write_bootloader_id(struct reflash_data *data)
{
	int retval;
	struct rmi_device *rmi_dev = data->rmi_dev;
	struct pdt_entry *f34_pdt = data->f34_pdt;

	retval = rmi_write_block(rmi_dev,
			f34_pdt->data_base_addr + F34_BLOCK_DATA_OFFSET,
			data->bootloader_id, ARRAY_SIZE(data->bootloader_id));
	if (retval < 0) {
		dev_err(&rmi_dev->dev,
			"Failed to write bootloader ID. Code: %d.\n", retval);
		return retval;
	}

	return 0;
}

static int write_f34_command(struct reflash_data *data, u8 command)
{
	int retval;
	struct rmi_device *rmi_dev = data->rmi_dev;

	retval = rmi_write(rmi_dev, data->f34_controls.address, command);
	if (retval < 0) {
		dev_err(&rmi_dev->dev,
			"Failed to write F34 command %#04x. Code: %d.\n",
			command, retval);
		return retval;
	}

	return 0;
}

static int enter_flash_programming(struct reflash_data *data)
{
	int retval;
	union f01_device_status device_status;
	struct rmi_device *rmi_dev = data->rmi_dev;

	retval = write_bootloader_id(data);
	if (retval < 0)
		return retval;

	dev_info(&rmi_dev->dev, "Enabling flash programming.\n");
	retval = write_f34_command(data, F34_ENABLE_FLASH_PROG);
	if (retval < 0)
		return retval;

	retval = wait_for_idle(data, F34_ENABLE_WAIT_MS);
	if (retval) {
		dev_err(&rmi_dev->dev, "Did not reach idle state after %d ms. Code: %d.\n",
			F34_ENABLE_WAIT_MS, retval);
		return retval;
	}
	/*add for fw_update begin*/
	msleep(500);
	read_f34_controls(data);
	/*add for fw_update end*/
	if (!data->f34_controls.program_enabled) {
		dev_err(&rmi_dev->dev, "Reached idle, but programming not enabled (current status register: %#04x.\n", data->f34_controls.regs[0]);
		read_f01_status(data, &device_status);
		dev_info(&rmi_dev->dev, "Rereading F34 status.");
		read_f34_controls(data);
		return -EINVAL;
	}
	dev_info(&rmi_dev->dev, "HOORAY! Programming is enabled!\n");

	retval = rescan_pdt(data);
	if (retval) {
		dev_err(&rmi_dev->dev, "Failed to rescan pdt.  Code: %d.\n",
			retval);
		return retval;
	}

	retval = read_f01_status(data, &device_status);
	if (retval) {
		dev_err(&rmi_dev->dev, "Failed to read F01 status after enabling reflash. Code: %d.\n",
			retval);
		return retval;
	}
	if (!(device_status.flash_prog)) {
		dev_err(&rmi_dev->dev, "Device reports as not in flash programming mode.\n");
		return -EINVAL;
	}

	retval = read_f34_queries(data);
	if (retval) {
		dev_err(&rmi_dev->dev, "F34 queries failed, code = %d.\n",
			retval);
		return retval;
	}

	return retval;
}

static void reset_device(struct reflash_data *data)
{
	int retval;
	struct rmi_device_platform_data *pdata =
		to_rmi_platform_data(data->rmi_dev);

	dev_info(&data->rmi_dev->dev, "Resetting...\n");
	retval = rmi_write(data->rmi_dev, data->f01_pdt->command_base_addr,
			   F01_RESET_MASK);
	if (retval < 0)
		dev_warn(&data->rmi_dev->dev,
			 "WARNING - post-flash reset failed, code: %d.\n",
			 retval);
	msleep(pdata->reset_delay_ms);
	dev_info(&data->rmi_dev->dev, "Reset completed.\n");
}

/*
 * Send data to the device one block at a time.
 */
static int write_blocks(struct reflash_data *data, u8 *block_ptr,
			u16 block_count, u8 cmd, bool skip_767_enable)
{
	int block_num;
	u8 zeros[] = {0, 0};
	int retval;
	u16 addr = data->f34_pdt->data_base_addr + F34_BLOCK_DATA_OFFSET;

	retval = rmi_write_block(data->rmi_dev, data->f34_pdt->data_base_addr,
				 zeros, ARRAY_SIZE(zeros));
	if (retval < 0) {
		dev_err(&data->rmi_dev->dev, "Failed to write initial zeros. Code=%d.\n",
			retval);
		return retval;
	}

	for (block_num = 0; block_num < block_count; ++block_num) {

		if (skip_767_enable && (block_num==767)) {
			dev_info(&data->rmi_dev->dev, "skip 767 block write.\n");
			block_num++;
			zeros[0] = 0x00;
			zeros[1] = 0x03;
			rmi_write_block(data->rmi_dev, data->f34_pdt->data_base_addr, zeros, ARRAY_SIZE(zeros));
			block_ptr += data->f34_queries.block_size;
		}

		retval = rmi_write_block(data->rmi_dev, addr, block_ptr,
					 data->f34_queries.block_size);
		if (retval < 0) {
			dev_err(&data->rmi_dev->dev, "Failed to write block %d. Code=%d.\n",
				block_num, retval);
			return retval;
		}

		retval = write_f34_command(data, cmd);
		if (retval) {
			dev_err(&data->rmi_dev->dev, "Failed to write command for block %d. Code=%d.\n",
				block_num, retval);
			return retval;
		}

		retval = wait_for_idle(data, F34_IDLE_WAIT_MS);
		if (retval) {
			dev_err(&data->rmi_dev->dev, "Failed to go idle after writing block %d. Code=%d.\n",
				block_num, retval);
			return retval;
		}

		block_ptr += data->f34_queries.block_size;
	}

	return 0;
}

static int write_firmware(struct reflash_data *data, bool skip_767_enable)
{
	return write_blocks(data, (u8 *) data->firmware_data,
		data->f34_queries.fw_block_count, F34_WRITE_FW_BLOCK, skip_767_enable);
}

static int write_configuration(struct reflash_data *data)
{
	return write_blocks(data, (u8 *) data->config_data,
		data->f34_queries.config_block_count, F34_WRITE_CONFIG_BLOCK, NO_SKIP_767);
}

static int get_tp_info(struct rmi_device *rmi_dev, struct reflash_data *data)
{
 	union pdt_properties pdt_props;
	int retval;
 	retval = rmi_read(rmi_dev, PDT_PROPERTIES_LOCATION, pdt_props.regs);
	if (retval < 0) {
		dev_warn(&rmi_dev->dev,
			 "Failed to read PDT props at %#06x (code %d). Assuming 0x00.\n",
			 PDT_PROPERTIES_LOCATION, retval);
	}
	if (pdt_props.has_bsr) {
		dev_warn(&rmi_dev->dev,
			 "Firmware update for LTS not currently supported.\n");
		return -1;
	}
	retval = read_f01_queries(data);
	if (retval) {
		dev_err(&rmi_dev->dev, "F01 queries failed, code = %d.\n",
			retval);
		return -1;
	}
	retval = read_f34_queries(data);
	if (retval) {
		dev_err(&rmi_dev->dev, "F34 queries failed, code = %d.\n",
			retval);
		return -1;
	}

	return 0;
}

static int get_bootloader_info(struct reflash_data *data,unsigned int  *bootloader_id)
{
	u8 build_id[3];
	int retval;

	retval = rmi_read_block(data->rmi_dev,
				data->f01_pdt->query_base_addr + F01_BUID_ID_OFFSET,
				build_id, sizeof(build_id));
	if (retval < sizeof(build_id)) {
		dev_err(&data->rmi_dev->dev, "BLpackrat read failed, code = %d.\n",
			retval);
		return -1;
	}
	*bootloader_id = (unsigned int)build_id[0] +
						(unsigned int)build_id[1] * 0x100 +
						(unsigned int)build_id[2] * 0x10000;
	dev_info(&data->rmi_dev->dev, "the current bootloader id : %d\n", *bootloader_id);
	return 0;
}

static int dload_fw_in_bootloader(struct reflash_data *data,bool skip_767_enable)
{
#ifdef DEBUG
	struct timespec start;
	struct timespec end;
	s64 duration_ns;
#endif
        int retval;

        /*write bootloader id() should be called before sending erase all command.*/
	retval = write_bootloader_id(data);
	if (retval)
	{
                dev_err(&data->rmi_dev->dev,"%s: write_bootloader_id error\n", __func__);
		return -1;
	}
	retval = write_f34_command(data, F34_ERASE_ALL);
	if (retval) {
                dev_err(&data->rmi_dev->dev, "%s: write_f34_command error\n", __func__);
		return -1;
	}
	retval = wait_for_idle(data, F34_ERASE_WAIT_MS);
	if (retval) {
		dev_err(&data->rmi_dev->dev,
			"Failed to reach idle state. Code: %d.\n", retval);
		return -1;
	}
#ifdef	DEBUG
	getnstimeofday(&end);
	duration_ns = timespec_to_ns(&end) - timespec_to_ns(&start);
	dev_info(&data->rmi_dev->dev,
		 "Erase complete, time: %lld ns.\n", duration_ns);
#endif

	if (data->firmware_data) {
#ifdef	DEBUG
		dev_info(&data->rmi_dev->dev, "Writing firmware...\n");
		getnstimeofday(&start);
#endif
		retval = write_firmware(data,skip_767_enable);
		if (retval) {
			dev_err(&data->rmi_dev->dev, "%s: write_firmware error\n", __func__);
			return -1;
		}
#ifdef	DEBUG
		getnstimeofday(&end);
		duration_ns = timespec_to_ns(&end) - timespec_to_ns(&start);
		dev_info(&data->rmi_dev->dev,
			 "Done writing FW, time: %lld ns.\n", duration_ns);
#endif
	}

	if (data->config_data) {
#ifdef	DEBUG
		dev_info(&data->rmi_dev->dev, "Writing configuration...\n");
		getnstimeofday(&start);
#endif
		retval = write_configuration(data);
		if (retval) {
			dev_err(&data->rmi_dev->dev, "%s: write_configuration error\n", __func__);
			return -1;
		}
#ifdef	DEBUG
		getnstimeofday(&end);
		duration_ns = timespec_to_ns(&end) - timespec_to_ns(&start);
		dev_info(&data->rmi_dev->dev,
			 "Done writing config, time: %lld ns.\n", duration_ns);
#endif
	}

	dev_info(&data->rmi_dev->dev, "%s suc\n", __func__);
	return 0;
}

static int dload_temp_firmware(struct rmi_device *rmi_dev,struct reflash_data *data)
{
	int retval;
	u16 test_addr ;
	u8 test_configid[4];
	const struct firmware *temp_fw_entry = NULL;
	char temp_firmware_name[RMI_PRODUCT_ID_LENGTH + 24] ={0};
	struct image_header temp_fw_header;

	dev_info(&rmi_dev->dev, "%s called\n",__func__);

	//request firmware and do reflash here
	snprintf(temp_firmware_name, sizeof(temp_firmware_name), "rmi4/BLL/%s.img",bt_img_name);

	test_addr = data->f34_pdt->control_base_addr;
	rmi_read_block(rmi_dev,test_addr,test_configid,4);
	data->config_id = extract_u32(&test_configid);

	retval = request_firmware(&temp_fw_entry, temp_firmware_name, &rmi_dev->dev);
	if (retval != 0) {
		dev_err(&rmi_dev->dev, "Firmware %s not available, code = %d\n",
			temp_firmware_name, retval);
		return -1;
	}

	extract_header(temp_fw_entry->data, 0, &temp_fw_header);

	if (temp_fw_header.image_size) {
		data->firmware_data = temp_fw_entry->data + F34_FW_IMAGE_OFFSET;
	}
	if (temp_fw_header.config_size) {
		data->config_data = temp_fw_entry->data + F34_FW_IMAGE_OFFSET +
		temp_fw_header.image_size;
	}

	if (dload_fw_in_bootloader(data, SKIP_767) < 0) {
		if(temp_fw_entry) {
			release_firmware(temp_fw_entry);
		}
		dev_err(&rmi_dev->dev, "%s:dload_fw_in_bootloader error\n", __func__);
		return -1;
	}

	if (temp_fw_entry) {
		release_firmware(temp_fw_entry);
	}
	return 0;
}

static int dload_final_firmware(struct rmi_device *rmi_dev,struct reflash_data *data,
                                                                        const struct firmware *fw_entry,const struct image_header *header)
{
	u16 test_addr ;
	u8 test_configid[4];

	dev_info(&data->rmi_dev->dev,"%s called\n",__func__);

	test_addr = data->f34_pdt->control_base_addr;
	rmi_read_block(rmi_dev, test_addr, test_configid, 4);
	data->config_id = extract_u32(&test_configid);

	if (header->image_size)
		data->firmware_data = fw_entry->data + F34_FW_IMAGE_OFFSET;
	if (header->config_size)
		data->config_data = fw_entry->data + F34_FW_IMAGE_OFFSET +header->image_size;

	if (dload_fw_in_bootloader(data, NO_SKIP_767) < 0) {
		dev_err(&rmi_dev->dev, "%s:dload_fw_in_bootloader error\n", __func__);
		return -1;
	}
	return 0;
}

static int goto_bootloader(struct reflash_data *data)
{
        int retval;
        union f01_device_status device_status;
        /*add read out current operating mode to decide if we need  issue comand to enter BL mode*/
        retval = read_f34_controls(data);
        if (data->f34_controls.program_enabled) {
		dev_info(&data->rmi_dev->dev, "Device has been in bootloader mode!\n");
		retval = read_f01_status(data, &device_status);
		if (retval) {
			dev_err(&data->rmi_dev->dev, "Failed to read F01 status after enabling reflash. Code: %d.\n",
				retval);
			return retval;
		}
		if (!(device_status.flash_prog)) {
			dev_err(&data->rmi_dev->dev, "Device reports as not in flash programming mode.\n");
			return -EINVAL;
		}
        } else {
		retval = enter_flash_programming(data);
		if (retval) {
	                dev_err(&data->rmi_dev->dev, "%s: enter_flash_programming error\n", __func__);
			return -1;
		}
        }
        return 0;
}

static void reflash_firmware(struct rmi_device *rmi_dev,struct reflash_data *data,
                                                            const struct firmware *fw_entry,const struct image_header *header)
{
#ifdef DEBUG
	struct timespec start;
	struct timespec end;
	s64 duration_ns;
#endif
	int retval = 0;
	unsigned int bootloader_id = 0;

	if (goto_bootloader(data) < 0) {
		dev_info(&data->rmi_dev->dev, "goto_bootloader error\n");
		return;
	}

#ifdef	DEBUG
	dev_info(&data->rmi_dev->dev, "Erasing FW...\n");
	getnstimeofday(&start);
#endif

        /*read out the bootloader packet id*/
         if (get_bootloader_info(data,&bootloader_id) < 0) {
		dev_info(&data->rmi_dev->dev, "Get bootloader packrat fail...\n");
		return;
         }

	 dev_info(&data->rmi_dev->dev, "Current ASIC BL packrat is %d...\n", bootloader_id);

        /*need to update a temper firmware at first and then reboot the system*/
	if (bootloader_id < TP_BOARDLOADER_ID_FIX) { //add a retry to reflash the BLL 10 times harry
		if (dload_temp_firmware(rmi_dev,data) < 0) {
			dev_err(&rmi_dev->dev, "%s:Failed to dload_temp_firmware\n", __func__);
			return;
		}
		reset_device(data);
		dev_info(&data->rmi_dev->dev, "sleep 5 secs...\n");
		msleep(5000);

		dev_info(&data->rmi_dev->dev, "rescan_pdt...\n");
		retval = rescan_pdt(data);
		if (retval) {
			dev_err(&rmi_dev->dev, "Failed to rescan pdt.  Code: %d.\n", retval);
			return;
		}
		if (get_tp_info(rmi_dev,data) < 0) {
			dev_err(&rmi_dev->dev, "%s: Failed to get_tp_info\n", __func__);
			return;
		}
		if (goto_bootloader(data) < 0) {
			dev_err(&rmi_dev->dev, "%s: goto_bootloader error.\n", __func__);
			return;
		}
		 if (get_bootloader_info(data,&bootloader_id) < 0) {
			dev_err(&rmi_dev->dev, "%s: get_bootloader_info error.\n", __func__);
			return;
		 }
		if (bootloader_id < TP_BOARDLOADER_ID_FIX) {
			dev_err(&data->rmi_dev->dev, "error: bootloader_id[%d]< 1321841\n",bootloader_id);
			return;
		}
	}

	if (dload_final_firmware(rmi_dev,data,fw_entry,header) < 0) {
		dev_err(&data->rmi_dev->dev, "%s: dload_final_firmware error\n", __func__);
	}

}

/* Returns false if the firmware should not be reflashed.
 */
 /*modified for fw_update begin */
#if 0
static bool go_nogo(struct reflash_data *data, struct image_header *header)
{
	union f01_device_status device_status;
	int retval;

	if (data->f01_queries.productinfo_1 < header->product_info[0] ||
		data->f01_queries.productinfo_2 < header->product_info[1]) {
		dev_info(&data->rmi_dev->dev,
			 "FW product ID is older than image product ID.\n");
		return true;
	}

	retval = read_f01_status(data, &device_status);
	if (retval)
		dev_err(&data->rmi_dev->dev,
			"Failed to read F01 status. Code: %d.\n", retval);

	return device_status.flash_prog || force;
}
#endif

static bool go_nogo(struct reflash_data *data, struct image_header *header)
{
	union f01_device_status device_status;
	int retval;

	if (ArchSwap32(data->config_id) != ArchSwap32(header->config_id)) {
		dev_info(&data->rmi_dev->dev,
			 "FW config ID is different from image config ID.\n");
		return true;
	}

	retval = read_f01_status(data, &device_status);
	if (retval)
		dev_err(&data->rmi_dev->dev,
			"Failed to read F01 status. Code: %d.\n", retval);

	return device_status.flash_prog ;
}
 /*modified for fw_update end */

void rmi4_fw_update(struct rmi_device *rmi_dev,
		struct pdt_entry *f01_pdt, struct pdt_entry *f34_pdt)
{
#ifdef DEBUG
	/*add for fw_update begin */
	u8 test_configid[4];
	u16 test_addr ;
	/*add for fw_update end */
	struct timespec start;
	struct timespec end;
	s64 duration_ns;
#endif
	char firmware_name[RMI_PRODUCT_ID_LENGTH + 12];
	const struct firmware *fw_entry = NULL;
	int retval;
	struct image_header header;
	struct reflash_data data = {
		.rmi_dev = rmi_dev,
		.f01_pdt = f01_pdt,
		.f34_pdt = f34_pdt,
	};
	int fw_type;

	dev_info(&rmi_dev->dev, "%s called.\n", __func__);
#ifdef	DEBUG
	getnstimeofday(&start);
#endif

	if (get_tp_info(rmi_dev,&data) < 0) {
		dev_err(&rmi_dev->dev, "Failed get_tp_info\n");
		return;
	}

	fw_type = get_touchscreen_fw_type();
	if (fw_type == E_TOUSCREEN_FW_GLOVE) {
		snprintf(firmware_name, sizeof(firmware_name), "rmi4/%s.img",
			img_name ? img_name : data.product_id);
	} else if(fw_type==E_TOUSCREEN_FW_WATER_PROOF) {
		snprintf(firmware_name, sizeof(firmware_name), "rmi4/%s.img", fw_water_proof);
	} else {
		dev_err(&rmi_dev->dev, "get_touchscreen_fw_type failed, fw_type=%d\n",
			retval, fw_type);
		return;
	}
	dev_info(&rmi_dev->dev, "firmware_name = %s\n", firmware_name);
#if 0
	dev_info(&rmi_dev->dev, "Requesting .\n");
	retval = file_open_firmware(&fw_entry,&fw_data);
       if (retval != 0) {
		dev_err(&rmi_dev->dev, "Firmware %s not available, code = %d\n",
			firmware_name, retval);
		return;
	}
#endif
#if 1
	retval = request_firmware(&fw_entry, firmware_name, &rmi_dev->dev);
	if (retval != 0) {
		dev_err(&rmi_dev->dev, "Firmware %s not available, code = %d\n",
			firmware_name, retval);
		return;
	}
#endif
	/*add for fw_update begin */
	test_addr = f34_pdt->control_base_addr;
	rmi_read_block(rmi_dev,test_addr,test_configid,4);
	data.config_id = extract_u32(&test_configid);
	/*add for fw_update end */

	extract_header(fw_entry->data, 0, &header);
#ifdef	DEBUG
	dev_info(&rmi_dev->dev, "Got firmware, size: %d.\n", fw_entry->size);
	dev_info(&rmi_dev->dev, "Img checksum:           %#08X\n",
		 header.checksum);
	dev_info(&rmi_dev->dev, "Img image size:         %d\n",
		 header.image_size);
	dev_info(&rmi_dev->dev, "Img config size:        %d\n",
		 header.config_size);
	dev_info(&rmi_dev->dev, "Img bootloader version: %d\n",
		 header.bootloader_version);
	dev_info(&rmi_dev->dev, "Img product id:         %s\n",
		 header.product_id);
	dev_info(&rmi_dev->dev, "Img product info:       %#04x %#04x\n",
		 header.product_info[0], header.product_info[1]);
	/*add for fw_update begin */
	dev_info(&rmi_dev->dev, "Config id : %x\n",ArchSwap32(data.config_id));
	dev_info(&rmi_dev->dev, "Img config id : %x\n",ArchSwap32(header.config_id));
	/*add for fw_update end */
#endif

	if (header.image_size)
		data.firmware_data = fw_entry->data + F34_FW_IMAGE_OFFSET;
	if (header.config_size)
		data.config_data = fw_entry->data + F34_FW_IMAGE_OFFSET +
			header.image_size;

	if (go_nogo(&data, &header)) {
		reflash_firmware(rmi_dev, &data, fw_entry, &header);
		reset_device(&data);	
	} else
		dev_info(&rmi_dev->dev, "Go/NoGo said don't reflash.\n");
	if (fw_entry)
	{
		release_firmware(fw_entry);
	}
#ifdef	DEBUG
	getnstimeofday(&end);
	duration_ns = timespec_to_ns(&end) - timespec_to_ns(&start);
	dev_info(&rmi_dev->dev, "Time to reflash: %lld ns.\n", duration_ns);
#endif
}
