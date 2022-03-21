#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/ioctl.h>
#include <linux/err.h>

#define WIEGAND1_DATA0_NAME	"wiegand1_input0"
#define WIEGAND1_DATA1_NAME	"wiegand1_input1"
#define WIEGAND2_DATA0_NAME	"wiegand2_input0"
#define WIEGAND2_DATA1_NAME	"wiegand2_input1"

#define WIEGAND_MAX_BUFFER_SIZE	4096

#define FIONREAD 0x541B
#define TIOCEXCL 0x540C
#define TIOCNXCL 0x540D

short exclusive=0;
unsigned long timer_interval_ns = 0x2FAF080;
unsigned long timer_interval_s = 0;
static struct hrtimer hr_timer1, hr_timer2;
ktime_t interval;
int timer1_started=0;
int timer2_started=0;
int file_instances_opened=0;
char tmp_string_fromat[38];
char tmp_read_buffer[39];
int sprintf_retval=0;
unsigned long file_pointer=0;
struct mutex write_buffer_mutex;
struct mutex read_pointer_mutex;
struct mutex open_counter_mutex;
wait_queue_head_t read_buf_wq;
static struct gpio_desc *wiegand1_input0;
static struct gpio_desc *wiegand1_input1;
static struct gpio_desc *wiegand2_input0;
static struct gpio_desc *wiegand2_input1;


struct wiegand_flip_buffer {
	char *buffer_a;
	char *buffer_b;
	char *write_buffer;
	char *read_buffer;
	unsigned int read_pointer;
	unsigned int write_pointer;
};

void flip_buffer_init(struct wiegand_flip_buffer *buf)
{
	buf->buffer_a=kmalloc(WIEGAND_MAX_BUFFER_SIZE, GFP_KERNEL);
	buf->buffer_b=kmalloc(WIEGAND_MAX_BUFFER_SIZE, GFP_KERNEL);
	buf->write_buffer=buf->buffer_a;
	buf->read_buffer=buf->buffer_a;
	buf->read_pointer=0;
	buf->write_pointer=0;
}

struct wiegand_flip_buffer flip_buffer;

void flip_write_buffer(struct wiegand_flip_buffer *buf)
{
	if(buf->write_buffer==buf->buffer_b) {
		buf->write_buffer=buf->buffer_a;
	} else {
		buf->write_buffer=buf->buffer_b;
	}
	buf->write_pointer=0;
}

void flip_read_buffer(struct wiegand_flip_buffer *buf)
{
	if(buf->read_buffer==buf->buffer_b) {
		buf->read_buffer=buf->buffer_a;
	} else {
		buf->read_buffer=buf->buffer_b;
	}
	buf->read_pointer=0;
}

uint32_t get_buffer_used_space(struct wiegand_flip_buffer *buf)
{
	if(buf->write_buffer != buf->read_buffer) {
		return (WIEGAND_MAX_BUFFER_SIZE - buf->read_pointer) +
		       buf->write_pointer;
	} else {
		return buf->write_pointer - buf->read_pointer;
	}
}

uint32_t get_buffer_free_space(struct wiegand_flip_buffer *buf)
{
	if(buf->write_buffer != buf->read_buffer) {
		return WIEGAND_MAX_BUFFER_SIZE - buf->write_pointer;
	} else {
		return WIEGAND_MAX_BUFFER_SIZE +
		       (WIEGAND_MAX_BUFFER_SIZE - buf->write_pointer);
	}
}

ssize_t copy_readbuf_data(size_t size, char __user *data, struct wiegand_flip_buffer *buf)
{
	int tmp_size = 0;
	size_t ret_value = 0;
	if (buf->read_pointer+size>WIEGAND_MAX_BUFFER_SIZE) {
		if(buf->write_buffer!=buf->read_buffer) {
			tmp_size=WIEGAND_MAX_BUFFER_SIZE - buf->read_pointer;
			ret_value = copy_to_user(data, buf->read_buffer + buf->read_pointer, sizeof(char)*tmp_size);
			if (ret_value)
				return ret_value;
			//memcpy(decoupler_buffer, buf->read_buffer + buf->read_pointer, sizeof(char)*tmp_size);
			buf->read_pointer=WIEGAND_MAX_BUFFER_SIZE;
			flip_read_buffer(buf);
			ret_value = copy_to_user(data + tmp_size, buf->read_buffer + buf->read_pointer, sizeof(char)*(size-tmp_size));
			if (ret_value)
				return ret_value;
			//memcpy(decoupler_buffer + tmp_size, buf->read_buffer + buf->read_pointer, sizeof(char)*(size-tmp_size));
			buf->read_pointer+=(size-tmp_size);
		}
	} else {
		ret_value = copy_to_user(data, buf->read_buffer + buf->read_pointer, sizeof(char)*size);
		if (ret_value)
			return ret_value;
		//memcpy(decoupler_buffer, buf->read_buffer + buf->read_pointer, sizeof(char)*size);
		buf->read_pointer+=size;
	}
	return size;
}

