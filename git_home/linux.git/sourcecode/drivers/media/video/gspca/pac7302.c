/*
 * Pixart PAC7302 driver
 *
 * Copyright (C) 2008-2012 Jean-Francois Moine <http://moinejf.free.fr>
 * Copyright (C) 2005 Thomas Kaiser thomas@kaiser-linux.li
 *
 * Separated from Pixart PAC7311 library by Márton Németh
 * Camera button input handling by Márton Németh <nm127@freemail.hu>
 * Copyright (C) 2009-2010 Márton Németh <nm127@freemail.hu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/* Some documentation about various registers as determined by trial and error.

   Register page 1:

   Address	Description
   0x78		Global control, bit 6 controls the LED (inverted)

   Register page 3:

   Address	Description
   0x02		Clock divider 3-63, fps = 90 / val. Must be a multiple of 3 on
		the 7302, so one of 3, 6, 9, ..., except when between 6 and 12?
   0x03		Variable framerate ctrl reg2==3: 0 -> ~30 fps, 255 -> ~22fps
   0x04		Another var framerate ctrl reg2==3, reg3==0: 0 -> ~30 fps,
		63 -> ~27 fps, the 2 msb's must always be 1 !!
   0x05		Another var framerate ctrl reg2==3, reg3==0, reg4==0xc0:
		1 -> ~30 fps, 2 -> ~20 fps
   0x0e		Exposure bits 0-7, 0-448, 0 = use full frame time
   0x0f		Exposure bit 8, 0-448, 448 = no exposure at all
   0x10		Master gain 0-31
   0x21		Bitfield: 0-1 unused, 2-3 vflip/hflip, 4-5 unknown, 6-7 unused

   The registers are accessed in the following functions:

   Page | Register   | Function
   -----+------------+---------------------------------------------------
    0   | 0x0f..0x20 | setcolors()
    0   | 0xa2..0xab | setbrightcont()
    0   | 0xc5       | setredbalance()
    0   | 0xc6       | setwhitebalance()
    0   | 0xc7       | setbluebalance()
    0   | 0xdc       | setbrightcont(), setcolors()
    3   | 0x02       | setexposure()
    3   | 0x10       | setgain()
    3   | 0x11       | setcolors(), setgain(), setexposure(), sethvflip()
    3   | 0x21       | sethvflip()
*/

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/input.h>
#include <media/v4l2-chip-ident.h>
#include "gspca.h"
/* Include pac common sof detection functions */
#include "pac_common.h"

MODULE_AUTHOR("Jean-Francois Moine <http://moinejf.free.fr>, "
		"Thomas Kaiser thomas@kaiser-linux.li");
MODULE_DESCRIPTION("Pixart PAC7302");
MODULE_LICENSE("GPL");

enum e_ctrl {
	BRIGHTNESS,
	CONTRAST,
	COLORS,
	WHITE_BALANCE,
	RED_BALANCE,
	BLUE_BALANCE,
	GAIN,
	AUTOGAIN,
	EXPOSURE,
	VFLIP,
	HFLIP,
	NCTRLS		/* number of controls */
};

/* specific webcam descriptor for pac7302 */
struct sd {
	struct gspca_dev gspca_dev;		/* !! must be the first item */

	struct gspca_ctrl ctrls[NCTRLS];

	u8 flags;
#define FL_HFLIP 0x01		/* mirrored by default */
#define FL_VFLIP 0x02		/* vertical flipped by default */

	u8 sof_read;
	s8 autogain_ignore_frames;

	atomic_t avg_lum;
};

/* V4L2 controls supported by the driver */
static void setbrightcont(struct gspca_dev *gspca_dev);
static void setcolors(struct gspca_dev *gspca_dev);
static void setwhitebalance(struct gspca_dev *gspca_dev);
static void setredbalance(struct gspca_dev *gspca_dev);
static void setbluebalance(struct gspca_dev *gspca_dev);
static void setgain(struct gspca_dev *gspca_dev);
static void setexposure(struct gspca_dev *gspca_dev);
static void setautogain(struct gspca_dev *gspca_dev);
static void sethvflip(struct gspca_dev *gspca_dev);

