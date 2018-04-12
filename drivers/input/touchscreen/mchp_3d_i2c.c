/*
 * Microchip 3D I2C Touchscreen Driver
 *
 * Copyright (c) 2017 Microchip Technology, Inc.
 *
 * http://www.microchip.com/mtouch
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */
#include <linux/input.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/input/mt.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_device.h>
#include <linux/version.h>
#include <linux/firmware.h>

#define MAX_WIDTH 0x7fff	
#define MAX_HEIGHT 0x7fff
#define MAX_UP 0x7fff // Z axes, this is here max value
#define MAX_TOUCHES 10
#define Empty0	0x00
#define InvalidFw	0x0A
#define ValidFw		0xAA
#define Empty1		0xFF

#ifndef __ATTR_RO
#define __ATTR_RO(_name) {                                              \
         .attr   = { .name = __stringify(_name), .mode = S_IRUGO },      \
         .show   = _name##_show,                                         \
}
#endif

// TODO: S_IWUGO is temporary to make sysfs parameters world writable
#ifndef __ATTR_WO
#define __ATTR_WO(_name) {                                              \
         .attr   = { .name = __stringify(_name), .mode = S_IWUSR | S_IWUGO },      \
         .store  = _name##_store,                                        \
}
#endif

// TODO: S_IWUGO is temporary to make sysfs parameters world writable
#ifndef __ATTR_RW
#define __ATTR_RW(_name) __ATTR(_name, (S_IWUSR | S_IRUGO | S_IWUGO ),             \
                         _name##_show, _name##_store)
#endif

#ifndef DEVICE_ATTR_WO
#define DEVICE_ATTR_WO(_name) \
         struct device_attribute dev_attr_##_name = __ATTR_WO(_name)
#endif

#ifndef DEVICE_ATTR_RO
#define DEVICE_ATTR_RO(_name) \
         struct device_attribute dev_attr_##_name = __ATTR_RO(_name)
#endif 

#ifndef DEVICE_ATTR_RW
#define DEVICE_ATTR_RW(_name) \
        struct device_attribute dev_attr_##_name = __ATTR_RW(_name)
#endif

#define BUFFER_SIZE  2000
#define MCHP_SLAVE_ADDR 0x42

char buf_size = 0x30; //default value for buffer size for MGC31x0 data

struct Buffer {
    unsigned char data[BUFFER_SIZE];
    unsigned int newestIndex;
    unsigned int oldestIndex;
};

struct mchp_3d_i2c_priv {
    struct i2c_client *client;
    struct input_dev *input;
    struct work_struct work;
    int touch_enable;
    int i2c_read_buffer_size;
    int air_swipe_left;
    int air_swipe_right;
    int air_swipe_up;
    int air_swipe_down;
    int air_hover_hold;
    int air_circle_clock;
    int air_circle_counterclock;
    char rx_buf[BUFFER_SIZE];
    int rx_len;
    int isReadingStreamBuffer;
    int stream_buffer_size;
    int target_device;
    struct Buffer circularBuffer;
};

enum BufferStatus {BUFFER_OK, BUFFER_EMPTY, BUFFER_FULL};

enum BufferStatus bufferRead(struct mchp_3d_i2c_priv *priv, unsigned char *byte) {
    if (priv->circularBuffer.newestIndex == priv->circularBuffer.oldestIndex)
    {
        return BUFFER_EMPTY;
    }
    
    *byte = priv->circularBuffer.data[priv->circularBuffer.oldestIndex];
    priv->circularBuffer.oldestIndex = (priv->circularBuffer.oldestIndex+1) % BUFFER_SIZE;
    return BUFFER_OK;
};

enum BufferStatus bufferWrite(struct mchp_3d_i2c_priv *priv, unsigned char byte){
    unsigned int next_index = (priv->circularBuffer.newestIndex+1) % BUFFER_SIZE;

    if (next_index == priv->circularBuffer.oldestIndex)
    {
        return BUFFER_FULL;
    }
    
    priv->circularBuffer.data[priv->circularBuffer.newestIndex] = byte;
    priv->circularBuffer.newestIndex = next_index;
    return BUFFER_OK;
};



/******************************************************************************
Function:
    stream_buffer_size_show()

Description:
    Display value of "stream_buffer_size" variable to application that is
    requesting it's value.    
******************************************************************************/
static ssize_t stream_buffer_size_show(struct device *dev,
                struct device_attribute *attr, char *buf)
{
    struct mchp_3d_i2c_priv *priv = dev_get_drvdata(dev);

