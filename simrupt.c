/* kmldrv: A kernel module that performs tic-tac-toe game between machine
 * learning algorithms */

#include <linux/cdev.h>
#include <linux/circ_buf.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kfifo.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#include "game.h"
#include "mcts.h"
#include "negamax.h"

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("A device that simulates interrupts");

/* Macro DECLARE_TASKLET_OLD exists for compatibiity.
 * See https://lwn.net/Articles/830964/
 */
#ifndef DECLARE_TASKLET_OLD
#define DECLARE_TASKLET_OLD(arg1, arg2) DECLARE_TASKLET(arg1, arg2, 0L)
#endif

#define DEV_NAME "kmldrv"

#define NR_KMLDRV 1

static int delay = 100; /* time (in ms) to generate an event */

/* Data produced by the simulated device */

/* Timer to simulate a periodic IRQ */
static struct timer_list timer;

/* Character device stuff */
static int major;
static struct class *kmldrv_class;
static struct cdev kmldrv_cdev;

static char draw_buffer[DRAWBUFFER_SIZE];

/* Data are stored into a kfifo buffer before passing them to the userspace */
static DECLARE_KFIFO_PTR(rx_fifo, unsigned char);

/* NOTE: the usage of kfifo is safe (no need for extra locking), until there is
 * only one concurrent reader and one concurrent writer. Writes are serialized
 * from the interrupt context, readers are serialized using this mutex.
 */
static DEFINE_MUTEX(read_lock);

/* Wait queue to implement blocking I/O from userspace */
static DECLARE_WAIT_QUEUE_HEAD(rx_wait);

/* Insert the whole chess board into the kfifo buffer */
static void produce_board(void)
{
    unsigned int len = kfifo_in(&rx_fifo, draw_buffer, sizeof(draw_buffer));
    if (unlikely(len < sizeof(draw_buffer)) && printk_ratelimit())
        pr_warn("%s: %zu bytes dropped\n", __func__, sizeof(draw_buffer) - len);

    pr_debug("kmldrv: %s: in %u/%u bytes\n", __func__, len,
             kfifo_len(&rx_fifo));
}

/* Mutex to serialize kfifo writers within the workqueue handler */
static DEFINE_MUTEX(producer_lock);

/* Mutex to serialize fast_buf consumers: we can use a mutex because consumers
 * run in workqueue handler (kernel thread context).
 */
static DEFINE_MUTEX(consumer_lock);

/* We use an additional "faster" circular buffer to quickly store data from
 * interrupt context, before adding them to the kfifo.
 */
static struct circ_buf fast_buf;


static char table[N_GRIDS];

/* Draw the board into draw_buffer */
static int draw_board(char *table)
{
    int i = 0, k = 0;
    draw_buffer[i++] = '\n';
    smp_wmb();
    draw_buffer[i++] = '\n';
    smp_wmb();

    while (i < DRAWBUFFER_SIZE) {
        for (int j = 0; j < (BOARD_SIZE << 1) - 1 && k < N_GRIDS; j++) {
            draw_buffer[i++] = j & 1 ? '|' : table[k++];
            smp_wmb();
        }
        draw_buffer[i++] = '\n';
        smp_wmb();
        for (int j = 0; j < (BOARD_SIZE << 1) - 1; j++) {
            draw_buffer[i++] = '-';
            smp_wmb();
        }
        draw_buffer[i++] = '\n';
        smp_wmb();
    }


    return 0;
}

/* Clear all data from the circular buffer fast_buf */
static void fast_buf_clear(void)
{
    fast_buf.head = fast_buf.tail = 0;
}

