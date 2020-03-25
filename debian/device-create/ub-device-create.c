/* device-create:
 *
 * Detect the available GPUs, and create the recommended device files.
 *
 * Authored by:
 *   Alberto Milone
 *
 * Copyright (C) 2020 Canonical Ltd
 *
 * Based on code from the following sources:
 *
 * 1) ./hw/xfree86/common/xf86pciBus.c in xorg-server:
 *    Copyright (c) 1997-2003 by The XFree86 Project, Inc.
 *
 * 2) gpu-manager.c from the ubuntu-drivers-common package:
 *    Copyright (C) 2014 Canonical Ltd
 *
 * 3) the nvidia-modprobe tool:
 *    Copyright (c) 2013, NVIDIA CORPORATION.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Except as contained in this notice, the name of the copyright holder(s)
 * and author(s) shall not be used in advertising or otherwise to promote
 * the sale, use or other dealings in this Software without prior written
 * authorization from the copyright holder(s) and author(s).
 *
 *
 * Build with `gcc -o device-create device-create.c $(pkg-config --cflags --libs pciaccess)`
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <ctype.h>
#include <pciaccess.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <getopt.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/limits.h>
#include <sys/utsname.h>
#include <libkmod.h>

static inline void freep(void *);
static inline void fclosep(FILE **);
static inline void pclosep(FILE **);

#define _cleanup_free_ __attribute__((cleanup(freep)))
#define _cleanup_fclose_ __attribute__((cleanup(fclosep)))
#define _cleanup_pclose_ __attribute__((cleanup(pclosep)))

#define PCI_CLASS_DISPLAY               0x03
#define PCI_CLASS_DISPLAY_OTHER         0x0380

#define PCIINFOCLASSES(c) \
    ( (((c) & 0x00ff0000) \
     == (PCI_CLASS_DISPLAY << 16)) )

#define AMD 0x1002
#define INTEL 0x8086
#define NVIDIA 0x10de

#define MAX_CARDS_N 200

/*
    Docs:
    https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/Documentation/admin-guide/devices.txt

    195 char   Nvidia graphics devices
      0 = /dev/nvidia0      First Nvidia card
      1 = /dev/nvidia1      Second Nvidia card
        ...
    255 = /dev/nvidiactl    Nvidia card control device

    sudo mknod /dev/nvidiactl c 195 255 -m 0666
    sudo mknod /dev/nvidia0 c 195 0 -m 0666
*/


/* From nvidia-modprobe-utils.c */

#define NV_DEVICE_FILE_MODE_MASK (S_IRWXU|S_IRWXG|S_IRWXO)
#define NV_DEVICE_FILE_MODE (S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH)
#define NV_DEVICE_FILE_UID 0
#define NV_DEVICE_FILE_GID 0

#define NV_MAKE_DEVICE(x,y) ((dev_t)((x) << 8 | (y)))

#define NV_MAJOR_DEVICE_NUMBER 195

#define NV_MAX_CHARACTER_DEVICE_FILE_STRLEN  128
#define NV_MODULE_INSTANCE_NONE              -1
#define NV_MODULE_INSTANCE_ZERO              0
#define NV_MAX_MODULE_INSTANCES              8
#define NV_CTL_DEVICE_NUM                    255
#define NV_MODESET_MINOR_DEVICE_NUM          254
#define NV_NVSWITCH_CTL_MINOR                255

#define NV_FRONTEND_CONTROL_DEVICE_MINOR_MAX NV_CTL_DEVICE_NUM

#define NV_DEVICE_FILE_PATH "/dev/nvidia%d"
#define NV_CTRL_DEVICE_FILE_PATH "/dev/nvidiactl"
#define NV_MODESET_DEVICE_NAME "/dev/nvidia-modeset"
#define NV_VGPU_VFIO_DEVICE_NAME "/dev/nvidia-vgpu%d"
#define NV_NVLINK_DEVICE_NAME "/dev/nvidia-nvlink"
#define NV_NVSWITCH_CTL_NAME "/dev/nvidia-nvswitchctl"
#define NV_NVSWITCH_DEVICE_NAME "/dev/nvidia-nvswitch%d"

#define NV_NMODULE_CTRL_DEVICE_FILE_PATH "/dev/nvidiactl%d"

#define NV_FRONTEND_CONTROL_DEVICE_MINOR_MIN \
    (NV_FRONTEND_CONTROL_DEVICE_MINOR_MAX - \
     NV_MAX_MODULE_INSTANCES)

#define NV_PROC_DEVICES_PATH "/proc/devices"
#define NV_MAX_LINE_LENGTH               256
#define NV_UVM_MODULE_NAME "nvidia-uvm"