    return sprintf(buf, "%d", priv->stream_buffer_size);;
}

static DEVICE_ATTR_RO(stream_buffer_size);

static ssize_t touch_enable_show(struct device *dev,
                struct device_attribute *attr, char *buf)
{
    struct mchp_3d_i2c_priv *priv = dev_get_drvdata(dev);

    return sprintf(buf, "%d", priv->touch_enable);
}

static ssize_t touch_enable_store(struct device *dev,
                   struct device_attribute *attr,
                   const char *buf, size_t count)
{
    struct mchp_3d_i2c_priv *priv = dev_get_drvdata(dev);

    sscanf(buf, "%d", &priv->touch_enable);

    printk("touchEnable_store1: value has changed to %d\n",priv->touch_enable);

    return count;
}

static DEVICE_ATTR_RW(touch_enable); 

static ssize_t i2c_read_buffer_size_show(struct device *dev,
                struct device_attribute *attr, char *buf)
{
    struct mchp_3d_i2c_priv *priv = dev_get_drvdata(dev);

    return sprintf(buf, "%d\n", priv->i2c_read_buffer_size);
}

static ssize_t i2c_read_buffer_size_store(struct device *dev,
                   struct device_attribute *attr,
                   const char *buf, size_t count)
{
    struct mchp_3d_i2c_priv *priv = dev_get_drvdata(dev);

    sscanf(buf, "%d", &priv->i2c_read_buffer_size);

    printk("i2c_read_buffer_size_store: value has changed to %d\n",priv->i2c_read_buffer_size);

    return count;
}

static DEVICE_ATTR_RW(i2c_read_buffer_size);

static ssize_t air_swipe_left_show(struct device *dev,
                struct device_attribute *attr, char *buf)
{
    struct mchp_3d_i2c_priv *priv = dev_get_drvdata(dev);

    return sprintf(buf, "%d", priv->air_swipe_left);
}

static ssize_t air_swipe_left_store(struct device *dev,
                   struct device_attribute *attr,
                   const char *buf, size_t count)
{
    struct mchp_3d_i2c_priv *priv = dev_get_drvdata(dev);

    sscanf(buf, "%d", &priv->air_swipe_left);

    printk("air_swipe_left_store: value has changed to %d\n",priv->air_swipe_left);

    return count;
}

static DEVICE_ATTR_RW(air_swipe_left);

static ssize_t air_swipe_right_show(struct device *dev,
                struct device_attribute *attr, char *buf)
{
    struct mchp_3d_i2c_priv *priv = dev_get_drvdata(dev);

    return sprintf(buf, "%d", priv->air_swipe_right);
}

static ssize_t air_swipe_right_store(struct device *dev,
                   struct device_attribute *attr,
                   const char *buf, size_t count)
{
    struct mchp_3d_i2c_priv *priv = dev_get_drvdata(dev);

    sscanf(buf, "%d", &priv->air_swipe_right);

    printk("air_swipe_right_store: value has changed to %d\n",priv->air_swipe_right);

    return count;
}

static DEVICE_ATTR_RW(air_swipe_right);

static ssize_t air_swipe_up_show(struct device *dev,
                struct device_attribute *attr, char *buf)
{
    struct mchp_3d_i2c_priv *priv = dev_get_drvdata(dev);

    return sprintf(buf, "%d", priv->air_swipe_up);
}

static ssize_t air_swipe_up_store(struct device *dev,
                   struct device_attribute *attr,
                   const char *buf, size_t count)
{
    struct mchp_3d_i2c_priv *priv = dev_get_drvdata(dev);

    sscanf(buf, "%d", &priv->air_swipe_up);

    printk("air_swipe_up_store: value has changed to %d\n",priv->air_swipe_up);

    return count;
}

static DEVICE_ATTR_RW(air_swipe_up);

static ssize_t air_swipe_down_show(struct device *dev,
                struct device_attribute *attr, char *buf)
{
    struct mchp_3d_i2c_priv *priv = dev_get_drvdata(dev);

    return sprintf(buf, "%d", priv->air_swipe_down);
}

static ssize_t air_swipe_down_store(struct device *dev,
                   struct device_attribute *attr,
                   const char *buf, size_t count)
{
    struct mchp_3d_i2c_priv *priv = dev_get_drvdata(dev);

    sscanf(buf, "%d", &priv->air_swipe_down);

    printk("air_swipe_down_store: value has changed to %d\n",priv->air_swipe_down);

    return count;
}