/* Workqueue handler: executed by a kernel thread */
static void simrupt_work_func(struct work_struct *w)
{
    int cpu;

    /* This code runs from a kernel thread, so softirqs and hard-irqs must
     * be enabled.
     */
    WARN_ON_ONCE(in_softirq());
    WARN_ON_ONCE(in_interrupt());

    /* Pretend to simulate access to per-CPU data, disabling preemption
     * during the pr_info().
     */
    cpu = get_cpu();
    pr_info("kmldrv: [CPU#%d] %s\n", cpu, __func__);
    put_cpu();

    mutex_lock(&producer_lock);
    draw_board(table);
    mutex_unlock(&producer_lock);

    /* Store data to the kfifo buffer */
    // mutex_lock(&producer_lock);
    mutex_lock(&consumer_lock);
    produce_board();
    mutex_unlock(&consumer_lock);
    // mutex_unlock(&producer_lock);

    wake_up_interruptible(&rx_wait);
}

static char turn;
static int finish;

static void ai_one_work_func(struct work_struct *w)
{
    ktime_t tv_start, tv_end;
    s64 nsecs;

    int cpu;

    WARN_ON_ONCE(in_softirq());
    WARN_ON_ONCE(in_interrupt());

    cpu = get_cpu();
    pr_info("kmldrv: [CPU#%d] start doing %s\n", cpu, __func__);
    tv_start = ktime_get();
    mutex_lock(&producer_lock);
    int move;
    WRITE_ONCE(move, mcts(table, 'O'));

    smp_mb();

    if (move != -1)
        WRITE_ONCE(table[move], 'O');

    WRITE_ONCE(turn, 'X');
    WRITE_ONCE(finish, 1);
    smp_wmb();
    mutex_unlock(&producer_lock);
    tv_end = ktime_get();

    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));
    pr_info("kmldrv: [CPU#%d] doing %s for %llu usec\n", cpu, __func__,
            (unsigned long long) nsecs >> 10);
    put_cpu();
}

static void ai_two_work_func(struct work_struct *w)
{
    ktime_t tv_start, tv_end;
    s64 nsecs;

    int cpu;

    WARN_ON_ONCE(in_softirq());
    WARN_ON_ONCE(in_interrupt());

    cpu = get_cpu();
    pr_info("kmldrv: [CPU#%d] start doing %s\n", cpu, __func__);
    tv_start = ktime_get();
    mutex_lock(&producer_lock);
    int move;
    WRITE_ONCE(move, negamax_predict(table, 'X').move);

    smp_mb();

    if (move != -1)
        WRITE_ONCE(table[move], 'X');

    WRITE_ONCE(turn, 'O');
    WRITE_ONCE(finish, 1);
    smp_wmb();
    mutex_unlock(&producer_lock);
    tv_end = ktime_get();

    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));
    pr_info("kmldrv: [CPU#%d] end doing %s for %llu usec\n", cpu, __func__,
            (unsigned long long) nsecs >> 10);
    put_cpu();
}

/* Workqueue for asynchronous bottom-half processing */
static struct workqueue_struct *kmldrv_workqueue;

/* Work item: holds a pointer to the function that is going to be executed
 * asynchronously.
 */
static DECLARE_WORK(work, simrupt_work_func);
static DECLARE_WORK(ai_one_work, ai_one_work_func);
static DECLARE_WORK(ai_two_work, ai_two_work_func);

/* Tasklet handler.
 *
 * NOTE: different tasklets can run concurrently on different processors, but
 * two of the same type of tasklet cannot run simultaneously. Moreover, a
 * tasklet always runs on the same CPU that schedules it.
 */
static void simrupt_tasklet_func(unsigned long __data)
{
    ktime_t tv_start, tv_end;
    s64 nsecs;

    WARN_ON_ONCE(!in_interrupt());
    WARN_ON_ONCE(!in_softirq());

    tv_start = ktime_get();

    READ_ONCE(finish);
    READ_ONCE(turn);
    smp_rmb();

    if (finish && turn == 'O') {
        WRITE_ONCE(finish, 0);
        smp_wmb();
        queue_work(kmldrv_workqueue, &ai_one_work);
    } else if (finish && turn == 'X') {
        WRITE_ONCE(finish, 0);
        smp_wmb();
        queue_work(kmldrv_workqueue, &ai_two_work);
    }
    queue_work(kmldrv_workqueue, &work);
    tv_end = ktime_get();

    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));

    pr_info("kmldrv: [CPU#%d] %s in_softirq: %llu usec\n", smp_processor_id(),
            __func__, (unsigned long long) nsecs >> 10);
}