/* end from nvidia-modprobe-utils.c */

static char *log_file = NULL;
static FILE *log_handle = NULL;
static FILE *devnull = NULL;
static int dry_run = 0;
static int verbose = 0;
static int log_level = 1;
static char *fake_modules_path = NULL;
static char *nvidia_driver_version_path = NULL;

static struct pci_slot_match match = {
    PCI_MATCH_ANY, PCI_MATCH_ANY, PCI_MATCH_ANY, PCI_MATCH_ANY, 0
};

struct device {
    int boot_vga;
    unsigned int vendor_id;
    unsigned int device_id;
    /* BusID components */
    unsigned int domain;
    unsigned int bus;
    unsigned int dev;
    unsigned int func;
    int has_connected_outputs;
};


static bool is_link(char *file);
static bool is_module_loaded(const char *module);

static inline void freep(void *p) {
    free(*(void**) p);
}


static inline void fclosep(FILE **file) {
    if (*file != NULL && *file >= 0)
        fclose(*file);
}


static inline void pclosep(FILE **file) {
    if (*file != NULL)
        pclose(*file);
}


static bool is_module_loaded(const char *module) {
    bool status = false;
    char line[4096];
    _cleanup_fclose_ FILE *file = NULL;

    if (!fake_modules_path)
        file = fopen("/proc/modules", "r");
    else
        file = fopen(fake_modules_path, "r");

    if (!file) {
        fprintf(log_handle, "Error: can't open /proc/modules");
        return false;
    }

    while (fgets(line, sizeof(line), file)) {
        char *tok;
        tok = strtok(line, " \t");
        if (strstr(tok, module) != NULL) {
            status = true;
            break;
        }
    }

    return status;
}


static bool is_link(char *file) {
    struct stat stbuf;

    if (lstat(file, &stbuf) == -1) {
        fprintf(log_handle, "Error: can't access %s\n", file);
        return false;
    }
    if ((stbuf.st_mode & S_IFMT) == S_IFLNK)
        return true;

    return false;
}

/* See if the device is bound to a driver */
static bool is_device_bound_to_driver(struct pci_device *info) {
    char sysfs_path[1024];
    snprintf(sysfs_path, sizeof(sysfs_path),
             "/sys/bus/pci/devices/%04x:%02x:%02x.%d/driver",
             info->domain, info->bus, info->dev, info->func);

    return(is_link(sysfs_path));
}


/* See if the device is a pci passthrough */
static bool is_device_pci_passthrough(struct pci_device *info) {
    enum { BUFFER_SIZE = 1024 };
    char buf[BUFFER_SIZE], sysfs_path[BUFFER_SIZE], *drv, *name;
    ssize_t length;

    length = snprintf(sysfs_path, sizeof(sysfs_path),
                      "/sys/bus/pci/devices/%04x:%02x:%02x.%d/driver",
                      info->domain, info->bus, info->dev, info->func);
    if (length < 0 || length >= sizeof(sysfs_path))
        return false;

    length = readlink(sysfs_path, buf, sizeof(buf)-1);

    if (length != -1) {
        buf[length] = '\0';

        if ((drv = strrchr(buf, '/')))
            name = drv+1;
        else
            name = buf;

        if (strcmp(name, "pci-stub") == 0 || strcmp(name, "pciback") == 0)
            return true;
    }
    return false;
}


/*
 * Scan NV_PROC_DEVICES_PATH to find the major number of the character
 * device with the specified name.  Returns the major number on success,
 * or -1 on failure.
 */
static int get_chardev_major(const char *name)
{
    int ret = -1;
    char line[NV_MAX_LINE_LENGTH];
    _cleanup_fclose_ FILE *fp = NULL;

    line[NV_MAX_LINE_LENGTH - 1] = '\0';

    fp = fopen(NV_PROC_DEVICES_PATH, "r");
    if (!fp) {
        goto done;
    }

    /* Find the beginning of the 'Character devices:' section */

    while (fgets(line, NV_MAX_LINE_LENGTH - 1, fp)) {
        if (strcmp(line, "Character devices:\n") == 0) {
            break;
        }
    }

    if (ferror(fp)) {
        goto done;
    }

    /* Search for the given module name */

    while (fgets(line, NV_MAX_LINE_LENGTH - 1, fp)) {
        char *found;

        if (strcmp(line, "\n") == 0 ) {
            /* we've reached the end of the 'Character devices:' section */
            break;
        }

        found = strstr(line, name);

        /* Check for a newline to avoid partial matches */
        if (found && found[strlen(name)] == '\n') {
            int major;
            /* Read the device major number */
            if (sscanf(line, " %d %*s", &major) == 1) {
                ret = major;
            }
            break;
        }
    }

done:
    return ret;
}



