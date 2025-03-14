/* @file    UnixDOOM.c
 * @author  vmx
 * @brief   Core DOOM - kernel bridge
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/fs.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/platform_device.h>

#define DOOM_IMPLEMENTATION
#include "PureDOOM.h"

#define DOOM_PRINT(fmt, ...) \
   printk(KERN_INFO "[DOOM]: " fmt, ##__VA_ARGS__)

#define UNREFERENCED_PARAMETER(v) (void)v

struct fb_info_kernel {
   void *fb_virt;
   unsigned int width;
   unsigned int height;
   unsigned int bpp;
   unsigned int line_length;
};

static struct fb_info_kernel *doom_fb_info_kernel = NULL;

/* Game loop control */

static int DoomStop = 0;

/* PureDOOM function overrides */

void DoomPrint(const char *str) {
   printk(KERN_INFO "[DOOM]: %s", str);
}

void *DoomMalloc(int size) {
   void *ret = kvmalloc(size, GFP_KERNEL);
   if (ret == NULL) {
      DOOM_PRINT("DoomMalloc: NULL POINTER [SIZE: %d]\n", size);
   }
   return ret;
   //return kmalloc(size, GFP_KERNEL);
}

void DoomFree(void *ptr) {
   return;
   //kfree(ptr);
}

void *DoomOpen(const char *filename, const char *mode) {
   UNREFERENCED_PARAMETER(mode);

   struct file *filp;
   filp = filp_open(filename, O_RDWR, 0644);
   if (IS_ERR(filp)) {
      DOOM_PRINT("DoomOpen: Failed to open %s\n, error %ld\n", filename,
                 PTR_ERR(filp));
      return NULL;
   }
   DOOM_PRINT("DoomOpen: Opened %s for reading and writing\n", filename);
   return filp;
}

void DoomClose(void *handle) {
   struct file *filp = (struct file*)handle;
   if (!filp) {
      DOOM_PRINT("DoomClose: Attempting to close NULL file handle\n");
      return;
   }
   filp_close(filp, NULL);
   DOOM_PRINT("DoomClose: Closed file\n");
}

int DoomRead(void *handle, void *buf, int count) {
   struct file *filp = (struct file*)handle;
   ssize_t bytes_read;
   loff_t pos;

   if (!filp || !buf) {
      return 0;
   }

   pos = filp->f_pos;
   bytes_read = kernel_read(filp, buf, count, &pos);
   filp->f_pos = pos;

   return (bytes_read > 0) ? bytes_read : 0;
}

int DoomWrite(void *handle, const void *buf, int count) {
   struct file *filp = (struct file*)handle;
   ssize_t bytes_written;
   loff_t pos;

   if (!filp || !buf || count <= 0) {
      return 0;
   }

   pos = filp->f_pos;
   bytes_written = kernel_write(filp, buf, count, &pos);
   filp->f_pos = pos;

   return (bytes_written > 0) ? bytes_written : 0;
}

int DoomSeek(void *handle, int offset, doom_seek_t origin) {
   struct file *filp = (struct file*)handle;
   loff_t new_pos = 0;

   if (!filp) {
      return -1;
   }

   switch (origin) {
   case DOOM_SEEK_SET:
      new_pos = offset;
      break;
   case DOOM_SEEK_CUR:
      new_pos = filp->f_pos + offset;
      break;
   case DOOM_SEEK_END:
      struct kstat stat;
      int err;

      err = vfs_getattr(&filp->f_path, &stat, STATX_SIZE, 
                        AT_STATX_SYNC_AS_STAT);
      if (err) {
         return -1;
      }

      new_pos = stat.size + offset;
      break;
   default:
      return -1;
   }

   if (new_pos < 0) {
      new_pos = 0;
   }
   filp->f_pos = new_pos;
   return 0;
}

int DoomTell(void *handle) {
   struct file *filp = (struct file*)handle;

   if (!filp) {
      return -1;
   }

   return filp->f_pos;
}

int DoomEof(void *handle) {
   struct file *filp = (struct file*)handle;
   struct kstat stat;
   int err;

   if (!filp) {
      return 1;
   }

   err = vfs_getattr(&filp->f_path, &stat, STATX_SIZE,
                     AT_STATX_SYNC_AS_STAT);
   if (err) {
      return 1;
   }

   return (filp->f_pos >= stat.size) ? 1 : 0;
}