/* Tasklet for asynchronous bottom-half processing in softirq context */
static DECLARE_TASKLET_OLD(simrupt_tasklet, simrupt_tasklet_func);

static void ai_game(void)
{
    WARN_ON_ONCE(!irqs_disabled());

    pr_info("kmldrv: [CPU#%d] doing AI game\n", smp_processor_id());
    pr_info("kmldrv: [CPU#%d] scheduling tasklet\n", smp_processor_id());
    tasklet_schedule(&simrupt_tasklet);
}

static void timer_handler(struct timer_list *__timer)
{
    ktime_t tv_start, tv_end;
    s64 nsecs;

    pr_info("kmldrv: [CPU#%d] enter %s\n", smp_processor_id(), __func__);
    /* We are using a kernel timer to simulate a hard-irq, so we must expect
     * to be in softirq context here.
     */
    WARN_ON_ONCE(!in_softirq());

    /* Disable interrupts for this CPU to simulate real interrupt context */
    local_irq_disable();

    tv_start = ktime_get();

    char win = check_win(table);

    if (win == ' ') {
        ai_game();
        mod_timer(&timer, jiffies + msecs_to_jiffies(delay));
    } else {
        int cpu = get_cpu();
        pr_info("kmldrv: [CPU#%d] Drawing final board\n", cpu);
        put_cpu();

        mutex_lock(&producer_lock);
        draw_board(table);
        mutex_unlock(&producer_lock);

        /* Store data to the kfifo buffer */
        mutex_lock(&consumer_lock);
        produce_board();
        mutex_unlock(&consumer_lock);

        wake_up_interruptible(&rx_wait);

        pr_info("kmldrv: %c win!!!\n", win);
    }
    tv_end = ktime_get();

    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));

    pr_info("kmldrv: [CPU#%d] %s in_irq: %llu usec\n", smp_processor_id(),
            __func__, (unsigned long long) nsecs >> 10);

    local_irq_enable();
}

static ssize_t kmldrv_read(struct file *file,
                           char __user *buf,
                           size_t count,
                           loff_t *ppos)
{
    unsigned int read;
    int ret;

    pr_debug("kmldrv: %s(%p, %zd, %lld)\n", __func__, buf, count, *ppos);

    if (unlikely(!access_ok(buf, count)))
        return -EFAULT;

    if (mutex_lock_interruptible(&read_lock))
        return -ERESTARTSYS;

    do {
        ret = kfifo_to_user(&rx_fifo, buf, count, &read);
        if (unlikely(ret < 0))
            break;
        if (read)
            break;
        if (file->f_flags & O_NONBLOCK) {
            ret = -EAGAIN;
            break;
        }
        ret = wait_event_interruptible(rx_wait, kfifo_len(&rx_fifo));
    } while (ret == 0);
    pr_debug("kmldrv: %s: out %u/%u bytes\n", __func__, read,
             kfifo_len(&rx_fifo));

    mutex_unlock(&read_lock);

    return ret ? ret : read;
}

static atomic_t open_cnt;

static int kmldrv_open(struct inode *inode, struct file *filp)
{
    pr_debug("kmldrv: %s\n", __func__);
    if (atomic_inc_return(&open_cnt) == 1)
        mod_timer(&timer, jiffies + msecs_to_jiffies(delay));
    pr_info("openm current cnt: %d\n", atomic_read(&open_cnt));

    return 0;
}