static int make_char_device_node(char *path, int major, int minor) {
    struct stat stbuf;

    mode_t mode = (S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH);
    dev_t dev = NV_MAKE_DEVICE(major, minor);
    int created = 0;
    int delete_device = 0;

create:
    if (stat(path, &stbuf) != 0) {
        fprintf(log_handle, "Creating device %s\n", path);
        if (mknod(path, S_IFCHR | mode, dev) != 0) {
            fprintf(log_handle, "Failed to create device %s\n", path);
            return 1;
        }
        else
            created = 1;
    }

    if (S_ISCHR(stbuf.st_mode) && (stbuf.st_rdev == dev)) {
        fprintf(log_handle, "%s is a char device\n", path);
    }
    else {
        fprintf(log_handle, "%s is not a valid char device\n", path);
        delete_device = 1;
    }

    if (((stbuf.st_mode & NV_DEVICE_FILE_MODE_MASK) == mode) &&
        (stbuf.st_uid == 0) &&
        (stbuf.st_gid == 0))
    {
        fprintf(log_handle, "%s exists, and has the correct permissions\n", path);
        return 0;
    }
    else {
        fprintf(log_handle, "%s exists, but does not have the correct permissions\n", path);
        delete_device = 1;
    }

    if ((chmod(path, mode) != 0) ||
           (chown(path, 0, 0) != 0)) {
        fprintf(log_handle, "Failed fixing permissions for %s\n", path);
    }
    else {
        fprintf(log_handle, "Permissions for %s fixed\n", path);
        return 0;
    }

    if (delete_device) {
        fprintf(log_handle, "Removing device %s\n", path);
        unlink(path);
    }

    if (created == 0)
        goto create;

    return 1;
}

static int make_nvidia_device_node(int minor) {
    char path[100];

    /* /dev/nvidia0, etc. */
    snprintf(path, sizeof(path),
             "/dev/nvidia%d",
             minor);

    return make_char_device_node(path, NV_MAJOR_DEVICE_NUMBER, minor);
}


static int make_nvidiactl_device_node() {
    /* Create /dev/nvidiactl 195 255 */
    return make_char_device_node("/dev/nvidiactl", NV_MAJOR_DEVICE_NUMBER, 255);
}


static int make_nvidia_modeset_node() {
    /* Create /dev/nvidiactl 195 255 */
    return make_char_device_node(NV_MODESET_DEVICE_NAME, NV_MAJOR_DEVICE_NUMBER, NV_MODESET_MINOR_DEVICE_NUM);
}

static int make_nvidia_uvm_node() {
    char path[100];
    int major;

    /* /dev/nvidia-uvm, minor 0 */
    snprintf(path, sizeof(path),
             "/dev/nvidia-uvm");

    major = get_chardev_major(NV_UVM_MODULE_NAME);

    return make_char_device_node(path, major, 0);
}

static int make_nvidia_uvm_tools_node() {
    char path[100];
    int major;

    /* /dev/nvidia-uvm-tools, minor 1 */
    snprintf(path, sizeof(path),
             "/dev/nvidia-uvm-tools");

    major = get_chardev_major(NV_UVM_MODULE_NAME);

    return make_char_device_node(path, major, 1);
}


