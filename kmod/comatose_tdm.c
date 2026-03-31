/*
 * comatose_tdm.ko - replacement TDM COMA service with sane error handling
 *
 * deregisters the stock kernel TDM service (which BUG_ON's on nack)
 * and registers our own with a proper response handler.
 *
 * usage:
 *   insmod comatose_tdm.ko
 *   echo "0 8000 16 16" > /proc/comatose/tdm_brute
 *   dmesg | grep comatose_tdm
 */

#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/coma/coma.h>

#define TDM_GRANT   0
#define TDM_REVOKE  1
#define TDM_ACK     2
#define TDM_NACK    3

#define SERVICE_NAME "tdm"
#define CFIFO_SIZE   1024

/*
 * the kernel source has: {type, id, cookie, channels, sample_size, rate}
 * but the kernel fills msg.rate=rate (field at offset 20) and msg.cookie=0
 * (field at offset 8). if the CSS actually expects {type, id, rate, channels,
 * sample_size, cookie}, then the kernel's "bug" puts everything in the right
 * place. let's try this layout:
 */
struct tdm_msg_grant {
	uint32_t type;
	uint32_t id;
	uint32_t rate;
	uint32_t channels;
	uint32_t sample_size;
	uint32_t cookie;
} __attribute__((__packed__));

struct tdm_msg_nack {
	uint32_t type;
	uint32_t cookie;
	int32_t  reason;
};

static int service_id = -1;
static int original_service_id = -1;
static struct proc_dir_entry *proc_dir;
static DECLARE_COMPLETION(tdm_reply);
static DEFINE_MUTEX(tdm_mutex);
static int last_response_type = -1;
static int last_nack_reason = 0;

/*
 * our TDM response handler — logs nacks instead of BUG()ing
 */
static void our_tdm_process_message(void *arg, struct cmsg *cmsg)
{
	uint32_t *payload = cmsg_payload(cmsg);
	uint32_t msg_type = payload[0];

	if (msg_type == TDM_ACK) {
		pr_info("comatose_tdm: received ACK!\n");
		last_response_type = TDM_ACK;
		last_nack_reason = 0;
	} else if (msg_type == TDM_NACK) {
		int32_t reason = (int32_t)payload[2];
		pr_info("comatose_tdm: received NACK, reason=%d\n", reason);
		last_response_type = TDM_NACK;
		last_nack_reason = reason;
	} else {
		pr_warn("comatose_tdm: unknown response type %u\n", msg_type);
		last_response_type = -1;
	}

	complete(&tdm_reply);
}

static void our_tdm_remove(void *arg)
{
	pr_info("comatose_tdm: service removed by CSS\n");
	service_id = -1;
}

static int send_grant_and_wait(unsigned int id, unsigned int rate,
                               unsigned int channels, unsigned int sample_size)
{
	struct cmsg *cmsg;
	struct tdm_msg_grant *msg;
	int ret;
	unsigned long timedout;

	if (service_id < 0)
		return -ENODEV;

	mutex_lock(&tdm_mutex);

	reinit_completion(&tdm_reply);
	last_response_type = -1;

	cmsg = coma_cmsg_alloc(service_id, service_id, 0, 5 * sizeof(uint32_t));
	if (IS_ERR(cmsg)) {
		mutex_unlock(&tdm_mutex);
		return PTR_ERR(cmsg);
	}

	/* try: 5-word message with no cookie field at all.
	 * maybe the CSS expects exactly {type, id, rate, channels, sample_size}
	 * and a 6th word confuses it. */
	{
		uint32_t *raw = cmsg_payload(cmsg);
		raw[0] = TDM_GRANT;
		raw[1] = id;
		raw[2] = rate;
		raw[3] = channels;
		raw[4] = sample_size;
		pr_info("comatose_tdm: wire(5w): %08x %08x %08x %08x %08x\n",
		        raw[0], raw[1], raw[2], raw[3], raw[4]);
	}

	ret = coma_cmsg_commit(service_id);
	if (ret < 0) {
		mutex_unlock(&tdm_mutex);
		return ret;
	}

	timedout = wait_for_completion_timeout(&tdm_reply, 2 * HZ);
	if (timedout == 0) {
		pr_err("comatose_tdm: grant timeout for ch=%u\n", channels);
		mutex_unlock(&tdm_mutex);
		return -ETIMEDOUT;
	}

	ret = (last_response_type == TDM_ACK) ? 0 : -last_nack_reason;
	mutex_unlock(&tdm_mutex);
	return ret;
}