static const struct ctrl sd_ctrls[] = {
[BRIGHTNESS] = {
	    {
		.id      = V4L2_CID_BRIGHTNESS,
		.type    = V4L2_CTRL_TYPE_INTEGER,
		.name    = "Brightness",
		.minimum = 0,
#define BRIGHTNESS_MAX 0x20
		.maximum = BRIGHTNESS_MAX,
		.step    = 1,
		.default_value = 0x10,
	    },
	    .set_control = setbrightcont
	},
[CONTRAST] = {
	    {
		.id      = V4L2_CID_CONTRAST,
		.type    = V4L2_CTRL_TYPE_INTEGER,
		.name    = "Contrast",
		.minimum = 0,
#define CONTRAST_MAX 255
		.maximum = CONTRAST_MAX,
		.step    = 1,
		.default_value = 127,
	    },
	    .set_control = setbrightcont
	},
[COLORS] = {
	    {
		.id      = V4L2_CID_SATURATION,
		.type    = V4L2_CTRL_TYPE_INTEGER,
		.name    = "Saturation",
		.minimum = 0,
#define COLOR_MAX 255
		.maximum = COLOR_MAX,
		.step    = 1,
		.default_value = 127
	    },
	    .set_control = setcolors
	},
[WHITE_BALANCE] = {
	    {
		.id      = V4L2_CID_WHITE_BALANCE_TEMPERATURE,
		.type    = V4L2_CTRL_TYPE_INTEGER,
		.name    = "White Balance",
		.minimum = 0,
		.maximum = 255,
		.step    = 1,
		.default_value = 4,
	    },
	    .set_control = setwhitebalance
	},
[RED_BALANCE] = {
	    {
		.id      = V4L2_CID_RED_BALANCE,
		.type    = V4L2_CTRL_TYPE_INTEGER,
		.name    = "Red",
		.minimum = 0,
		.maximum = 3,
		.step    = 1,
		.default_value = 1,
	    },
	    .set_control = setredbalance
	},
[BLUE_BALANCE] = {
	    {
		.id      = V4L2_CID_BLUE_BALANCE,
		.type    = V4L2_CTRL_TYPE_INTEGER,
		.name    = "Blue",
		.minimum = 0,
		.maximum = 3,
		.step    = 1,
		.default_value = 1,
	    },
	    .set_control = setbluebalance
	},
[GAIN] = {
	    {
		.id      = V4L2_CID_GAIN,
		.type    = V4L2_CTRL_TYPE_INTEGER,
		.name    = "Gain",
		.minimum = 0,
		.maximum = 255,
		.step    = 1,
#define GAIN_DEF 127
#define GAIN_KNEE 255 /* Gain seems to cause little noise on the pac73xx */
		.default_value = GAIN_DEF,
	    },
	    .set_control = setgain
	},
[EXPOSURE] = {
	    {
		.id      = V4L2_CID_EXPOSURE,
		.type    = V4L2_CTRL_TYPE_INTEGER,
		.name    = "Exposure",
		.minimum = 0,
		.maximum = 1023,
		.step    = 1,
#define EXPOSURE_DEF  66  /*  33 ms / 30 fps */
#define EXPOSURE_KNEE 133 /*  66 ms / 15 fps */
		.default_value = EXPOSURE_DEF,
	    },
	    .set_control = setexposure
	},
[AUTOGAIN] = {
	    {
		.id      = V4L2_CID_AUTOGAIN,
		.type    = V4L2_CTRL_TYPE_BOOLEAN,
		.name    = "Auto Gain",
		.minimum = 0,
		.maximum = 1,
		.step    = 1,
#define AUTOGAIN_DEF 1
		.default_value = AUTOGAIN_DEF,
	    },
	    .set_control = setautogain,
	},
[HFLIP] = {
	    {
		.id      = V4L2_CID_HFLIP,
		.type    = V4L2_CTRL_TYPE_BOOLEAN,
		.name    = "Mirror",
		.minimum = 0,
		.maximum = 1,
		.step    = 1,
		.default_value = 0,
	    },
	    .set_control = sethvflip,
	},
[VFLIP] = {
	    {
		.id      = V4L2_CID_VFLIP,
		.type    = V4L2_CTRL_TYPE_BOOLEAN,
		.name    = "Vflip",
		.minimum = 0,
		.maximum = 1,
		.step    = 1,
		.default_value = 0,
	    },
	    .set_control = sethvflip
	},
};

static const struct v4l2_pix_format vga_mode[] = {
	{640, 480, V4L2_PIX_FMT_PJPG, V4L2_FIELD_NONE,
		.bytesperline = 640,
		.sizeimage = 640 * 480 * 3 / 8 + 590,
		.colorspace = V4L2_COLORSPACE_JPEG,
	},
};

#define LOAD_PAGE3		255
#define END_OF_SEQUENCE		0

