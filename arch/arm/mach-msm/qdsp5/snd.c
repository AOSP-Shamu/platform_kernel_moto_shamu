/* arch/arm/mach-msm/qdsp5/snd.c
 *
 * interface to "snd" service on the baseband cpu
 *
 * Copyright (C) 2008 HTC Corporation
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/msm_audio.h>

#include <asm/atomic.h>
#include <asm/ioctls.h>
#include <asm/arch/board.h>
#include <asm/arch/msm_rpcrouter.h>

struct snd_ctxt {
	struct mutex lock;
	int opened;
	struct msm_rpc_endpoint *ept;
	struct msm_snd_endpoints *snd_epts;
};

static struct snd_ctxt the_snd;

#define RPC_SND_PROG	0x30000002
#define RPC_SND_CB_PROG	0x31000002
#if CONFIG_MSM_AMSS_VERSION == 6210
#define RPC_SND_VERS                    0x94756085 /* 2490720389 */
#elif (CONFIG_MSM_AMSS_VERSION == 6220) || (CONFIG_MSM_AMSS_VERSION == 6225)
#define RPC_SND_VERS                    0xaa2b1a44 /* 2854951492 */
#endif

#define SND_SET_DEVICE_PROC 2
#define SND_SET_VOLUME_PROC 3

struct rpc_snd_set_device_args {
	uint32_t device;
	uint32_t ear_mute;
	uint32_t mic_mute;

	uint32_t cb_func;
	uint32_t client_data;
};

struct rpc_snd_set_volume_args {
	uint32_t device;
	uint32_t method;
	uint32_t volume;

	uint32_t cb_func;
	uint32_t client_data;
};

struct snd_set_device_msg {
	struct rpc_request_hdr hdr;
	struct rpc_snd_set_device_args args;
};

struct snd_set_volume_msg {
	struct rpc_request_hdr hdr;
	struct rpc_snd_set_volume_args args;
};

extern struct snd_endpoint * get_snd_endpoints(int *size);

static inline int check_device(struct snd_ctxt *snd, int device)
{
	int cnt;
	for(cnt = 0; cnt < snd->snd_epts->num; cnt++)
		if (device == snd->snd_epts->endpoints[cnt].id)
			return 0;
	return -EINVAL;	
}

static inline int check_mute(int mute)
{
	return (mute == SND_MUTE_MUTED ||
		mute == SND_MUTE_UNMUTED) ? 0 : -EINVAL;
}

static long snd_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct snd_set_device_msg dmsg;
	struct snd_set_volume_msg vmsg;
	struct snd_device_config dev;
	struct snd_volume_config vol;
	struct snd_ctxt *snd = file->private_data;
	int rc = 0;

	mutex_lock(&snd->lock);
	switch (cmd) {
	case SND_SET_DEVICE:
		if (copy_from_user(&dev, (void __user*) arg, sizeof(dev))) {
			pr_err("snd_ioctl set device: invalid user pointer.\n");
			rc = -EFAULT;
			break;
		}


		rc = check_device(snd, dev.device); 
		if (rc < 0) {
			pr_err("snd_ioctl set device: invalid device.\n");
			rc = -EINVAL;
			break;
		}

		dmsg.args.device = cpu_to_be32(dev.device);
		dmsg.args.ear_mute = cpu_to_be32(dev.ear_mute);		  
		dmsg.args.mic_mute = cpu_to_be32(dev.mic_mute);
		if (check_mute(dev.ear_mute) < 0 ||
	            check_mute(dev.mic_mute) < 0) {
			pr_err("snd_ioctl set device: invalid mute status.\n");
			rc = -EINVAL;
			break;
		}
		dmsg.args.cb_func = -1; 
		dmsg.args.client_data = 0;

		pr_info("snd_set_device %d %d %d\n", dev.device,
						 dev.ear_mute, dev.mic_mute);

		rc = msm_rpc_call(snd->ept,
			SND_SET_DEVICE_PROC,
			&dmsg, sizeof(dmsg), 5 * HZ);
		break;

	case SND_SET_VOLUME:
		if (copy_from_user(&vol, (void __user*) arg, sizeof(vol))) {
			pr_err("snd_ioctl set volume: invalid user pointer.\n");
			rc = -EFAULT;
			break;
		}

		rc = check_device(snd, vol.device);
		if (rc < 0) {
			pr_err("snd_ioctl set volume: invalid device.\n");
			rc = -EINVAL;
			break;
		}

		vmsg.args.device = cpu_to_be32(vol.device);
		vmsg.args.method = cpu_to_be32(vol.method);
		if (vol.method != SND_METHOD_VOICE) {
			pr_err("snd_ioctl set volume: invalid method.\n");
			rc = -EINVAL;
			break;
		}

		vmsg.args.volume = cpu_to_be32(vol.volume);
		vmsg.args.cb_func = -1; 
		vmsg.args.client_data = 0; 

		pr_info("snd_set_volume %d %d %d\n", vol.device,
						vol.method, vol.volume);

		rc = msm_rpc_call(snd->ept,
			SND_SET_VOLUME_PROC,
			&vmsg, sizeof(vmsg), 5 * HZ);
		break;

	case SND_GET_ENDPOINTS:
		rc = snd->snd_epts->num * sizeof(struct snd_endpoint);
		if (arg && 
		    copy_to_user((void __user*)arg, snd->snd_epts->endpoints, rc)) {
			pr_err("snd_ioctl get endpoints: invalid user pointer.\n");
			rc = -EFAULT;
		}
		break;

	default:
		pr_err("snd_ioctl unknown command.\n");
		rc = -EINVAL;
		break;
	}
	mutex_unlock(&snd->lock);

	return rc;
}

static int snd_release(struct inode *inode, struct file *file)
{
	struct snd_ctxt *snd = file->private_data;

	mutex_lock(&snd->lock);
	snd->opened = 0;
	mutex_unlock(&snd->lock);
	return 0;
}

static int snd_open(struct inode *inode, struct file *file)
{
	struct snd_ctxt *snd = &the_snd;
	int rc = 0;

	mutex_lock(&snd->lock);
	if (snd->opened == 0) {
		if (snd->ept == NULL) {
			snd->ept = msm_rpc_connect(RPC_SND_PROG, RPC_SND_VERS,
						MSM_RPC_UNINTERRUPTIBLE);
			if (IS_ERR(snd->ept)) {
				rc = PTR_ERR(snd->ept);
				snd->ept = NULL;
				pr_err("snd: failed to connect snd svc\n");
				goto err;
			}
		}
		file->private_data = snd;
		snd->opened = 1;
	}
	else {
		pr_err("snd already opened.\n");
		rc = -EBUSY;
	}

err:
	mutex_unlock(&snd->lock);
	return rc;
}

static struct file_operations snd_fops = {
	.owner		= THIS_MODULE,
	.open		= snd_open,
	.release	= snd_release,
	.unlocked_ioctl	= snd_ioctl,
};

struct miscdevice snd_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "msm_snd",
	.fops	= &snd_fops,
};

static int snd_probe(struct platform_device *pdev)
{
	struct snd_ctxt *snd = &the_snd;
	mutex_init(&snd->lock);
	snd->snd_epts = (struct msm_snd_endpoints *)pdev->dev.platform_data;
	return misc_register(&snd_misc);
}

static struct platform_driver snd_plat_driver = {
	.probe = snd_probe,
	.driver = {
		.name = "msm_snd",
		.owner = THIS_MODULE,
	},
};

static int __init snd_init(void)
{
	return platform_driver_register(&snd_plat_driver);
}

module_init(snd_init);