static DEVICE_ATTR_RW(air_swipe_down);

static ssize_t air_hover_hold_show(struct device *dev,
                struct device_attribute *attr, char *buf)
{
    struct mchp_3d_i2c_priv *priv = dev_get_drvdata(dev);

    return sprintf(buf, "%d", priv->air_hover_hold);
}

static ssize_t air_hover_hold_store(struct device *dev,
                   struct device_attribute *attr,
                   const char *buf, size_t count)
{
    struct mchp_3d_i2c_priv *priv = dev_get_drvdata(dev);

    sscanf(buf, "%d", &priv->air_hover_hold);

    printk("air_hover_hold_store: value has changed to %d\n",priv->air_hover_hold);

    return count;
}

static DEVICE_ATTR_RW(air_hover_hold);

static ssize_t air_circle_clock_show(struct device *dev,
                struct device_attribute *attr, char *buf)
{
    struct mchp_3d_i2c_priv *priv = dev_get_drvdata(dev);

    return sprintf(buf, "%d", priv->air_circle_clock);
}

static ssize_t air_circle_clock_store(struct device *dev,
                   struct device_attribute *attr,
                   const char *buf, size_t count)
{
    struct mchp_3d_i2c_priv *priv = dev_get_drvdata(dev);

    sscanf(buf, "%d", &priv->air_circle_clock);

    printk("air_swipe_left_store: value has changed to %d\n",priv->air_circle_clock);

    return count;
}

static DEVICE_ATTR_RW(air_circle_clock);

static ssize_t air_circle_counterclock_show(struct device *dev,
                struct device_attribute *attr, char *buf)
{
    struct mchp_3d_i2c_priv *priv = dev_get_drvdata(dev);

    return sprintf(buf, "%d", priv->air_circle_counterclock);
}

static ssize_t air_circle_counterclock_store(struct device *dev,
                   struct device_attribute *attr,
                   const char *buf, size_t count)
{
    struct mchp_3d_i2c_priv *priv = dev_get_drvdata(dev);

    sscanf(buf, "%d", &priv->air_circle_counterclock);

    printk("air_swipe_left_store: value has changed to %d\n",priv->air_circle_counterclock);

    return count;
}

static DEVICE_ATTR_RW(air_circle_counterclock);

static ssize_t receive_buffer_show(struct device *dev,
                struct device_attribute *attr, char *buf)
{
    struct mchp_3d_i2c_priv *priv = dev_get_drvdata(dev);
    int i=0;
    int ret;

    if (priv->isReadingStreamBuffer)
    {
        unsigned char byte_from_buffer;

        printk("receive_buffer_show: reading from stream buffer\n");
        priv->isReadingStreamBuffer=0;

        while (!(BUFFER_EMPTY == bufferRead(priv, &byte_from_buffer)))
        {
               priv->rx_buf[i]=byte_from_buffer;
            // printk("rx_buf[%d] = %x\n",i,priv->rx_buf[i]);
               i++;
               if (i == priv->rx_len)
               {
                printk("receiveBuffer_show: breaking, i %d rx_len %d\n",i, priv->rx_len);
                break;
               }
        }

        priv->stream_buffer_size-=i;
        priv->rx_len = 0;

        if (i < priv->rx_len)
        {
            printk("receiveBuffer_show: request for more bytes than available, i %d rx_len %d\n",i, priv->rx_len);
        }
        else
        {
            printk("receiveBuffer_show1: returning %d bytes\n",i);
        }
        printk("stream_buffer_size is now %d\n",priv->stream_buffer_size);
        memcpy(buf,priv->rx_buf,i);

        return i;
    }
    else
    {
        printk("receive_buffer_show: NOT reading from stream buffer\n");
    }

    printk("receiveBuffer_show: trying to read %d bytes\n",priv->rx_len);
    ret=i2c_master_recv(priv->client, priv->rx_buf, priv->rx_len);
    priv->rx_len = 0;

    if (ret<0)
    {
        printk("receiveBuffer_show: error reading bytes!\n");
        return 0;
    }

    /* ret contains the actual number of bytes read */
    for (i=0;i<ret;i++)
    {
        printk("receiveBuffer_show: value 0x%02x\n", priv->rx_buf[i]);
    }
    memcpy(buf, priv->rx_buf, ret);

    return ret;

}

