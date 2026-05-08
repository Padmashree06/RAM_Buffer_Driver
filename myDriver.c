// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/suspend.h>

#define DEVICE_NAME "my_ram_buffer"
#define WRITE_BLOCK_SIZE (256 * 1024)
#define TOTAL_BLOCKS 4096
#define STOP_THRESHOLD 3600
#define START_THRESHOLD 2800
#define DISK_SPACE "/home/ssd_disk.bin"

static char *block_pool[TOTAL_BLOCKS];
static int write_ptr;
static int read_ptr;
static int blocks_filled;
static int major_number;
static bool is_emergency;
static struct notifier_block power_nb;
static struct task_struct *ssd_flush_thread;
static DECLARE_WAIT_QUEUE_HEAD(ram_wait_q);
static DEFINE_MUTEX(buffer_lock);

/* When battery is low, RAM Buffer is emptied and writes directly to disk */
static int power_callback(struct notifier_block *nb, unsigned long action,
			  void *data)
{
	if (action == PM_SUSPEND_PREPARE || action == PM_HIBERNATION_PREPARE) {
		pr_alert("RAM_BUFFER_DRIVER: EMERGENCY! Switching to direct-write.\n");
		is_emergency = true;
		wake_up_process(ssd_flush_thread);
	}
	return NOTIFY_OK;
}

static int ssd_flush_worker(void *data)
{
	struct file *filp;
	loff_t pos = 0;
	ssize_t ret;

	filp = filp_open(DISK_SPACE, O_WRONLY | O_CREAT | O_APPEND | O_LARGEFILE, 0644);
	if (IS_ERR(filp)) {
		pr_err("RAM_BUFFER_DRIVER: Failed to open SATA disk file!\n");
		return PTR_ERR(filp);
	}

	pr_info("RAM_BUFFER_DRIVER: SSD Worker started.\n");

	while (!kthread_should_stop()) {
		mutex_lock(&buffer_lock);
		if (blocks_filled > 0) {
			char *block_to_write = block_pool[read_ptr];

			mutex_unlock(&buffer_lock);

			ret = kernel_write(filp, block_to_write, WRITE_BLOCK_SIZE, &pos);
			if (ret < 0)
				pr_err("RAM_BUFFER_DRIVER: Write error: %zd\n", ret);
			else
				pr_info("RAM_DRIVER: Flushed. Left: %d\n", blocks_filled);

			mutex_lock(&buffer_lock);
			read_ptr = (read_ptr + 1) % TOTAL_BLOCKS;
			blocks_filled--;

			if (blocks_filled <= START_THRESHOLD)
				wake_up_interruptible(&ram_wait_q);
		}
		mutex_unlock(&buffer_lock);

		if (blocks_filled == 0) {
			set_current_state(TASK_INTERRUPTIBLE);
			if (kthread_should_stop())
				break;
			schedule();
		}
	}

	filp_close(filp, NULL);
	return 0;
}

static ssize_t dev_write(struct file *file, const char __user *buf,
			 size_t len, loff_t *ppos)
{
	size_t to_copy;

	if (unlikely(is_emergency)) {
		pr_info("RAM_BUFFER_DRIVER: Direct write due to emergency.\n");
		return len;
	}

	to_copy = (len > WRITE_BLOCK_SIZE) ? WRITE_BLOCK_SIZE : len;
	wait_event_interruptible(ram_wait_q, blocks_filled < STOP_THRESHOLD);

	mutex_lock(&buffer_lock);
	if (copy_from_user(block_pool[write_ptr], buf, to_copy)) {
		mutex_unlock(&buffer_lock);
		return -EFAULT;
	}

	write_ptr = (write_ptr + 1) % TOTAL_BLOCKS;
	blocks_filled++;
	mutex_unlock(&buffer_lock);

	if (ssd_flush_thread)
		wake_up_process(ssd_flush_thread);

	return to_copy;
}

static const struct file_operations fops = {
	.owner = THIS_MODULE,
	.write = dev_write,
};

static int __init ram_buffer_init(void)
{
	int i;

	power_nb.notifier_call = power_callback;
	register_pm_notifier(&power_nb);

	major_number = register_chrdev(0, DEVICE_NAME, &fops);
	if (major_number < 0)
		return major_number;

	for (i = 0; i < TOTAL_BLOCKS; i++) {
		block_pool[i] = vmalloc(WRITE_BLOCK_SIZE);
		if (!block_pool[i])
			goto fail;
	}

	ssd_flush_thread = kthread_run(ssd_flush_worker, NULL, "ssd_flusher");
	pr_info("Driver loaded! Major: %d\n", major_number);
	return 0;

fail:
	while (--i >= 0)
		vfree(block_pool[i]);
	unregister_chrdev(major_number, DEVICE_NAME);
	return -ENOMEM;
}

static void __exit ram_buffer_exit(void)
{
	int i;

	if (ssd_flush_thread)
		kthread_stop(ssd_flush_thread);

	for (i = 0; i < TOTAL_BLOCKS; i++)
		vfree(block_pool[i]);

	unregister_pm_notifier(&power_nb);
	unregister_chrdev(major_number, DEVICE_NAME);
	pr_info("RAM Buffer Driver unloaded\n");
}

module_init(ram_buffer_init);
module_exit(ram_buffer_exit);

MODULE_LICENSE("GPL");