static ssize_t grant_write(struct file *file, const char __user *buf,
                           size_t len, loff_t *offset)
{
	char kbuf[64];
	unsigned int id, rate, channels, sample_size;
	int ret;

	if (len >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, len))
		return -EFAULT;
	kbuf[len] = '\0';

	if (sscanf(kbuf, "%u %u %u %u", &id, &rate, &channels, &sample_size) != 4)
		return -EINVAL;

	ret = send_grant_and_wait(id, rate, channels, sample_size);
	pr_info("comatose_tdm: grant ch=%u result=%d\n", channels, ret);
	return len;
}

static ssize_t brute_write(struct file *file, const char __user *buf,
                           size_t len, loff_t *offset)
{
	char kbuf[64];
	unsigned int id, rate, max_ch, sample_size;
	unsigned int ch;

	if (len >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, len))
		return -EFAULT;
	kbuf[len] = '\0';

	if (sscanf(kbuf, "%u %u %u %u", &id, &rate, &max_ch, &sample_size) != 4)
		return -EINVAL;

	pr_info("comatose_tdm: brute force ch=1..%u\n", max_ch);

	for (ch = 1; ch <= max_ch; ch++) {
		int ret = send_grant_and_wait(id, rate, ch, sample_size);
		if (ret == 0) {
			pr_info("comatose_tdm: *** ch=%u ACCEPTED! ***\n", ch);
			return len;
		}
		pr_info("comatose_tdm: ch=%u rejected (ret=%d)\n", ch, ret);
	}

	pr_info("comatose_tdm: no channel count accepted\n");
	return len;
}

static const struct file_operations grant_fops = {
	.owner = THIS_MODULE,
	.write = grant_write,
};

static const struct file_operations brute_fops = {
	.owner = THIS_MODULE,
	.write = brute_write,
};

static int __init comatose_tdm_init(void)
{
	/*
	 * deregister the stock kernel TDM service and register our own.
	 * the stock handler has BUG_ON(1) on nack which is unusable.
	 *
	 * the stock TDM service was registered as service ID 1 during
	 * COMA init. we deregister it and re-register with the same name.
	 */
	original_service_id = 1; /* TDM is always service 1 */
	coma_deregister(original_service_id);
	pr_info("comatose_tdm: deregistered stock TDM service (id=%d)\n",
	        original_service_id);

	/* small delay for the deregistration to complete */
	msleep(100);

	service_id = coma_register(SERVICE_NAME, CFIFO_SIZE, NULL,
	                           our_tdm_process_message, our_tdm_remove,
	                           NULL);
	if (service_id < 0) {
		pr_err("comatose_tdm: failed to register: %d\n", service_id);
		return service_id;
	}
	pr_info("comatose_tdm: registered as service id %d\n", service_id);

	proc_dir = proc_mkdir("comatose", NULL);
	if (!proc_dir)
		return -ENOMEM;

	proc_create("tdm_grant", 0220, proc_dir, &grant_fops);
	proc_create("tdm_brute", 0220, proc_dir, &brute_fops);

	pr_info("comatose_tdm: loaded (replacement TDM service)\n");
	return 0;
}

static void __exit comatose_tdm_exit(void)
{
	remove_proc_entry("tdm_brute", proc_dir);
	remove_proc_entry("tdm_grant", proc_dir);
	remove_proc_entry("comatose", NULL);

	if (service_id >= 0)
		coma_deregister(service_id);

	pr_info("comatose_tdm: unloaded\n");
}

module_init(comatose_tdm_init);
module_exit(comatose_tdm_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Replacement TDM COMA service with proper nack handling");