ssize_t fill_writebuf_data(int size, char *data, struct wiegand_flip_buffer *buf)
{
	int tmp_size=0;
	if (buf->write_pointer+size>WIEGAND_MAX_BUFFER_SIZE) {
		if(buf->write_buffer==buf->read_buffer) {
			tmp_size=WIEGAND_MAX_BUFFER_SIZE - buf->write_pointer;
			memcpy(buf->write_buffer + buf->write_pointer, data, sizeof(char)*tmp_size);
			buf->write_pointer=WIEGAND_MAX_BUFFER_SIZE;
			flip_write_buffer(buf);
			memcpy(buf->write_buffer + buf->write_pointer, data + tmp_size, sizeof(char)*(size-tmp_size));
			buf->write_pointer+=(size-tmp_size);
		}
	} else {
		memcpy(buf->write_buffer + buf->write_pointer, data, sizeof(char)*size);
		buf->write_pointer+=size;
	}
	return size;
}

void flush_readbuf_data(struct wiegand_flip_buffer *buf)
{
	buf->read_buffer=buf->write_buffer;
	buf->read_pointer=buf->write_pointer;
}

ssize_t write_kdata_buffer(size_t count, char *data, struct wiegand_flip_buffer *buf)
{
	int done=0;
	while(!done) {
		/* TODO: check write lock */
		if(count<get_buffer_free_space(buf)) {
			fill_writebuf_data(count,data,buf);
			//printk(KERN_DEBUG"Wiegand: write_kdata_buffer, data %s written! Pointer to %d\n",data,buf->write_pointer);
			done=1;
		} else {
			printk(KERN_WARNING"Wiegand: write_kdata_buffer, buffer is full %d, dropping data!\n",get_buffer_free_space(buf));
			done=1;
			break; /* drop data if buffer is full */
		}
	}
	return count;
}

static int user_misc_open(struct inode *inode, struct file *file)
{
	if(exclusive&&file_instances_opened>0)
		return -EBUSY;
	
	if(file_instances_opened==0) {
		mutex_lock(&write_buffer_mutex);
		printk(KERN_INFO "Wiegand: Flushing read buffer when opening first instance\n");
		flush_readbuf_data(&flip_buffer);
		mutex_unlock(&write_buffer_mutex);
	}
	mutex_lock(&open_counter_mutex);
	file_instances_opened++;
	printk(KERN_INFO "Wiegand: new file instance opened. %d total\n", file_instances_opened);
	mutex_unlock(&open_counter_mutex);
	return 0;
}

static int user_misc_close(struct inode *inodep, struct file *filp)
{
	mutex_lock(&open_counter_mutex);
	file_instances_opened--;
	printk(KERN_INFO "Wiegand: file instance closed. %d remaining\n", file_instances_opened);
	mutex_unlock(&open_counter_mutex);
	return 0;
}

/*
 * static ssize_t user_misc_write(struct file *file, const char __user *buf,
 * 			      size_t len, loff_t *ppos)
 * {
 * 	return len; 
 * }
 */

static ssize_t user_misc_read(struct file *filp, char __user *buf,
			     size_t count, loff_t *f_pos)
{
	ssize_t retval;
	int mutex_interrupt, wq_interrupt;
	uint32_t to_be_read=0;
	
	mutex_interrupt=mutex_lock_interruptible(&read_pointer_mutex);
	if (mutex_interrupt)
		return mutex_interrupt;
	
	wq_interrupt=wait_event_interruptible(read_buf_wq, (to_be_read=get_buffer_used_space(&flip_buffer))>0);
	if (wq_interrupt) {
		mutex_unlock(&read_pointer_mutex);
		return wq_interrupt;
	}
	//printk(KERN_DEBUG"Wiegand: Reading %d bytes, available: %d\n",count,to_be_read);
	if(to_be_read>count) {
		to_be_read=count;
	}
	retval=copy_readbuf_data(to_be_read, buf, &flip_buffer);	

	mutex_unlock(&read_pointer_mutex);
	return retval;
}