/* pac 7302 */
static const u8 init_7302[] = {
/*	index,value */
	0xff, 0x01,		/* page 1 */
	0x78, 0x00,		/* deactivate */
	0xff, 0x01,
	0x78, 0x40,		/* led off */
};
static const u8 start_7302[] = {
/*	index, len, [value]* */
	0xff, 1,	0x00,		/* page 0 */
	0x00, 12,	0x01, 0x40, 0x40, 0x40, 0x01, 0xe0, 0x02, 0x80,
			0x00, 0x00, 0x00, 0x00,
	0x0d, 24,	0x03, 0x01, 0x00, 0xb5, 0x07, 0xcb, 0x00, 0x00,
			0x07, 0xc8, 0x00, 0xea, 0x07, 0xcf, 0x07, 0xf7,
			0x07, 0x7e, 0x01, 0x0b, 0x00, 0x00, 0x00, 0x11,
	0x26, 2,	0xaa, 0xaa,
	0x2e, 1,	0x31,
	0x38, 1,	0x01,
	0x3a, 3,	0x14, 0xff, 0x5a,
	0x43, 11,	0x00, 0x0a, 0x18, 0x11, 0x01, 0x2c, 0x88, 0x11,
			0x00, 0x54, 0x11,
	0x55, 1,	0x00,
	0x62, 4,	0x10, 0x1e, 0x1e, 0x18,
	0x6b, 1,	0x00,
	0x6e, 3,	0x08, 0x06, 0x00,
	0x72, 3,	0x00, 0xff, 0x00,
	0x7d, 23,	0x01, 0x01, 0x58, 0x46, 0x50, 0x3c, 0x50, 0x3c,
			0x54, 0x46, 0x54, 0x56, 0x52, 0x50, 0x52, 0x50,
			0x56, 0x64, 0xa4, 0x00, 0xda, 0x00, 0x00,
	0xa2, 10,	0x22, 0x2c, 0x3c, 0x54, 0x69, 0x7c, 0x9c, 0xb9,
			0xd2, 0xeb,
	0xaf, 1,	0x02,
	0xb5, 2,	0x08, 0x08,
	0xb8, 2,	0x08, 0x88,
	0xc4, 4,	0xae, 0x01, 0x04, 0x01,
	0xcc, 1,	0x00,
	0xd1, 11,	0x01, 0x30, 0x49, 0x5e, 0x6f, 0x7f, 0x8e, 0xa9,
			0xc1, 0xd7, 0xec,
	0xdc, 1,	0x01,
	0xff, 1,	0x01,		/* page 1 */
	0x12, 3,	0x02, 0x00, 0x01,
	0x3e, 2,	0x00, 0x00,
	0x76, 5,	0x01, 0x20, 0x40, 0x00, 0xf2,
	0x7c, 1,	0x00,
	0x7f, 10,	0x4b, 0x0f, 0x01, 0x2c, 0x02, 0x58, 0x03, 0x20,
			0x02, 0x00,
	0x96, 5,	0x01, 0x10, 0x04, 0x01, 0x04,
	0xc8, 14,	0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00,
			0x07, 0x00, 0x01, 0x07, 0x04, 0x01,
	0xd8, 1,	0x01,
	0xdb, 2,	0x00, 0x01,
	0xde, 7,	0x00, 0x01, 0x04, 0x04, 0x00, 0x00, 0x00,
	0xe6, 4,	0x00, 0x00, 0x00, 0x01,
	0xeb, 1,	0x00,
	0xff, 1,	0x02,		/* page 2 */
	0x22, 1,	0x00,
	0xff, 1,	0x03,		/* page 3 */
	0, LOAD_PAGE3,			/* load the page 3 */
	0x11, 1,	0x01,
	0xff, 1,	0x02,		/* page 2 */
	0x13, 1,	0x00,
	0x22, 4,	0x1f, 0xa4, 0xf0, 0x96,
	0x27, 2,	0x14, 0x0c,
	0x2a, 5,	0xc8, 0x00, 0x18, 0x12, 0x22,
	0x64, 8,	0x00, 0x00, 0xf0, 0x01, 0x14, 0x44, 0x44, 0x44,
	0x6e, 1,	0x08,
	0xff, 1,	0x01,		/* page 1 */
	0x78, 1,	0x00,
	0, END_OF_SEQUENCE		/* end of sequence */
};