static ssize_t receive_buffer_store(struct device *dev,
                   struct device_attribute *attr,
                   const char *buf, size_t count)
{
    struct mchp_3d_i2c_priv *priv = dev_get_drvdata(dev);
    int ret;

    ret=sscanf(buf,"%d", &priv->rx_len);

    printk("receiveBuffer_store: setting number of bytes to clock, buf %s count %d ret %d rx_len %d\n",buf,count,ret, priv->rx_len);

    return count;
}

static DEVICE_ATTR_RW(receive_buffer);

static ssize_t send_buffer_store(struct device *dev,
                   struct device_attribute *attr,
                   const char *buf, size_t count)
{
    struct mchp_3d_i2c_priv *priv = dev_get_drvdata(dev);

    char tx_buf[256];
    unsigned char tx_len;
    unsigned char address;
    int i;
    //int temp;

    if (!buf)
    {
        printk("send_buffer_store: warning - buf is NULL.\n");
        return 0;
    }

    if (count > 256)
    {
        printk("send_buffer_store: buffer size too large.\n");
        return 0;
    }
    printk("sendBuffer_store: address - %d\n",buf[0]);
    address = buf[0];
    printk("sendBuffer_store: bytes to send - %d\n",buf[1]);
    tx_len = buf[1];
    tx_buf[0] = address;

    if((tx_len==0) && (address==0xfe))
    {
        printk("it has been detected that we want to read from streambuffer\n");
        priv->isReadingStreamBuffer=1;
        return count;
    }

    /* re-create buffer without number of bytes element */
    for (i = 1;i < tx_len + 1;i++) 
    {
        tx_buf[i] = buf[i + 1];
    }

    i2c_master_send(priv->client, tx_buf, tx_len + 1);

    /* We convert to a hex string in case we want to see what was last transmitted.*/
/*    for (i = 0;i < tx_len; i++) {
        if (i == 0)
            sprintf(priv->tx_buf,"0x%02x",buf[i]);
        else
            sprintf(priv->tx_buf, "%s 0x%02x",
                priv->tx_buf, buf[i]);
    }*/

    return count;
}

static DEVICE_ATTR_WO(send_buffer);


static struct attribute *mchp_3d_attrs[] = {
    &dev_attr_stream_buffer_size.attr,
    &dev_attr_touch_enable.attr,
    &dev_attr_i2c_read_buffer_size.attr,
    &dev_attr_air_swipe_left.attr,
    &dev_attr_air_swipe_right.attr,
    &dev_attr_air_swipe_up.attr,
    &dev_attr_air_swipe_down.attr,
    &dev_attr_air_hover_hold.attr,
    &dev_attr_air_circle_clock.attr,
    &dev_attr_air_circle_counterclock.attr,
    &dev_attr_receive_buffer.attr,
    &dev_attr_send_buffer.attr,
    NULL,
};

static struct attribute_group attr_group = {
    .attrs = mchp_3d_attrs,
};

static void send_input(struct mchp_3d_i2c_priv *priv, int id)
{
    if (id < 255)
    {
        printk("send_input: sending keypress %d\n",id);
                input_report_key(priv->input, id, 1);
                input_sync(priv->input);
                input_report_key(priv->input, id, 0);
                input_sync(priv->input);
    }

    if (id == 2000)
    {
        printk("send_input: sending left click\n");
                input_report_key(priv->input, BTN_LEFT, 1);
                input_sync(priv->input);
                input_report_key(priv->input, BTN_LEFT, 0);
                input_sync(priv->input);
    }
    else if (id == 2001)
    {
        printk("send_input: sending middle click\n");
                input_report_key(priv->input, BTN_MIDDLE, 1);
                input_sync(priv->input);
                input_report_key(priv->input, BTN_MIDDLE, 0);
                input_sync(priv->input);
    }
    else if (id == 2002)
    {
        printk("send_input: sending right click\n");
                input_report_key(priv->input, BTN_RIGHT, 1);
                input_sync(priv->input);
                input_report_key(priv->input, BTN_RIGHT, 0);
                input_sync(priv->input);
    }

}