static long user_misc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long ret_val=-ENOTTY;
	//printk(KERN_DEBUG"Wiegand: IOCTL: %d, %d\n", cmd, FIONREAD);
	switch(cmd) {
                case FIONREAD:
			if(mutex_trylock(&read_pointer_mutex)) {
				uint32_t winsize=get_buffer_used_space(&flip_buffer);
				//printk(KERN_DEBUG"Wiegand: Window size: %d\n", winsize);
				ret_val = copy_to_user((uint32_t *) arg, &winsize, sizeof (uint32_t)) ? -EFAULT : 0;
				mutex_unlock(&read_pointer_mutex);
			} else {
				ret_val = -EBUSY;
			}
			break;
		case TIOCEXCL:
			exclusive=1;
			ret_val=0;
			break;
		case TIOCNXCL:
			exclusive=0;
			ret_val=0;
			break;
        }
	return ret_val;
}

static const struct file_operations wiegand_fops = {
	.owner		= THIS_MODULE,
	/*.write		= user_misc_write,*/
	.read		= user_misc_read,
	.open		= user_misc_open,
	.release	= user_misc_close,
	.unlocked_ioctl = user_misc_ioctl,
	/*.llseek		= no_llseek,*/
};

static struct miscdevice wiegand_miscdev = {
	.minor		= MISC_DYNAMIC_MINOR,
	.name		= "wiegand",
	.fops		= &wiegand_fops,
};

struct wiegand {
	int startParity;
	int readNum;
	unsigned int datas[4];
	int currentBit;
	int num;
	unsigned int lastFacilityCode;
	unsigned int lastCardNumber;
}; 

static struct wiegand wiegand1;
static struct wiegand wiegand2;

irqreturn_t wiegand1_data0_isr(int irq, void *dev_id);
irqreturn_t wiegand1_data1_isr(int irq, void *dev_id);
irqreturn_t wiegand2_data0_isr(int irq, void *dev_id);
irqreturn_t wiegand2_data1_isr(int irq, void *dev_id);

void wiegand_clear(struct wiegand *w)
{
	w->currentBit = 0;
	w->startParity = 0;
	w->datas[0]=0;
	w->datas[1]=0;
	w->datas[2]=0;
	w->datas[3]=0;
}

void wiegand_init(struct wiegand *w, int num)
{
	w->lastFacilityCode = 0;
	w->lastCardNumber = 0;
	w->datas[0]=0;
	w->datas[1]=0;
	w->datas[2]=0;
	w->datas[3]=0;
	w->readNum = 0;
	w->num=num;
	wiegand_clear(w);
}

enum hrtimer_restart timer1_callback(struct hrtimer *timer_for_restart)
{
	timer1_started=0;
	mutex_lock(&write_buffer_mutex);
	if (file_instances_opened>0) {
		sprintf_retval=sprintf(tmp_string_fromat,
			"01%02X%08X%08X%08X%08X\r\n",
			wiegand1.currentBit,
			wiegand1.datas[0],wiegand1.datas[1],
			wiegand1.datas[2],wiegand1.datas[3]);
		write_kdata_buffer(38, tmp_string_fromat, &flip_buffer);
	}
	wiegand_clear(&wiegand1);
	mutex_unlock(&write_buffer_mutex);
	wake_up_interruptible_sync(&read_buf_wq);
	return HRTIMER_NORESTART;
}

enum hrtimer_restart timer2_callback(struct hrtimer *timer_for_restart)
{
	timer2_started=0;
	mutex_lock(&write_buffer_mutex);
	if (file_instances_opened>0) {
		sprintf_retval=sprintf(tmp_string_fromat,
			"02%02X%08X%08X%08X%08X\r\n",
			wiegand2.currentBit,
			wiegand2.datas[0],wiegand2.datas[1],
			wiegand2.datas[2],wiegand2.datas[3]);
		write_kdata_buffer(38, tmp_string_fromat, &flip_buffer);
	}
	wiegand_clear(&wiegand2);
	mutex_unlock(&write_buffer_mutex);
	wake_up_interruptible_sync(&read_buf_wq);
	return HRTIMER_NORESTART;
}