#define SKIP		0xaa
/* page 3 - the value SKIP says skip the index - see reg_w_page() */
static const u8 page3_7302[] = {
	0x90, 0x40, 0x03, 0x00, 0xc0, 0x01, 0x14, 0x16,
	0x14, 0x12, 0x00, 0x00, 0x00, 0x02, 0x33, 0x00,
	0x0f, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x47, 0x01, 0xb3, 0x01, 0x00,
	0x00, 0x08, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x21,
	0x00, 0x00, 0x00, 0x54, 0xf4, 0x02, 0x52, 0x54,
	0xa4, 0xb8, 0xe0, 0x2a, 0xf6, 0x00, 0x00, 0x00,
	0x00, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0xfc, 0x00, 0xf2, 0x1f, 0x04, 0x00, 0x00,
	SKIP, 0x00, 0x00, 0xc0, 0xc0, 0x10, 0x00, 0x00,
	0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x40, 0xff, 0x03, 0x19, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0xc8, 0xc8, 0xc8,
	0xc8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50,
	0x08, 0x10, 0x24, 0x40, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x02, 0x47, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x02, 0xfa, 0x00, 0x64, 0x5a, 0x28, 0x00,
	0x00
};

static void reg_w_buf(struct gspca_dev *gspca_dev,
		u8 index,
		  const u8 *buffer, int len)
{
	int ret;

	if (gspca_dev->usb_err < 0)
		return;
	memcpy(gspca_dev->usb_buf, buffer, len);
	ret = usb_control_msg(gspca_dev->dev,
			usb_sndctrlpipe(gspca_dev->dev, 0),
			0,		/* request */
			USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
			0,		/* value */
			index, gspca_dev->usb_buf, len,
			500);
	if (ret < 0) {
		pr_err("reg_w_buf failed i: %02x error %d\n",
		       index, ret);
		gspca_dev->usb_err = ret;
	}
}


static void reg_w(struct gspca_dev *gspca_dev,
		u8 index,
		u8 value)
{
	int ret;

	if (gspca_dev->usb_err < 0)
		return;
	gspca_dev->usb_buf[0] = value;
	ret = usb_control_msg(gspca_dev->dev,
			usb_sndctrlpipe(gspca_dev->dev, 0),
			0,			/* request */
			USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
			0, index, gspca_dev->usb_buf, 1,
			500);
	if (ret < 0) {
		pr_err("reg_w() failed i: %02x v: %02x error %d\n",
		       index, value, ret);
		gspca_dev->usb_err = ret;
	}
}

static void reg_w_seq(struct gspca_dev *gspca_dev,
		const u8 *seq, int len)
{
	while (--len >= 0) {
		reg_w(gspca_dev, seq[0], seq[1]);
		seq += 2;
	}
}

/* load the beginning of a page */
static void reg_w_page(struct gspca_dev *gspca_dev,
			const u8 *page, int len)
{
	int index;
	int ret = 0;

	if (gspca_dev->usb_err < 0)
		return;
	for (index = 0; index < len; index++) {
		if (page[index] == SKIP)		/* skip this index */
			continue;
		gspca_dev->usb_buf[0] = page[index];
		ret = usb_control_msg(gspca_dev->dev,
				usb_sndctrlpipe(gspca_dev->dev, 0),
				0,			/* request */
			USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
				0, index, gspca_dev->usb_buf, 1,
				500);
		if (ret < 0) {
			pr_err("reg_w_page() failed i: %02x v: %02x error %d\n",
			       index, page[index], ret);
			gspca_dev->usb_err = ret;
			break;
		}
	}
}

/* output a variable sequence */
static void reg_w_var(struct gspca_dev *gspca_dev,
			const u8 *seq,
			const u8 *page3, unsigned int page3_len)
{
	int index, len;

	for (;;) {
		index = *seq++;
		len = *seq++;
		switch (len) {
		case END_OF_SEQUENCE:
			return;
		case LOAD_PAGE3:
			reg_w_page(gspca_dev, page3, page3_len);
			break;
		default:
#ifdef GSPCA_DEBUG
			if (len > USB_BUF_SZ) {
				PDEBUG(D_ERR|D_STREAM,
					"Incorrect variable sequence");
				return;
			}
#endif
			while (len > 0) {
				if (len < 8) {
					reg_w_buf(gspca_dev,
						index, seq, len);
					seq += len;
					break;
				}
				reg_w_buf(gspca_dev, index, seq, 8);
				seq += 8;
				index += 8;
				len -= 8;
			}
		}
	}
	/* not reached */
}

/* this function is called at probe time for pac7302 */
static int sd_config(struct gspca_dev *gspca_dev,
			const struct usb_device_id *id)
{
	struct sd *sd = (struct sd *) gspca_dev;
	struct cam *cam;

	cam = &gspca_dev->cam;

	cam->cam_mode = vga_mode;	/* only 640x480 */
	cam->nmodes = ARRAY_SIZE(vga_mode);

	gspca_dev->cam.ctrls = sd->ctrls;

	sd->flags = id->driver_info;
	return 0;
}