static void decode_mgc313X_data(struct mchp_3d_i2c_priv *priv, char* data, int size)
{
    //int i;	//Debug only
	
    unsigned int dataOutputConfigMask = ((data[3]) & 0xFF) + (((data[4]) & 0xFF) << 8);
	//unsigned int timeStamp = data[5];
    unsigned int systemInformation = data[6];
    unsigned int element = 7;
    
    if((dataOutputConfigMask & 0x0001) == 1) // DSP Status
    {
        element += 2;
    }

    if((dataOutputConfigMask & 0x0002) == 2) // Gesture Information
    {	
        unsigned int gesture = ((data[element]) & 0xFF) + (((data[element + 1]) & 0xFF) << 8) + (((data[element + 2]) & 0xFF) << 16) + (((data[element + 3]) & 0xFF) << 24);
        unsigned int gestureType = gesture & 0x0000000F;
        unsigned int gestureClass = (gesture & 0x0000F000) >> 12;
        unsigned int edgeFlick = (gesture & 0x00010000) >> 16;
        unsigned int gestureInProgress = (gesture & 0x80000000) >> 31;
        element += 4;
 	
	//Debug only
	//printk("in function decode_mgc313X_data 0x%02x\n",dataOutputConfigMask); //Output Data Mask

        if (gestureClass == 1)
        {
            printk(KERN_DEBUG "decode_mgc313X_data 0x%02x 0x%02x 0x%02x 0x%02x\n",gestureType, gestureClass, edgeFlick, gestureInProgress);
            switch (gestureType)
            {
            case 0:
                printk("decode_mgc313X_data: hover hold\n");
                send_input(priv, priv->air_hover_hold);
                break;
            case 2:
                printk("decode_mgc313X_data: air swipe right\n");
                send_input(priv, priv->air_swipe_right);
                break;
            case 3:
                printk("decode_mgc313X_data: air swipe left\n");
                send_input(priv, priv->air_swipe_left);
                break;
            case 4:
                printk("decode_mgc313X_data: air swipe up\n");
                send_input(priv, priv->air_swipe_up);
                break;
            case 5:
                printk("decode_mgc313X_data: air swipe down\n");
                send_input(priv, priv->air_swipe_down);
                break;
	    	case 6:
                printk("decode_mgc313X_data: air circle clock\n");
                send_input(priv, priv->air_circle_clock);
                break;
	    	case 7:
                printk("decode_mgc313X_data: air circle counterclock\n");
                send_input(priv, priv->air_circle_counterclock);
                break;
            default:
                printk("decode_mgc313X_data unhandled gesture type %d\n",gestureType);
            }
        }
    }

    if((dataOutputConfigMask & 0x0004) == 4) // Touch Information
    {
        element += 4;
    }

    if((dataOutputConfigMask & 0x0008) == 8) // Air Wheel
    {
        if((systemInformation & 0x02) == 2)
        {
            unsigned int airWheel = ((data[element]) & 0xFF) + (((data[element + 1]) & 0xFF) << 8);
            printk("airwheel %u\n", airWheel);
        }

        element += 2;
    }


    if((dataOutputConfigMask & 16) == 16)	//3D position data
    {
        
        int x = ((data[element+1] << 8) & 0xff00) | ((data[element]) & 0xff);// added
        int y = ((data[element+3] << 8) & 0xff00) | ((data[element+2]) & 0xff);// added
        int z = ((data[element+5] << 8) & 0xff00) | ((data[element+4]) & 0xff);// added
        
        input_report_abs(priv->input, ABS_X, x);// added
        input_report_abs(priv->input, ABS_Y, y);// added
        input_report_abs(priv->input, ABS_Z, z);// added
        input_sync(priv->input);	// sync touch reports to userspace
        
        //debug only : printk("MCHP_3DInterface::decodeGestICPacket: 16 , X %d Y %d Z %d\n", ((data[element+1] << 8) & 0xff00) | ((data[element]) & 0xff), ((data[element+3] << 8) & 0xff00) | ((data[element+2]) & 0xff), ((data[element+5] << 8) & 0xff00) | ((data[element+4]) & 0xff));

    }
}

