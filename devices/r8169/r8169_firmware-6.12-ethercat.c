// SPDX-License-Identifier: GPL-2.0-only
/* r8169_firmware.c: RealTek 8169/8168/8101 ethernet driver.
 *
 * Copyright (c) 2002 ShuChen <shuchen@realtek.com.tw>
 * Copyright (c) 2003 - 2007 Francois Romieu <romieu@fr.zoreil.com>
 * Copyright (c) a lot of people too. Please respect their work.
 *
 * See MAINTAINERS file for support contact information.
 */

#include <linux/delay.h>
#include <linux/firmware.h>

#include "r8169_firmware-6.12-ethercat.h"

enum rtl_fw_opcode {
	PHY_READ		= 0x0,
	PHY_DATA_OR		= 0x1,
	PHY_DATA_AND		= 0x2,
	PHY_BJMPN		= 0x3,
	PHY_MDIO_CHG		= 0x4,
	PHY_CLEAR_READCOUNT	= 0x7,
	PHY_WRITE		= 0x8,
	PHY_READCOUNT_EQ_SKIP	= 0x9,
	PHY_COMP_EQ_SKIPN	= 0xa,
	PHY_COMP_NEQ_SKIPN	= 0xb,
	PHY_WRITE_PREVIOUS	= 0xc,
	PHY_SKIPN		= 0xd,
	PHY_DELAY_MS		= 0xe,
};

struct fw_info {
	u32	magic;
	char	version[RTL_VER_SIZE];
	__le32	fw_start;
	__le32	fw_len;
	u8	chksum;
} __packed;

#define FW_OPCODE_SIZE sizeof_field(struct rtl_fw_phy_action, code[0])

static bool rtl_fw_format_ok(struct rtl_fw *rtl_fw)
{
	const struct firmware *fw = rtl_fw->fw;
	struct fw_info *fw_info = (struct fw_info *)fw->data;
	struct rtl_fw_phy_action *pa = &rtl_fw->phy_action;

	if (fw->size < FW_OPCODE_SIZE)
		return false;

	if (!fw_info->magic) {
		size_t i, size, start;
		u8 checksum = 0;

		if (fw->size < sizeof(*fw_info))
			return false;

		for (i = 0; i < fw->size; i++)
			checksum += fw->data[i];
		if (checksum != 0)
			return false;

		start = le32_to_cpu(fw_info->fw_start);
		if (start > fw->size)
			return false;

		size = le32_to_cpu(fw_info->fw_len);
		if (size > (fw->size - start) / FW_OPCODE_SIZE)
			return false;

		strscpy(rtl_fw->version, fw_info->version, RTL_VER_SIZE);

		pa->code = (__le32 *)(fw->data + start);
		pa->size = size;
	} else {
		if (fw->size % FW_OPCODE_SIZE)
			return false;

		strscpy(rtl_fw->version, rtl_fw->fw_name, RTL_VER_SIZE);

		pa->code = (__le32 *)fw->data;
		pa->size = fw->size / FW_OPCODE_SIZE;
	}

	return true;
}

static bool rtl_fw_data_ok(struct rtl_fw *rtl_fw)
{
	struct rtl_fw_phy_action *pa = &rtl_fw->phy_action;
	size_t index;

	for (index = 0; index < pa->size; index++) {
		u32 action = le32_to_cpu(pa->code[index]);
		u32 val = action & 0x0000ffff;
		u32 regno = (action & 0x0fff0000) >> 16;

		switch (action >> 28) {
		case PHY_READ:
		case PHY_DATA_OR:
		case PHY_DATA_AND:
		case PHY_CLEAR_READCOUNT:
		case PHY_WRITE:
		case PHY_WRITE_PREVIOUS:
		case PHY_DELAY_MS:
			break;

		case PHY_MDIO_CHG:
			if (val > 1)
				goto out;
			break;

		case PHY_BJMPN:
			if (regno > index)
				goto out;
			break;
		case PHY_READCOUNT_EQ_SKIP:
			if (index + 2 >= pa->size)
				goto out;
			break;
		case PHY_COMP_EQ_SKIPN:
		case PHY_COMP_NEQ_SKIPN:
		case PHY_SKIPN:
			if (index + 1 + regno >= pa->size)
				goto out;
			break;

		default:
			dev_err(rtl_fw->dev, "Invalid action 0x%08x\n", action);
			return false;
		}
	}

	return true;
out:
	dev_err(rtl_fw->dev, "Out of range of firmware\n");
	return false;
}

void rtl_fw_write_firmware(struct rtl8169_private *tp, struct rtl_fw *rtl_fw)
{
	struct rtl_fw_phy_action *pa = &rtl_fw->phy_action;
	rtl_fw_write_t fw_write = rtl_fw->phy_write;
	rtl_fw_read_t fw_read = rtl_fw->phy_read;
	int predata = 0, count = 0;
	size_t index;

	for (index = 0; index < pa->size; index++) {
		u32 action = le32_to_cpu(pa->code[index]);
		u32 data = action & 0x0000ffff;
		u32 regno = (action & 0x0fff0000) >> 16;
		enum rtl_fw_opcode opcode = action >> 28;

		switch (opcode) {
		case PHY_READ:
			predata = fw_read(tp, regno);
			count++;
			break;
		case PHY_DATA_OR:
			predata |= data;
			break;
		case PHY_DATA_AND:
			predata &= data;
			break;
		case PHY_BJMPN:
			index -= (regno + 1);
			break;
		case PHY_MDIO_CHG:
			if (data) {
				fw_write = rtl_fw->mac_mcu_write;
				fw_read = rtl_fw->mac_mcu_read;
			} else {
				fw_write = rtl_fw->phy_write;
				fw_read = rtl_fw->phy_read;
			}

			break;
		case PHY_CLEAR_READCOUNT:
			count = 0;
			break;
		case PHY_WRITE:
			fw_write(tp, regno, data);
			break;
		case PHY_READCOUNT_EQ_SKIP:
			if (count == data)
				index++;
			break;
		case PHY_COMP_EQ_SKIPN:
			if (predata == data)
				index += regno;
			break;
		case PHY_COMP_NEQ_SKIPN:
			if (predata != data)
				index += regno;
			break;
		case PHY_WRITE_PREVIOUS:
			fw_write(tp, regno, predata);
			break;
		case PHY_SKIPN:
			index += regno;
			break;
		case PHY_DELAY_MS:
			msleep(data);
			break;
		}
	}
}

void rtl_fw_release_firmware(struct rtl_fw *rtl_fw)
{
	release_firmware(rtl_fw->fw);
}

int rtl_fw_request_firmware(struct rtl_fw *rtl_fw)
{
	int rc;

	rc = request_firmware(&rtl_fw->fw, rtl_fw->fw_name, rtl_fw->dev);
	if (rc < 0)
		goto out;

	if (!rtl_fw_format_ok(rtl_fw) || !rtl_fw_data_ok(rtl_fw)) {
		release_firmware(rtl_fw->fw);
		rc = -EINVAL;
		goto out;
	}

	return 0;
out:
	dev_err(rtl_fw->dev, "Unable to load firmware %s (%d)\n",
		rtl_fw->fw_name, rc);
	return rc;
}