/* This function is used by pac7302 only */
static void setbrightcont(struct gspca_dev *gspca_dev)
{
	struct sd *sd = (struct sd *) gspca_dev;
	int i, v;
	static const u8 max[10] =
		{0x29, 0x33, 0x42, 0x5a, 0x6e, 0x80, 0x9f, 0xbb,
		 0xd4, 0xec};
	static const u8 delta[10] =
		{0x35, 0x33, 0x33, 0x2f, 0x2a, 0x25, 0x1e, 0x17,
		 0x11, 0x0b};

	reg_w(gspca_dev, 0xff, 0x00);		/* page 0 */
	for (i = 0; i < 10; i++) {
		v = max[i];
		v += (sd->ctrls[BRIGHTNESS].val - BRIGHTNESS_MAX)
			* 150 / BRIGHTNESS_MAX;		/* 200 ? */
		v -= delta[i] * sd->ctrls[CONTRAST].val / CONTRAST_MAX;
		if (v < 0)
			v = 0;
		else if (v > 0xff)
			v = 0xff;
		reg_w(gspca_dev, 0xa2 + i, v);
	}
	reg_w(gspca_dev, 0xdc, 0x01);
}

/* This function is used by pac7302 only */
static void setcolors(struct gspca_dev *gspca_dev)
{
	struct sd *sd = (struct sd *) gspca_dev;
	int i, v;
	static const int a[9] =
		{217, -212, 0, -101, 170, -67, -38, -315, 355};
	static const int b[9] =
		{19, 106, 0, 19, 106, 1, 19, 106, 1};

	reg_w(gspca_dev, 0xff, 0x03);			/* page 3 */
	reg_w(gspca_dev, 0x11, 0x01);
	reg_w(gspca_dev, 0xff, 0x00);			/* page 0 */
	for (i = 0; i < 9; i++) {
		v = a[i] * sd->ctrls[COLORS].val / COLOR_MAX + b[i];
		reg_w(gspca_dev, 0x0f + 2 * i, (v >> 8) & 0x07);
		reg_w(gspca_dev, 0x0f + 2 * i + 1, v);
	}
	reg_w(gspca_dev, 0xdc, 0x01);
}

static void setwhitebalance(struct gspca_dev *gspca_dev)
{
	struct sd *sd = (struct sd *) gspca_dev;

	reg_w(gspca_dev, 0xff, 0x00);		/* page 0 */
	reg_w(gspca_dev, 0xc6, sd->ctrls[WHITE_BALANCE].val);

	reg_w(gspca_dev, 0xdc, 0x01);
}

static void setredbalance(struct gspca_dev *gspca_dev)
{
	struct sd *sd = (struct sd *) gspca_dev;

	reg_w(gspca_dev, 0xff, 0x00);		/* page 0 */
	reg_w(gspca_dev, 0xc5, sd->ctrls[RED_BALANCE].val);

	reg_w(gspca_dev, 0xdc, 0x01);
}

static void setbluebalance(struct gspca_dev *gspca_dev)
{
	struct sd *sd = (struct sd *) gspca_dev;

	reg_w(gspca_dev, 0xff, 0x00);			/* page 0 */
	reg_w(gspca_dev, 0xc7, sd->ctrls[BLUE_BALANCE].val);

	reg_w(gspca_dev, 0xdc, 0x01);
}

static void setgain(struct gspca_dev *gspca_dev)
{
	struct sd *sd = (struct sd *) gspca_dev;

	reg_w(gspca_dev, 0xff, 0x03);			/* page 3 */
	reg_w(gspca_dev, 0x10, sd->ctrls[GAIN].val >> 3);

	/* load registers to sensor (Bit 0, auto clear) */
	reg_w(gspca_dev, 0x11, 0x01);
}