static int wiegand_setup(struct platform_device *pdev){	
	printk(KERN_INFO"Wiegand: Init data structures\n");
	
	mutex_init(&write_buffer_mutex);
	mutex_init(&read_pointer_mutex);
	mutex_init(&open_counter_mutex);
	init_waitqueue_head(&read_buf_wq);
	
	wiegand_init(&wiegand1,1);
	wiegand_init(&wiegand2,2);
	flip_buffer_init(&flip_buffer);
	
	hrtimer_init(&hr_timer1, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hrtimer_init(&hr_timer2, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	
	printk(KERN_INFO"Wiegand: Setting Up GPIOs\n");
	
	wiegand1_input0=gpiod_get(&pdev->dev,WIEGAND1_DATA0_NAME, GPIOD_IN);
	if (IS_ERR(wiegand1_input0)) {
		printk(KERN_ERR"Wiegand: Could not get %s as input. Error: %ld, Please, check you device tree!\n",WIEGAND1_DATA0_NAME,PTR_ERR(wiegand1_input0));
		return PTR_ERR(wiegand1_input0);
	} else {
		//kernel docs says that gpiod_get will never return null pointer...will be true?
		printk(KERN_DEBUG"Wiegand: GPIO %d correctly set as %s input.\n",desc_to_gpio(wiegand1_input0),WIEGAND1_DATA0_NAME);
	}
	wiegand1_input1=gpiod_get(&pdev->dev,WIEGAND1_DATA1_NAME, GPIOD_IN);
	if (IS_ERR(wiegand1_input1)) {
		printk(KERN_ERR"Wiegand: Could not get %s as input. Error: %ld, Please, check you device tree!\n",WIEGAND1_DATA1_NAME,PTR_ERR(wiegand1_input1));
		return PTR_ERR(wiegand1_input1);
	} else {
		//kernel docs says that gpiod_get will never return null pointer...will be true?
		printk(KERN_DEBUG"Wiegand: GPIO %d correctly set as %s input.\n",desc_to_gpio(wiegand1_input1),WIEGAND1_DATA1_NAME);
	}
	wiegand2_input0=gpiod_get(&pdev->dev,WIEGAND2_DATA0_NAME, GPIOD_IN);
	if (IS_ERR(wiegand2_input0)) {
		printk(KERN_ERR"Wiegand: Could not get %s as input. Error: %ld, Please, check you device tree!\n",WIEGAND2_DATA0_NAME,PTR_ERR(wiegand2_input0));
		return PTR_ERR(wiegand2_input0);
	} else {
		//kernel docs says that gpiod_get will never return null pointer...will be true?
		printk(KERN_DEBUG"Wiegand: GPIO %d correctly set as %s input.\n",desc_to_gpio(wiegand2_input0),WIEGAND2_DATA0_NAME);
	}
	wiegand2_input1=gpiod_get(&pdev->dev,WIEGAND2_DATA1_NAME, GPIOD_IN);
	if (IS_ERR(wiegand2_input1)) {
		printk(KERN_ERR"Wiegand: Could not get %s as input. Error: %ld, Please, check you device tree!\n",WIEGAND2_DATA1_NAME,PTR_ERR(wiegand2_input1));
		return PTR_ERR(wiegand2_input1);
	} else {
		//kernel docs says that gpiod_get will never return null pointer...will be true?
		printk(KERN_DEBUG"Wiegand: GPIO %d correctly set as %s input.\n",desc_to_gpio(wiegand2_input1),WIEGAND2_DATA1_NAME);
	}
	
	if(request_irq(gpiod_to_irq(wiegand1_input0), wiegand1_data0_isr, IRQF_TRIGGER_FALLING, WIEGAND1_DATA0_NAME, &wiegand1)) //pin 0
	{
		printk(KERN_ERR"Wiegand: Can't register IRQ %d for GPIO %d\n", gpiod_to_irq(wiegand1_input0), desc_to_gpio(wiegand1_input0));
		return -EIO;
	}
	
	if(request_irq(gpiod_to_irq(wiegand1_input1), wiegand1_data1_isr, IRQF_TRIGGER_FALLING, WIEGAND1_DATA1_NAME, &wiegand1)) //pin 1
	{
		printk(KERN_ERR"Wiegand: Can't register IRQ %d for GPIO %d\n", gpiod_to_irq(wiegand1_input1), desc_to_gpio(wiegand1_input1));
		return -EIO;
	}
	
	if(request_irq(gpiod_to_irq(wiegand2_input0), wiegand2_data0_isr, IRQF_TRIGGER_FALLING, WIEGAND2_DATA0_NAME, &wiegand2)) //pin 0
	{
		printk(KERN_ERR"Wiegand: Can't register IRQ %d for GPIO %d\n", gpiod_to_irq(wiegand2_input0), desc_to_gpio(wiegand2_input0));
		return -EIO;
	}
	
	if(request_irq(gpiod_to_irq(wiegand2_input1), wiegand2_data1_isr, IRQF_TRIGGER_FALLING, WIEGAND2_DATA1_NAME, &wiegand2)) //pin 1
	{
		printk(KERN_ERR"Wiegand: Can't register IRQ %d for GPIO %d\n", gpiod_to_irq(wiegand2_input1), desc_to_gpio(wiegand2_input1));
		return -EIO;
	}
	
	interval=ktime_set( timer_interval_s, timer_interval_ns );
	
	printk(KERN_INFO"Wiegand: Creating /dev/wiegand\n");
	return misc_register(&wiegand_miscdev);
}

irqreturn_t wiegand1_data0_isr(int irq, void *dev_id)
{
	struct wiegand *w = (struct wiegand *)dev_id;

	if (timer1_started)
	{
		ktime_t delta;
		ktime_t now;
		now = hrtimer_cb_get_time(&hr_timer1);
		delta = ktime_sub(now, hrtimer_get_expires(&hr_timer1));
		hrtimer_add_expires(&hr_timer1, ktime_add(interval, delta));
		hrtimer_restart(&hr_timer1);
	}
	
	w-> datas[0]<<=1;
	w-> datas[0]|=(w-> datas[1]>>31);
	w-> datas[1]<<=1;
	w-> datas[1]|=(w-> datas[2]>>31);
	w-> datas[2]<<=1;
	w-> datas[2]|=(w-> datas[3]>>31);
	w-> datas[3]<<=1;
	
	w->currentBit++;
	
	if (!timer1_started)
	{
		timer1_started=1;
		hrtimer_init( &hr_timer1, CLOCK_MONOTONIC, HRTIMER_MODE_REL );
		hr_timer1.function = &timer1_callback;
		hrtimer_start( &hr_timer1, interval, HRTIMER_MODE_REL );
	}
	
	return IRQ_HANDLED;
}

irqreturn_t wiegand1_data1_isr(int irq, void *dev_id)
{	
	struct wiegand *w = (struct wiegand *)dev_id;  
	
	if (timer1_started)
	{
		ktime_t delta;
		ktime_t now;
		now = hrtimer_cb_get_time(&hr_timer1);
		delta = ktime_sub(now, hrtimer_get_expires(&hr_timer1));
		hrtimer_add_expires(&hr_timer1, ktime_add(interval, delta));
		hrtimer_restart(&hr_timer1);
	}
	
	w-> datas[0]<<=1;
	w-> datas[0]|=(w-> datas[1]>>31);
	w-> datas[1]<<=1;
	w-> datas[1]|=(w-> datas[2]>>31);
	w-> datas[2]<<=1;
	w-> datas[2]|=(w-> datas[3]>>31);
	w-> datas[3]<<=1;
	w-> datas[3]|=1;

	w->currentBit++;
	
	if (!timer1_started)
	{
		timer1_started=1;
		hrtimer_init( &hr_timer1, CLOCK_MONOTONIC, HRTIMER_MODE_REL );
		hr_timer1.function = &timer1_callback;
		hrtimer_start( &hr_timer1, interval, HRTIMER_MODE_REL );
	}
	 
	return IRQ_HANDLED;
}

irqreturn_t wiegand2_data0_isr(int irq, void *dev_id)
{
	struct wiegand *w = (struct wiegand *)dev_id;
	
	if (timer2_started)
	{
		ktime_t delta;
		ktime_t now;
		now = hrtimer_cb_get_time(&hr_timer2);
		delta = ktime_sub(now, hrtimer_get_expires(&hr_timer2));
		hrtimer_add_expires(&hr_timer2, ktime_add(interval, delta));
		hrtimer_restart(&hr_timer2);
	}
	
	w-> datas[0]<<=1;
	w-> datas[0]|=(w-> datas[1]>>31);
	w-> datas[1]<<=1;
	w-> datas[1]|=(w-> datas[2]>>31);
	w-> datas[2]<<=1;
	w-> datas[2]|=(w-> datas[3]>>31);
	w-> datas[3]<<=1;
	
	w->currentBit++;
	
	if (!timer2_started)
	{
		timer2_started=1;
		hrtimer_init( &hr_timer2, CLOCK_MONOTONIC, HRTIMER_MODE_REL );
		hr_timer2.function = &timer2_callback;
		hrtimer_start( &hr_timer2, interval, HRTIMER_MODE_REL );
	}
	
	return IRQ_HANDLED;
}

irqreturn_t wiegand2_data1_isr(int irq, void *dev_id)
{	
	struct wiegand *w = (struct wiegand *)dev_id;  
	
	if (timer2_started)
	{
		ktime_t delta;
		ktime_t now;
		now = hrtimer_cb_get_time(&hr_timer2);
		delta = ktime_sub(now, hrtimer_get_expires(&hr_timer2));
		hrtimer_add_expires(&hr_timer2, ktime_add(interval, delta));
		hrtimer_restart(&hr_timer2);
	}
	
	w-> datas[0]<<=1;
	w-> datas[0]|=(w-> datas[1]>>31);
	w-> datas[1]<<=1;
	w-> datas[1]|=(w-> datas[2]>>31);
	w-> datas[2]<<=1;
	w-> datas[2]|=(w-> datas[3]>>31);
	w-> datas[3]<<=1;
	w-> datas[3]|=1;

	w->currentBit++;
	
	if (!timer2_started)
	{
		timer2_started=1;
		hrtimer_init( &hr_timer2, CLOCK_MONOTONIC, HRTIMER_MODE_REL );
		hr_timer2.function = &timer2_callback;
		hrtimer_start( &hr_timer2, interval, HRTIMER_MODE_REL );
	}

	return IRQ_HANDLED;
}

static int wiegand_exit(struct platform_device *pdev){
	printk(KERN_INFO"Wiegand: Destroying /dev/wiegand\n");
	misc_deregister(&wiegand_miscdev);
	printk(KERN_INFO"Wiegand: Deleting timers\n");
	hrtimer_cancel(&hr_timer1);
	hrtimer_cancel(&hr_timer2);
	printk(KERN_INFO"Wiegand: Freeing Up GPIOs\n");
	free_irq(gpiod_to_irq(wiegand1_input0), &wiegand1);
	free_irq(gpiod_to_irq(wiegand1_input1), &wiegand1);
	free_irq(gpiod_to_irq(wiegand2_input0), &wiegand2);
	free_irq(gpiod_to_irq(wiegand2_input1), &wiegand2);
	gpiod_put(wiegand1_input0);
	gpiod_put(wiegand1_input1);
	gpiod_put(wiegand2_input0);
	gpiod_put(wiegand2_input1);
	printk(KERN_INFO"Wiegand: Module Unloaded, BYE!\n");
	return 0;
}

static const struct of_device_id wiegand_match_table[] = {
	{ .compatible = "trexom,wiegand" },
	{},
};

static struct platform_driver wiegand_driver = {
	//.probe = wiegand_setup,
	.remove = wiegand_exit,
	.driver = {
		.name = "trexom,wiegand",
		.of_match_table = wiegand_match_table,
	}
};

static int __init platform_setup(void){
	printk(KERN_INFO"Wiegand: Registering and probing platform driver\n");
	return platform_driver_probe(&wiegand_driver,wiegand_setup);
}

static void __exit platform_exit(void){
	printk(KERN_INFO"Wiegand: Unregistering platform driver\n");
	platform_driver_unregister(&wiegand_driver);
}

module_init(platform_setup);
module_exit(platform_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Misc Wiegand Module for Trexom Cosmo tx151v3.1");
MODULE_AUTHOR("Marjan Pascolo");
MODULE_VERSION("1:1.3");