int main(int argc, char *argv[]) {

    int opt, i, ret;
    /* Variables for pciaccess */
    int pci_init = -1;
    struct pci_device_iterator *iter = NULL;
    struct pci_device *info = NULL;

    log_handle = stdout;



    /* Arguments */


    while (1) {
        static struct option long_options[] =
        {
        /* These options set a flag. */
        {"dry-run", no_argument,     &dry_run, 1},
        {"verbose", no_argument,     &verbose, 1},
        /* These options don't set a flag.
          We distinguish them by their indices. */
        {"log",  required_argument, 0, 'l'},
        {0, 0, 0, 0}
        };
        /* getopt_long stores the option index here. */
        int option_index = 0;

        opt = getopt_long (argc, argv, "a:b:c:d:f:g:h:i:j:k:l:m:n:o:p:q:r:s:t:x:y:z:w:",
                        long_options, &option_index);

        /* Detect the end of the options. */
        if (opt == -1)
         break;

        switch (opt) {
            case 0:
                if (long_options[option_index].flag != 0)
                    break;
                printf("option %s", long_options[option_index].name);
                if (optarg)
                    printf(" with arg %s", optarg);
                printf("\n");
                break;
            case 'l':
                /* printf("option -l with value '%s'\n", optarg); */
                log_file = malloc(strlen(optarg) + 1);
                if (log_file)
                    strcpy(log_file, optarg);
                else
                    abort();
                break;
            case '?':
                /* getopt_long already printed an error message. */
                exit(1);
                break;

            default:
                abort();
        }

    }
    /* End Argument parsing */

    if (log_file) {
        log_handle = fopen(log_file, "w");

        if (!log_handle) {
            /* Use stdout */
            log_handle = stdout;
            fprintf(stderr, "Warning: writing to %s failed (%s)\n",
                    log_file, strerror(errno));
        }
    }
    else {
        log_handle = stdout;
    }

    if (!verbose) {
        if ((devnull = fopen("/dev/null", "w")) == NULL) {
            fprintf(stderr, "Error: Can't open /dev/null: %s\n", strerror(errno));
            exit(1);
        }
        else {
            log_handle = devnull;
        }
    }

    nvidia_driver_version_path = strdup("/sys/module/nvidia/version");
    if (!nvidia_driver_version_path) {
        fprintf(stderr, "Couldn't allocate nvidia_driver_version_path\n");
        goto end;
    }

    if (!is_module_loaded("nvidia")) {
        fprintf(stderr, "No nvidia module is loaded. Aborting.\n");
        goto end;
    }

    /* Get the current system data */
    pci_init = pci_system_init();
    if (pci_init != 0)
        goto end;

    iter = pci_slot_match_iterator_create(&match);
    if (!iter)
        goto end;

    int nvidia_devices_n = 0;
    while ((info = pci_device_next(iter)) != NULL) {
        if (PCIINFOCLASSES(info->device_class)) {
            fprintf(log_handle, "Vendor/Device Id: %x:%x\n", info->vendor_id, info->device_id);
            fprintf(log_handle, "BusID \"PCI:%d@%d:%d:%d\"\n",
                    (int)info->bus, (int)info->domain, (int)info->dev, (int)info->func);
            fprintf(log_handle, "Is boot vga? %s\n", (pci_device_is_boot_vga(info) ? "yes" : "no"));

            /* char *driver = NULL; */
            if (info->vendor_id == NVIDIA) {
                if (!is_device_bound_to_driver(info)) {
                    fprintf(log_handle, "The device is not bound to any driver.\n");
                    continue;
                }

                if (is_device_pci_passthrough(info)) {
                    fprintf(log_handle, "The device is a pci passthrough. Skipping...\n");
                    continue;
                }
                nvidia_devices_n++;

            }
            else {
                fprintf(log_handle, "The device is a not NVIDIA. Skipping...\n");
                continue;
            }
        }
    }

#if 0
    int nvidia_devices_n = 3;
    int nvidia_uvm_loaded = 1;
#endif


    /*
     * TODO: maybe we should load uvm only after all the GPUs are registered?
     *       we should have arguments for just devices, and then one for uvm
     */

    if (nvidia_devices_n > MAX_CARDS_N) {
        fprintf(log_handle, "Warning: too many devices %d. "
                            "Max supported %d. Ignoring the rest.\n",
                            nvidia_devices_n, MAX_CARDS_N);
        nvidia_devices_n = MAX_CARDS_N;
    }

    if (nvidia_devices_n <= 0) {
        fprintf(log_handle, "No NVIDIA devices detected. Skipping...\n");
        goto end;
    }

    /* Create /dev/nvidiactl 195 255 */
    ret = make_nvidiactl_device_node();
    if (log_level > 2)
        fprintf(log_handle, "result of make_nvidiactl_device_node: %d\n", ret);

    for(i = 0; i < nvidia_devices_n; i++) {
        /* Create /dev/nvidiactl 195 255 */
        ret = make_nvidia_device_node(i);
        if (log_level > 2)
            fprintf(log_handle, "result of make_nvidia_device_node: %d\n", ret);
    }

    if (is_module_loaded("nvidia_uvm")) {
    /*if (nvidia_uvm_loaded) {*/
        ret = make_nvidia_uvm_node();
        if (log_level > 2)
            fprintf(log_handle, "result of make_nvidia_uvm_node: %d\n", ret);
        ret = make_nvidia_uvm_tools_node();
        if (log_level > 2)
            fprintf(log_handle, "result of make_nvidia_uvm_tools_node: %d\n", ret);
    }

    if (is_module_loaded("nvidia_modeset")) {
    /*if (nvidia_uvm_loaded) {*/
        ret = make_nvidia_modeset_node();
        if (log_level > 2)
            fprintf(log_handle, "result of make_nvidia_modeset_node: %d\n", ret);
    }

end:
    if (pci_init == 0)
        pci_system_cleanup();

    if (iter)
        free(iter);

    if (log_file)
        free(log_file);

    /* Flush and close the log */
    if (log_handle != stdout) {
        fflush(log_handle);
        fclose(log_handle);
    }

    return 0;
}