static void setexposure(struct gspca_dev *gspca_dev)
{
	struct sd *sd = (struct sd *) gspca_dev;
	u8 clockdiv;
	u16 exposure;

	/* register 2 of frame 3 contains the clock divider configuring the
	   no fps according to the formula: 90 / reg. sd->exposure is the
	   desired exposure time in 0.5 ms. */
	clockdiv = (90 * sd->ctrls[EXPOSURE].val + 1999) / 2000;

	/* Note clockdiv = 3 also works, but when running at 30 fps, depending
	   on the scene being recorded, the camera switches to another
	   quantization table for certain JPEG blocks, and we don't know how
	   to decompress these blocks. So we cap the framerate at 15 fps */
	if (clockdiv < 6)
		clockdiv = 6;
	else if (clockdiv > 63)
		clockdiv = 63;

	/* reg2 MUST be a multiple of 3, except when between 6 and 12?
	   Always round up, otherwise we cannot get the desired frametime
	   using the partial frame time exposure control */
	if (clockdiv < 6 || clockdiv > 12)
		clockdiv = ((clockdiv + 2) / 3) * 3;

	/* frame exposure time in ms = 1000 * clockdiv / 90    ->
	exposure = (sd->exposure / 2) * 448 / (1000 * clockdiv / 90) */
	exposure = (sd->ctrls[EXPOSURE].val * 45 * 448) / (1000 * clockdiv);
	/* 0 = use full frametime, 448 = no exposure, reverse it */
	exposure = 448 - exposure;

	reg_w(gspca_dev, 0xff, 0x03);			/* page 3 */
	reg_w(gspca_dev, 0x02, clockdiv);
	reg_w(gspca_dev, 0x0e, exposure & 0xff);
	reg_w(gspca_dev, 0x0f, exposure >> 8);

	/* load registers to sensor (Bit 0, auto clear) */
	reg_w(gspca_dev, 0x11, 0x01);
}

static void setautogain(struct gspca_dev *gspca_dev)
{
	struct sd *sd = (struct sd *) gspca_dev;

	/* when switching to autogain set defaults to make sure
	   we are on a valid point of the autogain gain /
	   exposure knee graph, and give this change time to
	   take effect before doing autogain. */
	if (sd->ctrls[AUTOGAIN].val) {
		sd->ctrls[EXPOSURE].val = EXPOSURE_DEF;
		sd->ctrls[GAIN].val = GAIN_DEF;
		sd->autogain_ignore_frames =
				PAC_AUTOGAIN_IGNORE_FRAMES;
	} else {
		sd->autogain_ignore_frames = -1;
	}
	setexposure(gspca_dev);
	setgain(gspca_dev);
}

static void sethvflip(struct gspca_dev *gspca_dev)
{
	struct sd *sd = (struct sd *) gspca_dev;
	u8 data, hflip, vflip;

	hflip = sd->ctrls[HFLIP].val;
	if (sd->flags & FL_HFLIP)
		hflip = !hflip;
	vflip = sd->ctrls[VFLIP].val;
	if (sd->flags & FL_VFLIP)
		vflip = !vflip;

	reg_w(gspca_dev, 0xff, 0x03);			/* page 3 */
	data = (hflip ? 0x08 : 0x00) | (vflip ? 0x04 : 0x00);
	reg_w(gspca_dev, 0x21, data);

	/* load registers to sensor (Bit 0, auto clear) */
	reg_w(gspca_dev, 0x11, 0x01);
}

/* this function is called at probe and resume time for pac7302 */
static int sd_init(struct gspca_dev *gspca_dev)
{
	reg_w_seq(gspca_dev, init_7302, sizeof(init_7302)/2);
	return gspca_dev->usb_err;
}

static int sd_start(struct gspca_dev *gspca_dev)
{
	struct sd *sd = (struct sd *) gspca_dev;

	reg_w_var(gspca_dev, start_7302,
		page3_7302, sizeof(page3_7302));
	setbrightcont(gspca_dev);
	setcolors(gspca_dev);
	setwhitebalance(gspca_dev);
	setredbalance(gspca_dev);
	setbluebalance(gspca_dev);
	setautogain(gspca_dev);
	sethvflip(gspca_dev);

	/* only resolution 640x480 is supported for pac7302 */

	sd->sof_read = 0;
	atomic_set(&sd->avg_lum, 270 + sd->ctrls[BRIGHTNESS].val);

	/* start stream */
	reg_w(gspca_dev, 0xff, 0x01);
	reg_w(gspca_dev, 0x78, 0x01);

	return gspca_dev->usb_err;
}

static void sd_stopN(struct gspca_dev *gspca_dev)
{

	/* stop stream */
	reg_w(gspca_dev, 0xff, 0x01);
	reg_w(gspca_dev, 0x78, 0x00);
}

/* called on streamoff with alt 0 and on disconnect for pac7302 */
static void sd_stop0(struct gspca_dev *gspca_dev)
{
	if (!gspca_dev->present)
		return;
	reg_w(gspca_dev, 0xff, 0x01);
	reg_w(gspca_dev, 0x78, 0x40);
}

/* !! coarse_grained_expo_autogain is not used !! */
#define exp_too_low_cnt flags
#define exp_too_high_cnt sof_read
#include "autogain_functions.h"