static void mgc313x_i2c_readdata(struct work_struct *work)
{
    struct mchp_3d_i2c_priv *priv =
        container_of(work, struct mchp_3d_i2c_priv, work);

    char* payload_buf = NULL;
    int num_bytes = 0;
	int bytes_to_read = 0;
    //char buff[400];
	//int i;
	
	payload_buf = (char*)kmalloc(priv->i2c_read_buffer_size, GFP_KERNEL);
	
    if(payload_buf == NULL)
		
    {
        printk("mgc313x_i2c_readdata: unable to allocate temporary data block\n");
        return;
    }
 
 	//memset(buff,0,sizeof(buff));  //Clear buff memory 
	
	i2c_master_recv (priv->client, &payload_buf[0], priv->i2c_read_buffer_size);	
	

    num_bytes = payload_buf[0]; 
	
	if (num_bytes > priv->i2c_read_buffer_size)
	{
		bytes_to_read = num_bytes - priv->i2c_read_buffer_size;	//Record left over bytes to be read
		num_bytes = priv->i2c_read_buffer_size;	//Never allow num_bytes to overrun buffer
		
	}
	
/* Debug Only

	for (i=0;i<num_bytes;i++)
    {
        if (i==0)
        {
            sprintf(buff,"0x%02x ",payload_buf[i]);
        }
        else
        {
            sprintf(buff,"%s 0x%02x ",buff, payload_buf[i]);
        }
    }
	
	
	printk("mchp_3d_i2c_readdata - Msg_size: %d, Payload Data: %s\n", num_bytes, buff);
	*/
	
	if (bytes_to_read > 0)
	{
		i2c_master_recv (priv->client, &payload_buf[0], bytes_to_read);
		
		kfree(payload_buf);//free alocated mem.
		
		return; 	//Do not decode the reset of the messages.  TODO: Check if change line is high?
	}
	
	decode_mgc313X_data (priv, &payload_buf[1], num_bytes-1); //Only send the payload based on msg_size, not full i2c_buffer

    kfree(payload_buf);  //free alocated mem.
	

}

static irqreturn_t mchp_3d_ts_interrupt(int irq, void *dev_id)
{
    struct mchp_3d_i2c_priv *priv = dev_id;

    if (priv)
    {
        schedule_work(&priv->work);
		
    }
    
    return IRQ_HANDLED;
}

static int mchp_3d_create_input_device(struct mchp_3d_i2c_priv *priv)
{
    struct device *dev = &priv->client->dev;
    struct input_dev *input;
    int ret;
    int i;

    #if LINUX_VERSION_CODE < KERNEL_VERSION(3, 7, 0)    
        input = input_allocate_device();
    #else
        input = devm_input_allocate_device(dev);
    #endif

    if (!input)
    {
        dev_err(dev, "failed to allocate input device\n");
        return -ENOMEM;
    }
    priv->input = input;
   
    input->name = "MCHP_3D_Touchscreen";
    input->id.bustype = BUS_I2C;
    input->dev.parent = dev;

    input->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
    input->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);
    input->keybit[BIT_WORD(BTN_MOUSE)] = BIT_MASK(BTN_LEFT) | BIT_MASK(BTN_MIDDLE) | BIT_MASK(BTN_RIGHT);

    // enable all keyboard events to be mappable/usable
    for (i=0;i<255;i++)
    {
        set_bit(i,  input->keybit);
    }
    
    input_set_abs_params(input, ABS_X, 0, MAX_WIDTH, 0, 0);
    input_set_abs_params(input, ABS_Y, 0, MAX_HEIGHT, 0, 0);
    input_set_abs_params(input, ABS_Z, 0, MAX_UP, 0, 0);//!!!!added Z axis in input with range from absmin=0 to absmax=MAX_UP, noise in the data absfuzz=0, center flat position absflat=0.

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 6, 0)
    input_mt_init_slots(input, MAX_TOUCHES);
#else
    input_mt_init_slots(input, MAX_TOUCHES,
    INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
#endif

    input_set_abs_params(input, ABS_MT_POSITION_X, 0,
            MAX_WIDTH, 0, 0);
    input_set_abs_params(input, ABS_MT_POSITION_Y, 0,
            MAX_HEIGHT, 0, 0);
    input_set_abs_params(input, ABS_MT_TRACKING_ID, 0, 15, 0, 0);

    input_set_drvdata(input, priv);
    i2c_set_clientdata(priv->client, priv);

    ret = input_register_device(input);
    if (ret)
    {
        dev_err(dev, "failed to register input device: %d\n", ret);
        return ret;
    }

    return 0;
}