void DoomGetTime(int *sec, int *usec) {
   struct timespec64 ts;
   ktime_get_real_ts64(&ts);

   if (sec) {
      *sec = ts.tv_sec;
   }
   if (usec) {
      *usec = ts.tv_nsec / 1000;
   }
}

void DoomExit(int code) {
   DOOM_PRINT("DoomExit: EXITING\n");
   DoomStop = 1;
}

#define DRIVE_ROOT "/"

char *DoomGetEnv(const char *var) {
   if (!strcmp(var, "HOME")) {
      return DRIVE_ROOT;
   }
   return NULL;
}

/* Kernel framebuffer */

static void *fb_virt;

static void doom_blt_to_framebuffer(struct fb_info_kernel *fb_info,
                                    unsigned char *doom_fb,
                                    int doom_width, int doom_height) {
   unsigned int x, y;
   unsigned int *screen_pixels = (unsigned int*)fb_info->fb_virt;
   unsigned int *doom_pixels = (unsigned int*)doom_fb;

   unsigned int start_x = (fb_info->width - doom_width) / 2;
   unsigned int start_y = (fb_info->height - doom_height) / 2;

   for (y = 0; y < doom_height; y++) {
      for (x = 0; x < doom_width; x++) {
         unsigned int screen_pos = 
            (start_y + y) * (fb_info->line_length / 4) + (start_x + x);
         unsigned int doom_pos = y * doom_width + x;

         screen_pixels[screen_pos] = doom_pixels[doom_pos];
      }
   }
}

/* DOOM kthread */

static struct task_struct *doom_thread;

static int doom_thread_func(void *data) {
   while (!kthread_should_stop()) {
      doom_update();
      
      unsigned char *fb = doom_get_framebuffer(4);
      doom_blt_to_framebuffer(doom_fb_info_kernel, fb, 320, 200);

      msleep(33);
   }
   DOOM_PRINT("Thread exiting\n");
   return 0;
}

/* Input handling */

static struct input_handler doom_input_handler;

static bool doom_input_match(struct input_handler *handler,
                             struct input_dev *dev) {
   bool is_keyboard = test_bit(EV_KEY, dev->evbit);

   bool is_rel_mouse = test_bit(EV_REL, dev->evbit) &&
                       test_bit(REL_X, dev->relbit) &&
                       test_bit(REL_Y, dev->relbit);

   bool is_abs_mouse = test_bit(EV_ABS, dev->evbit) &&
                       test_bit(ABS_X, dev->absbit) &&
                       test_bit(ABS_Y, dev->absbit);

   return is_keyboard || is_rel_mouse || is_abs_mouse;
}

static int doom_input_connect(struct input_handler *handler,
                              struct input_dev *dev,
                              const struct input_device_id *id) {
   struct input_handle *handle;
   int err;

   handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
   if (!handle) {
      return -ENOMEM;
   }

   handle->dev = dev;
   handle->handler = handler;
   handle->name = "doom_input";

   err = input_register_handle(handle);
   if (err) {
      goto err_free_handle;
   }
   err = input_open_device(handle);
   if (err) {
      goto err_unregister_handle;
   }

   DOOM_PRINT("Input: Connected to %s\n", dev->name);
   return 0;

err_unregister_handle:
   input_unregister_handle(handle);
err_free_handle:
   kfree(handle);
   return err;
}

static void doom_input_disconnect(struct input_handle *handle) {
   DOOM_PRINT("Input: Disconnected from: %s\n", handle->dev->name);
   input_close_device(handle);
   input_unregister_handle(handle);
   kfree(handle);
}

static void doom_input_event(struct input_handle *handle, unsigned int type,
                             unsigned int code, int value) {
   if (type == EV_KEY) {
      DOOM_PRINT("Input: Key event code=%u, value=%d\n", code, value);
   } else if (type == EV_REL) {
      DOOM_PRINT("Input: Mouse movement code=%u, value=%d\n", code, value);
   } else if (type == EV_ABS) {
      DOOM_PRINT("Input: Absolute event code=%u, value=%d\n", code, value);
   }
}