static void do_autogain(struct gspca_dev *gspca_dev)
{
	struct sd *sd = (struct sd *) gspca_dev;
	int avg_lum = atomic_read(&sd->avg_lum);
	int desired_lum;
	const int deadzone = 30;

	if (sd->autogain_ignore_frames < 0)
		return;

	if (sd->autogain_ignore_frames > 0) {
		sd->autogain_ignore_frames--;
	} else {
		desired_lum = 270 + sd->ctrls[BRIGHTNESS].val;

		auto_gain_n_exposure(gspca_dev, avg_lum, desired_lum,
				deadzone, GAIN_KNEE, EXPOSURE_KNEE);
		sd->autogain_ignore_frames = PAC_AUTOGAIN_IGNORE_FRAMES;
	}
}

/* JPEG header */
static const u8 jpeg_header[] = {
	0xff, 0xd8,	/* SOI: Start of Image */

	0xff, 0xc0,	/* SOF0: Start of Frame (Baseline DCT) */
	0x00, 0x11,	/* length = 17 bytes (including this length field) */
	0x08,		/* Precision: 8 */
	0x02, 0x80,	/* height = 640 (image rotated) */
	0x01, 0xe0,	/* width = 480 */
	0x03,		/* Number of image components: 3 */
	0x01, 0x21, 0x00, /* ID=1, Subsampling 1x1, Quantization table: 0 */
	0x02, 0x11, 0x01, /* ID=2, Subsampling 2x1, Quantization table: 1 */
	0x03, 0x11, 0x01, /* ID=3, Subsampling 2x1, Quantization table: 1 */

	0xff, 0xda,	/* SOS: Start Of Scan */
	0x00, 0x0c,	/* length = 12 bytes (including this length field) */
	0x03,		/* number of components: 3 */
	0x01, 0x00,	/* selector 1, table 0x00 */
	0x02, 0x11,	/* selector 2, table 0x11 */
	0x03, 0x11,	/* selector 3, table 0x11 */
	0x00, 0x3f,	/* Spectral selection: 0 .. 63 */
	0x00		/* Successive approximation: 0 */
};

/* this function is run at interrupt level */
static void sd_pkt_scan(struct gspca_dev *gspca_dev,
			u8 *data,			/* isoc packet */
			int len)			/* iso packet length */
{
	struct sd *sd = (struct sd *) gspca_dev;
	u8 *image;
	u8 *sof;

	sof = pac_find_sof(&sd->sof_read, data, len);
	if (sof) {
		int n, lum_offset, footer_length;

		/* 6 bytes after the FF D9 EOF marker a number of lumination
		   bytes are send corresponding to different parts of the
		   image, the 14th and 15th byte after the EOF seem to
		   correspond to the center of the image */
		lum_offset = 61 + sizeof pac_sof_marker;
		footer_length = 74;

		/* Finish decoding current frame */
		n = (sof - data) - (footer_length + sizeof pac_sof_marker);
		if (n < 0) {
			gspca_dev->image_len += n;
			n = 0;
		} else {
			gspca_frame_add(gspca_dev, INTER_PACKET, data, n);
		}

		image = gspca_dev->image;
		if (image != NULL
		 && image[gspca_dev->image_len - 2] == 0xff
		 && image[gspca_dev->image_len - 1] == 0xd9)
			gspca_frame_add(gspca_dev, LAST_PACKET, NULL, 0);

		n = sof - data;
		len -= n;
		data = sof;

		/* Get average lumination */
		if (gspca_dev->last_packet_type == LAST_PACKET &&
				n >= lum_offset)
			atomic_set(&sd->avg_lum, data[-lum_offset] +
						data[-lum_offset + 1]);

		/* Start the new frame with the jpeg header */
		/* The PAC7302 has the image rotated 90 degrees */
		gspca_frame_add(gspca_dev, FIRST_PACKET,
				jpeg_header, sizeof jpeg_header);
	}
	gspca_frame_add(gspca_dev, INTER_PACKET, data, len);
}

#ifdef CONFIG_VIDEO_ADV_DEBUG
static int sd_dbg_s_register(struct gspca_dev *gspca_dev,
			struct v4l2_dbg_register *reg)
{
	u8 index;
	u8 value;

	/* reg->reg: bit0..15: reserved for register index (wIndex is 16bit
			       long on the USB bus)
	*/
	if (reg->match.type == V4L2_CHIP_MATCH_HOST &&
	    reg->match.addr == 0 &&
	    (reg->reg < 0x000000ff) &&
	    (reg->val <= 0x000000ff)
	) {
		/* Currently writing to page 0 is only supported. */
		/* reg_w() only supports 8bit index */
		index = reg->reg;
		value = reg->val;

		/* Note that there shall be no access to other page
		   by any other function between the page swith and
		   the actual register write */
		reg_w(gspca_dev, 0xff, 0x00);		/* page 0 */
		reg_w(gspca_dev, index, value);

		reg_w(gspca_dev, 0xdc, 0x01);
	}
	return gspca_dev->usb_err;
}