static int kmldrv_release(struct inode *inode, struct file *filp)
{
    pr_debug("kmldrv: %s\n", __func__);
    if (atomic_dec_and_test(&open_cnt) == 0) {
        del_timer_sync(&timer);
        flush_workqueue(kmldrv_workqueue);
        fast_buf_clear();
    }
    pr_info("release, current cnt: %d\n", atomic_read(&open_cnt));

    return 0;
}

static const struct file_operations kmldrv_fops = {
    .read = kmldrv_read,
    .llseek = no_llseek,
    .open = kmldrv_open,
    .release = kmldrv_release,
    .owner = THIS_MODULE,
};

static int __init kmldrv_init(void)
{
    dev_t dev_id;
    int ret;

    if (kfifo_alloc(&rx_fifo, PAGE_SIZE, GFP_KERNEL) < 0)
        return -ENOMEM;

    /* Register major/minor numbers */
    ret = alloc_chrdev_region(&dev_id, 0, NR_KMLDRV, DEV_NAME);
    if (ret)
        goto error_alloc;
    major = MAJOR(dev_id);

    /* Add the character device to the system */
    cdev_init(&kmldrv_cdev, &kmldrv_fops);
    ret = cdev_add(&kmldrv_cdev, dev_id, NR_KMLDRV);
    if (ret) {
        kobject_put(&kmldrv_cdev.kobj);
        goto error_region;
    }

    /* Create a class structure */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
    kmldrv_class = class_create(THIS_MODULE, DEV_NAME);
#else
    kmldrv_class = class_create(DEV_NAME);
#endif
    if (IS_ERR(kmldrv_class)) {
        printk(KERN_ERR "error creating kmldrv class\n");
        ret = PTR_ERR(kmldrv_class);
        goto error_cdev;
    }

    /* Register the device with sysfs */
    device_create(kmldrv_class, NULL, MKDEV(major, 0), NULL, DEV_NAME);

    /* Allocate fast circular buffer */
    fast_buf.buf = vmalloc(PAGE_SIZE);
    if (!fast_buf.buf) {
        device_destroy(kmldrv_class, dev_id);
        class_destroy(kmldrv_class);
        ret = -ENOMEM;
        goto error_cdev;
    }

    /* Create the workqueue */
    kmldrv_workqueue = alloc_workqueue("kmldrvd", WQ_UNBOUND, WQ_MAX_ACTIVE);
    if (!kmldrv_workqueue) {
        vfree(fast_buf.buf);
        device_destroy(kmldrv_class, dev_id);
        class_destroy(kmldrv_class);
        ret = -ENOMEM;
        goto error_cdev;
    }

    negamax_init();
    memset(table, ' ', N_GRIDS);
    turn = 'O';
    finish = 1;
    /* Setup the timer */
    timer_setup(&timer, timer_handler, 0);
    atomic_set(&open_cnt, 0);

    pr_info("kmldrv: registered new kmldrv device: %d,%d\n", major, 0);
out:
    return ret;
error_cdev:
    cdev_del(&kmldrv_cdev);
error_region:
    unregister_chrdev_region(dev_id, NR_KMLDRV);
error_alloc:
    kfifo_free(&rx_fifo);
    goto out;
}

static void __exit kmldrv_exit(void)
{
    dev_t dev_id = MKDEV(major, 0);

    del_timer_sync(&timer);
    tasklet_kill(&simrupt_tasklet);
    flush_workqueue(kmldrv_workqueue);
    destroy_workqueue(kmldrv_workqueue);
    vfree(fast_buf.buf);
    device_destroy(kmldrv_class, dev_id);
    class_destroy(kmldrv_class);
    cdev_del(&kmldrv_cdev);
    unregister_chrdev_region(dev_id, NR_KMLDRV);

    kfifo_free(&rx_fifo);
    pr_info("kmldrv: unloaded\n");
}

module_init(kmldrv_init);
module_exit(kmldrv_exit);