static const struct input_device_id doom_input_ids[] = {
   {
      .flags = INPUT_DEVICE_ID_MATCH_EVBIT,
      .evbit = { BIT_MASK(EV_KEY) },
   },
   {
      .flags = INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_RELBIT,
      .evbit = { BIT_MASK(EV_REL) },
      .relbit = { BIT_MASK(REL_X) | BIT_MASK(REL_Y) },
   },
   {
      .flags = INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_ABSBIT,
      .evbit = { BIT_MASK(EV_ABS) },
      .absbit = { BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
   },
   { },
};

static struct fb_info_kernel *init_fb(void *fb_virt, struct fb_info *info) {
   struct fb_info_kernel *fb_kernel = 
      kmalloc(sizeof(struct fb_info_kernel), GFP_KERNEL);
   if (!fb_kernel) {
      return NULL;
   }

   fb_kernel->fb_virt = fb_virt;
   fb_kernel->width = info->var.xres;
   fb_kernel->height = info->var.yres;
   fb_kernel->bpp = info->var.bits_per_pixel;
   fb_kernel->line_length = info->fix.line_length;

   return fb_kernel;
}

static int __init unix_doom_init(void) {
   DOOM_PRINT("Module loaded\n");
   int err;

   doom_input_handler.name = "doom_input";
   doom_input_handler.event = doom_input_event;
   doom_input_handler.connect = doom_input_connect;
   doom_input_handler.disconnect = doom_input_disconnect;
   doom_input_handler.match = doom_input_match;
   doom_input_handler.id_table = doom_input_ids;

   err = input_register_handler(&doom_input_handler);
   if (err) {
      DOOM_PRINT("Input: Failed to register handler\n");
      return err;
   }

   struct fb_info *fb = NULL;
   struct file *filp;

   filp = filp_open("/dev/fb0", O_RDWR, 0);
   if (IS_ERR(filp)) {
      DOOM_PRINT("Failed to open /dev/fb0\n");
      input_unregister_handler(&doom_input_handler);
      return PTR_ERR(filp);
   }

   fb = ((struct fb_info*)(filp->private_data));
   if (!fb) {
      DOOM_PRINT("Failed to get framebuffer info\n");
      filp_close(filp, NULL);
      input_unregister_handler(&doom_input_handler);
      return -ENOMEM;
   }

   DOOM_PRINT("Found framebuffer: %s\n", fb->fix.id);
   DOOM_PRINT("Resolution: %dx%d, %dbpp\n", fb->var.xres, fb->var.yres,
              fb->var.bits_per_pixel);

   fb_virt = ioremap(fb->fix.smem_start, fb->fix.smem_len);
   if (!fb_virt) {
      DOOM_PRINT("Failed to map framebuffer memory\n");
      filp_close(filp, NULL);
      input_unregister_handler(&doom_input_handler);
      return -ENOMEM;
   }
   DOOM_PRINT("unix_doom_init: fb_init = %p\n", fb_virt);

   doom_fb_info_kernel = init_fb(fb_virt, fb);
   if (!doom_fb_info_kernel) {
      DOOM_PRINT("Failed to initialize framebuffer\n");
      filp_close(filp, NULL);
      input_unregister_handler(&doom_input_handler);
      return -ENOMEM;
   }

   doom_set_file_io(DoomOpen, DoomClose, DoomRead, DoomWrite, DoomSeek,
                    DoomTell, DoomEof);
   doom_set_malloc(DoomMalloc, DoomFree);
   doom_set_exit(DoomExit);
   doom_set_getenv(DoomGetEnv);
   doom_set_gettime(DoomGetTime);
   doom_set_print(DoomPrint);

   char *argv[] = { "doom", "-file", "/DOOM/DOOM.WAD" };
   doom_init(3, argv, 0);

   doom_thread = kthread_create(doom_thread_func, NULL, "doom_kthread");
   if (IS_ERR(doom_thread)) {
      DOOM_PRINT("Failed to start DOOM thread\n");
      input_unregister_handler(&doom_input_handler);
      return PTR_ERR(doom_thread);
   }
   wake_up_process(doom_thread);

   filp_close(filp, NULL);
   return 0;
}

static void __exit unix_doom_exit(void) {
   DOOM_PRINT("Module unloaded\n");

   if (doom_thread) {
      /* stop DOOM thread before shutting down input handler */
      kthread_stop(doom_thread);
   }
   input_unregister_handler(&doom_input_handler);
   if (fb_virt) {
      iounmap(fb_virt);
   }
   if (doom_fb_info_kernel) {
      kfree(doom_fb_info_kernel);
   }
}

module_init(unix_doom_init);
module_exit(unix_doom_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("vmx");
MODULE_DESCRIPTION("DOOM in the Linux kernel");