static int sd_chip_ident(struct gspca_dev *gspca_dev,
			struct v4l2_dbg_chip_ident *chip)
{
	int ret = -EINVAL;

	if (chip->match.type == V4L2_CHIP_MATCH_HOST &&
	    chip->match.addr == 0) {
		chip->revision = 0;
		chip->ident = V4L2_IDENT_UNKNOWN;
		ret = 0;
	}
	return ret;
}
#endif

#if defined(CONFIG_INPUT) || defined(CONFIG_INPUT_MODULE)
static int sd_int_pkt_scan(struct gspca_dev *gspca_dev,
			u8 *data,		/* interrupt packet data */
			int len)		/* interrput packet length */
{
	int ret = -EINVAL;
	u8 data0, data1;

	if (len == 2) {
		data0 = data[0];
		data1 = data[1];
		if ((data0 == 0x00 && data1 == 0x11) ||
		    (data0 == 0x22 && data1 == 0x33) ||
		    (data0 == 0x44 && data1 == 0x55) ||
		    (data0 == 0x66 && data1 == 0x77) ||
		    (data0 == 0x88 && data1 == 0x99) ||
		    (data0 == 0xaa && data1 == 0xbb) ||
		    (data0 == 0xcc && data1 == 0xdd) ||
		    (data0 == 0xee && data1 == 0xff)) {
			input_report_key(gspca_dev->input_dev, KEY_CAMERA, 1);
			input_sync(gspca_dev->input_dev);
			input_report_key(gspca_dev->input_dev, KEY_CAMERA, 0);
			input_sync(gspca_dev->input_dev);
			ret = 0;
		}
	}

	return ret;
}
#endif

/* sub-driver description for pac7302 */
static const struct sd_desc sd_desc = {
	.name = KBUILD_MODNAME,
	.ctrls = sd_ctrls,
	.nctrls = ARRAY_SIZE(sd_ctrls),
	.config = sd_config,
	.init = sd_init,
	.start = sd_start,
	.stopN = sd_stopN,
	.stop0 = sd_stop0,
	.pkt_scan = sd_pkt_scan,
	.dq_callback = do_autogain,
#ifdef CONFIG_VIDEO_ADV_DEBUG
	.set_register = sd_dbg_s_register,
	.get_chip_ident = sd_chip_ident,
#endif
#if defined(CONFIG_INPUT) || defined(CONFIG_INPUT_MODULE)
	.int_pkt_scan = sd_int_pkt_scan,
#endif
};

/* -- module initialisation -- */
static const struct usb_device_id device_table[] = {
	{USB_DEVICE(0x06f8, 0x3009)},
	{USB_DEVICE(0x06f8, 0x301b)},
	{USB_DEVICE(0x093a, 0x2620)},
	{USB_DEVICE(0x093a, 0x2621)},
	{USB_DEVICE(0x093a, 0x2622), .driver_info = FL_VFLIP},
	{USB_DEVICE(0x093a, 0x2624), .driver_info = FL_VFLIP},
	{USB_DEVICE(0x093a, 0x2625)},
	{USB_DEVICE(0x093a, 0x2626)},
	{USB_DEVICE(0x093a, 0x2628)},
	{USB_DEVICE(0x093a, 0x2629), .driver_info = FL_VFLIP},
	{USB_DEVICE(0x093a, 0x262a)},
	{USB_DEVICE(0x093a, 0x262c)},
	{USB_DEVICE(0x145f, 0x013c)},
	{USB_DEVICE(0x1ae7, 0x2001)}, /* SpeedLink Snappy Mic SL-6825-SBK */
	{}
};
MODULE_DEVICE_TABLE(usb, device_table);

/* -- device connect -- */
static int sd_probe(struct usb_interface *intf,
			const struct usb_device_id *id)
{
	return gspca_dev_probe(intf, id, &sd_desc, sizeof(struct sd),
				THIS_MODULE);
}

static struct usb_driver sd_driver = {
	.name = KBUILD_MODNAME,
	.id_table = device_table,
	.probe = sd_probe,
	.disconnect = gspca_disconnect,
#ifdef CONFIG_PM
	.suspend = gspca_suspend,
	.resume = gspca_resume,
#endif
};

module_usb_driver(sd_driver);