static int mchp_3d_i2c_probe(struct i2c_client *client,
            const struct i2c_device_id *id)
{
 
    struct mchp_3d_i2c_priv *priv;
	
    unsigned long irqflags;
    int ret;
	int msg_size;
	char fw_buffer[50];
	int loopcount = 0x05; 
	//int i;

    // set MGC3140 flags,  (Byte [8]) --Bit [0] - DSP Data, Bit[1] - Gesture Data, Bit[3] - AirWheelInfo, Bit[4] - xyzPosition
    char setGestIDFlags_Enable[16] = { 0x10, 0x00, 0x00, 0xA2, 0xA0, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF };
	char setGestIDFlags_Disable[16] = { 0x10, 0x00, 0x00, 0xA2, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF };
	char setGestIDFlags_Req_FwVersion[12] = { 0x0c, 0x00, 0x00, 0x06, 0x83, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	
	// clear fw_buffer
	memset(fw_buffer,0,sizeof(fw_buffer));
	
	
    /* Check if the I2C function is ok in this adaptor */
  
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
    {
        return -ENXIO;
    }

    priv = devm_kzalloc(&client->dev, sizeof(struct mchp_3d_i2c_priv), GFP_KERNEL);

    if (!priv)
    {
        printk(KERN_ERR "mchp_3d_i2c: kzalloc error\n");
        ret = -ENOMEM;
        goto error;
    }

    priv->touch_enable = 1;
    priv->i2c_read_buffer_size = buf_size;	//Init the buffer size, curerntly 0x2E
    priv->air_swipe_left=KEY_LEFT;			//Value = 105
    priv->air_swipe_right=KEY_RIGHT;		//Valve = 106
    priv->air_swipe_up=KEY_UP;				//Value = 103
    priv->air_swipe_down=KEY_DOWN;			//Value = 108
    priv->air_hover_hold=KEY_ENTER;			//Value = 28
    priv->air_circle_clock=KEY_0;			//Value = 11
    priv->air_circle_counterclock=KEY_1;	//Value = 1

    priv->rx_len = 1;
    priv->isReadingStreamBuffer=0;
    priv->stream_buffer_size=0;
    priv->client = client;
    

    client->addr = MCHP_SLAVE_ADDR; 
	
    printk("mchp_3d_i2c_probe: client_addr: %x\n", client->addr);
	
    INIT_WORK(&priv->work, mgc313x_i2c_readdata);
 
    ret = mchp_3d_create_input_device(priv);
	
    if (ret)
    {
        goto error;
    }


    printk("mchp_3d_i2c_probe: Initializing interrupt\n");

    irqflags = client->dev.of_node ? 0 : IRQF_TRIGGER_FALLING;

    ret = devm_request_threaded_irq(&client->dev, client->irq, NULL,
          		mchp_3d_ts_interrupt,
           	 	irqflags | IRQF_ONESHOT,
            	client->name, priv);

    if (ret)
    {
    	dev_err(&client->dev, "request irq failed: %d\n", ret);
        return ret;
    }
	
	disable_irq(client->irq);
	printk("mchp_3d_i2c_probe: Disable IRQ.\n");
	
	//Output adapter and irq #
	printk("mchp_3d_i2c_probe: adapter=%d, client irq: %d\n", client->adapter->nr, client->irq);

    ret = sysfs_create_group(&client->dev.kobj, &attr_group);

    if (ret)
    {

        printk("mchp_3d_i2c_probe:create mchp_3d sysfs group failed: %d\n", ret);
        kobject_put(&client->dev.kobj);
        goto error;
    }
	
	
	//Check if there is a FW_VERSION_INFO
	
	i2c_master_send (priv->client, setGestIDFlags_Disable, sizeof(setGestIDFlags_Disable));
	printk("mchp_3d_i2c_probe: Disable MGC3140 outputs\n");
	
	i2c_master_send (priv->client, setGestIDFlags_Req_FwVersion, sizeof(setGestIDFlags_Req_FwVersion));
	printk("mchp_3d_i2c_probe: Request FW version\n");
	
	i2c_master_recv (priv->client, &fw_buffer[0], buf_size);	//Read full message up to 46 bytes
	 
	msg_size = fw_buffer[0];
	
	//loop for 5 counts
	
	while (loopcount > 0) {
		
		if (msg_size != 0x00) {
			
			if (fw_buffer[3] == 0x83) {
				
				switch (fw_buffer[4]) {
						
					case Empty0:
					case Empty1:
					case InvalidFw:
						printk("mchp_3d_probe: No complete firmware image on device \n");
						printk("mchp_3d_probe: Reload a valid firmware into device \n");
						break;
						
					case ValidFw:
						printk("mchp_3d_probe: A valid firmware image is found in device \n");
						printk("mchp_3d_probe: Msg size - 0x%02x Msg ID: 0x%02x\n", fw_buffer[0], fw_buffer[3]);
						printk("mchp_3d_probe: Bootloader Ver: %d.%d\n", fw_buffer[11], fw_buffer[10]);
						printk("mchp_3d_probe: FW.Rev Version: %d.%d.%d.%d.%d\n", fw_buffer[42], fw_buffer[43], fw_buffer[44], fw_buffer[45], fw_buffer[46]);
						break;
						
					default:
						break;	
				}					   
			} else {
				
					printk("mchp_3d_probe: Msg size - 0x%02x Msg ID: 0x%02x\n", fw_buffer[0], fw_buffer[3]);
			}
			
			/*
			//For debug
			if (msg_size > buf_size)
			{
				msg_size = buf_size;
			}
			
			
			for (i=0; i<msg_size; i++)
			{
				printk("mchp_3d_probe: Msg bytes - 0x%02x \n", fw_buffer[i]);		
			}
			*/
			
			i2c_master_recv (priv->client, &fw_buffer[0], buf_size);	//Read full message up to 46 bytes
			
			msg_size = fw_buffer[0];
			loopcount --;
			
		} else {
			loopcount = -1;
		}	
	}
	enable_irq(client->irq);
	printk("mchp_3d_probe: Enable IRQ\n");	
	
	i2c_master_send (priv->client, setGestIDFlags_Enable, sizeof(setGestIDFlags_Enable));
	printk("mchp_3d_probe: Enable MGC3140 Outputs\n");
	

	
    return 0;

error:

    /* A critical component component has failed to initialize.  We must unload. */
	
    if (priv->input)
    {
        input_free_device(priv->input);
    }

    if (priv)
    {
        kfree(priv);
    }
						
    return ret;

}

static int mchp_3d_i2c_remove(struct i2c_client *client)
{
    struct mchp_3d_i2c_priv *priv = (struct mchp_3d_i2c_priv *)i2c_get_clientdata(client);

    sysfs_remove_group(&client->dev.kobj, &attr_group);

    if (client->irq) 
    {
        free_irq(client->irq, priv);
    }
    
    input_unregister_device(priv->input);
//    kfree(priv);
    devm_kfree(&client->dev,priv);// added, freeng mem. allocated by devm_kzalloc. with kfree() we canot insmod and rmmod multiple times.
    return 0;
}

/*
 * The device tree based i2c loader looks for
 * "i2c:" + second_component_of(property("compatible"))
 * and therefore we need an alias to be found.
 */
static const struct i2c_device_id mchp_3d_i2c_id[] = {
    { "mchp_3d_i2c", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, mchp_3d_i2c_id);

#ifdef CONFIG_OF
static const struct of_device_id mchp_3d_i2c_dt_ids[] = {
        { .compatible = "microchip,mchp_3d_i2c", },
        { }
};
MODULE_DEVICE_TABLE(of, mchp_3d_i2c_dt_ids);

#endif

static struct i2c_driver mchp_3d_i2c_driver = {
	
	
	.driver = {
        .name   = "mchp_3d_i2c",
        .owner  = THIS_MODULE,
		.of_match_table = of_match_ptr(mchp_3d_i2c_dt_ids)
    },
	
    .probe      = mchp_3d_i2c_probe,
    .remove     = mchp_3d_i2c_remove,
    .id_table   = mchp_3d_i2c_id,
};


/******************************************************************************
Function:
    mchp_3d_i2c_init()

Description:
    This function is called during startup even if the platform specific
    files have not been setup yet.
******************************************************************************/
static int __init mchp_3d_i2c_init(void)
{
    printk("mchp_3d_i2c: mchp_3d_i2c_init: begin\n");
    
    return i2c_add_driver(&mchp_3d_i2c_driver);
}

/******************************************************************************
Function:
    mchp_3d_i2c_exit()

Description:
    This function is called after mchp_3d_i2c_remove() immediately before 
    being removed from the kernel.
******************************************************************************/
static void __exit mchp_3d_i2c_exit(void)
{
    printk("MCHP_3D I2C: mchp_3d_i2c_exit begin\n");
    
    
    i2c_del_driver(&mchp_3d_i2c_driver);
}

/* Enable the mchp_3d_i2c_init() to be run by the kernel during initialization */
module_init(mchp_3d_i2c_init);

/* Enables the mchp_3d_i2c_exit() to be called during cleanup.  This only
has an effect if the driver is compiled as a kernel module. */
module_exit(mchp_3d_i2c_exit);

MODULE_AUTHOR("Steve Grahovac");
MODULE_DESCRIPTION("MCHP 3D touchscreen I2C bus driver");
MODULE_LICENSE("GPL");
