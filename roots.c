/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <ctype.h>

#include "mtdutils/mtdutils.h"
#include "mounts.h"
#include "roots.h"
#include "common.h"
#include "make_ext4fs.h"

#include <fs_mgr.h>
#include <libgen.h>
#include "flashutils/flashutils.h"
#include "extendedcommands.h"

#include "voldclient/voldclient.h"

static struct fstab *fstab = NULL;

static int first_run_done = 0;

// Support additional extra.fstab entries and add device2
// Needed until fs_mgr_read_fstab() starts to parse a blk_device2 entries
static struct fstab *fstab_extra = NULL;
static void add_extra_fstab_entries(int num) {
    int i;
    for(i = 0; i < fstab->num_entries; ++i) {
        if (strcmp(fstab->recs[i].mount_point, fstab_extra->recs[num].mount_point) == 0) {
            fstab->recs[i].blk_device2 = strdup(fstab_extra->recs[num].blk_device);
            fstab->recs[i].fs_type2 = strdup(fstab_extra->recs[num].fs_type);
            if (fstab_extra->recs[num].fs_options != NULL)
                fstab->recs[i].fs_options2 = strdup(fstab_extra->recs[num].fs_options);
        }
    }
}

static void load_volume_table_extra(int filesystem) {
    int i;

    switch (filesystem) {
	case 1:
		fstab_extra = fs_mgr_read_fstab("/etc/extra1.fstab");
		break;
	case 2:
		fstab_extra = fs_mgr_read_fstab("/etc/extra2.fstab");
		break;
    }

    if (!fstab_extra) {
        fstab_extra = fs_mgr_read_fstab("/etc/extra_default.fstab");
    }

    if (!fstab_extra) {
        LOGI("No /etc/extra.fstab\n");
        return;
    }

    fprintf(stderr, "extra filesystem table (device2, fstype2, options2):\n");
    for(i = 0; i < fstab_extra->num_entries; ++i) {
        Volume* v = &fstab_extra->recs[i];
        add_extra_fstab_entries(i);
        fprintf(stderr, "  %d %s %s %s %lld\n", i, v->mount_point, v->fs_type,
                v->blk_device, v->length);
    }
    fprintf(stderr, "\n");

     if (first_run_done == 0) {
 	first_run_done = 1;
    	char init_command[PATH_MAX];
    	sprintf(init_command, "sbin/mount_fs.sh initial");
    	__system(init_command);
    }
}

Volume* volume_for_path_extra(const char* path) {
    return fs_mgr_get_entry_for_mount_point(fstab_extra, path);
}
//----- end extra.fstab support

int get_num_volumes() {
    return fstab->num_entries;
}

Volume* get_device_volumes() {
    return fstab->recs;
}

void load_volume_table() {
    int i;
    int ret;
    int filesystem = 1;

    if (first_run_done != 0) {
	filesystem = get_filesystem();
    }

    switch (filesystem) {
	case 1:
		fstab = fs_mgr_read_fstab("/etc/primary.fstab");
		break;
	case 2:
		fstab = fs_mgr_read_fstab("/etc/secondary.fstab");
		break;
    }

    if (!fstab) {
        fstab = fs_mgr_read_fstab("/etc/default.fstab");
    }

    if (!fstab) {
        LOGE("failed to read /etc/recovery.fstab\n");
        return;
    }

    ret = fs_mgr_add_entry(fstab, "/tmp", "ramdisk", "ramdisk", 0);
    if (ret < 0 ) {
        LOGE("failed to add /tmp entry to fstab\n");
        fs_mgr_free_fstab(fstab);
        fstab = NULL;
        return;
    }

    load_volume_table_extra(filesystem);

    fprintf(stderr, "recovery filesystem table\n");
    fprintf(stderr, "=========================\n");
    for (i = 0; i < fstab->num_entries; ++i) {
        Volume* v = &fstab->recs[i];
        fprintf(stderr, "  %d %s %s %s %lld\n", i, v->mount_point, v->fs_type,
               v->blk_device, v->length);
    }
    fprintf(stderr, "\n");
}

Volume* volume_for_path(const char* path) {
    return fs_mgr_get_entry_for_mount_point(fstab, path);
}

int is_primary_storage_voldmanaged() {
    Volume* v;
    v = volume_for_path("/storage/sdcard0");
    return fs_mgr_is_voldmanaged(v);
}

static char* primary_storage_path = NULL;
char* get_primary_storage_path() {
    if (primary_storage_path == NULL) {
        if (volume_for_path("/storage/sdcard0"))
            primary_storage_path = "/storage/sdcard0";
        else
            primary_storage_path = "/sdcard";
    }
    return primary_storage_path;
}

int get_num_extra_volumes() {
    int num = 0;
    int i;
    for (i = 0; i < get_num_volumes(); i++) {
        Volume* v = get_device_volumes() + i;
        if ((strcmp("/external_sd", v->mount_point) == 0) ||
                ((strcmp(get_primary_storage_path(), v->mount_point) != 0) &&
                fs_mgr_is_voldmanaged(v) && vold_is_volume_available(v->mount_point)))
            num++;
    }
    return num;
}

char** get_extra_storage_paths() {
    int i = 0, j = 0;
    static char* paths[MAX_NUM_MANAGED_VOLUMES];
    int num_extra_volumes = get_num_extra_volumes();

    if (num_extra_volumes == 0)
        return NULL;

    for (i = 0; i < get_num_volumes(); i++) {
        Volume* v = get_device_volumes() + i;
        if ((strcmp("/external_sd", v->mount_point) == 0) ||
                ((strcmp(get_primary_storage_path(), v->mount_point) != 0) &&
                fs_mgr_is_voldmanaged(v) && vold_is_volume_available(v->mount_point))) {
            paths[j] = v->mount_point;
            j++;
        }
    }
    paths[j] = NULL;

    return paths;
}

static char* android_secure_path = NULL;
char* get_android_secure_path() {
    if (android_secure_path == NULL) {
        android_secure_path = malloc(sizeof("/.android_secure") + strlen(get_primary_storage_path()) + 1);
        sprintf(android_secure_path, "%s/.android_secure", primary_storage_path);
    }
    return android_secure_path;
}

int try_mount(const char* device, const char* mount_point, const char* fs_type, const char* fs_options) {
	//LOGE("Trying to mount %s at %s with fs_type: %s and options %s\n", device, mount_point, fs_type, fs_options);
    if (device == NULL || mount_point == NULL || fs_type == NULL) {
        return -1;
    }
    int ret = 0;
    if (fs_options == NULL && strcmp(fs_type, "bind") != 0 && strcmp(fs_type, "img") != 0) {
        ret = mount(device, mount_point, fs_type,
                       MS_NOATIME | MS_NODEV | MS_NODIRATIME, "");
    }
    else {
        char mount_cmd[PATH_MAX];
	if (strcmp(fs_type, "bind") == 0) {
        sprintf(mount_cmd, "mount --bind %s %s", device, mount_point);
	} else if (strcmp(fs_type, "img") == 0) {
        sprintf(mount_cmd, "mount -o loop %s %s", device, mount_point);
	} else {
        sprintf(mount_cmd, "mount -t %s -o%s %s %s", fs_type, fs_options, device, mount_point);
	}
        ret = __system(mount_cmd);
         //LOGE("ret =%d - mount_cmd=%s\n", ret, mount_cmd); // debug
    }
    if (ret == 0) {
        return 0;
    } else if (strcmp(mount_point, "/data") == 0) { 
	ret = __system("sbin/data_mount.sh");
    } else if (strcmp(mount_point, "/system") == 0) { 
	ret = __system("mount system");
    }
    if (ret == 0)
        return 0;
    LOGW("failed to mount %s (%s)\n", device, strerror(errno));
    return ret;
}

int is_data_media() {
    int i;
    int has_sdcard = 0;
    for (i = 0; i < get_num_volumes(); i++) {
        Volume* vol = get_device_volumes() + i;
        if (strcmp(vol->fs_type, "datamedia") == 0)
            return 1;
        if (strcmp(vol->mount_point, "/sdcard") == 0)
            has_sdcard = 1;
        if (fs_mgr_is_voldmanaged(vol) &&
                (strcmp(vol->mount_point, "/storage/sdcard0") == 0))
            has_sdcard = 1;
    }
    return !has_sdcard;
}

void setup_data_media() {
    int i;
    char* mount_point = "/sdcard";
    for (i = 0; i < get_num_volumes(); i++) {
        Volume* vol = get_device_volumes() + i;
        if (strcmp(vol->fs_type, "datamedia") == 0) {
            mount_point = vol->mount_point;
            break;
        }
    }

    LOGI("using /.secondrom/media for %s\n", mount_point);
    rmdir(mount_point);

    mkdir("/.secondrom/media", 0755);
    symlink("/.secondrom/media", mount_point);

}

int is_data_media_volume_path(const char* path) {
    Volume* v = volume_for_path(path);
    if (v != NULL)
        return strcmp(v->fs_type, "datamedia") == 0;

    if (!is_data_media()) {
        return 0;
    }

    return strcmp(path, "/sdcard") == 0 || path == strstr(path, "/sdcard/");
}

int ensure_path_mounted(const char* path) {
    return ensure_path_mounted_at_mount_point(path, NULL);
}

int ensure_path_mounted_at_mount_point(const char* path, const char* mount_point) {
    if (is_data_media_volume_path(path)) {
        if (ui_should_log_stdout()) {
            LOGI("using /data/media for %s.\n", path);
        }
        int ret;
        if (0 != (ret = ensure_path_mounted("/data")))
            return ret;
        setup_data_media();
        return 0;
    }

    Volume* v = volume_for_path(path);
    LOGI("trying to mount %s at %s.\n", v, path);
    if (v == NULL) {
    int ret = 0; // ignore all the errors, which aren't related to /data or /system
    if (strstr(path, "/.secondrom/media/.secondrom/data") != NULL) {
        // bind-mount /data
        char bindmount_command[PATH_MAX];
        sprintf(bindmount_command, "mount -o bind /.secondrom/media/.secondrom/data /data");
        ret = __system(bindmount_command);
    } else if (strcmp(path, "/data") == 0) { 
	ret = __system("sbin/data_mount.sh");
    } else if (strcmp(path, "/system") == 0) { 
	ret = __system("mount system");
    } 
	if (ret == 0) {
	return 0;
	} else {
        LOGE("unknown volume for path [%s]\n", path);
        return -1;
	}
    }

    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // the ramdisk is always mounted.
        return 0;
    }

    int result;
    result = scan_mounted_volumes();
    if (result < 0) {
        LOGE("failed to scan mounted volumes\n");
        return -1;
    }

    if (NULL == mount_point)
        mount_point = v->mount_point;

    const MountedVolume* mv =
        find_mounted_volume_by_mount_point(mount_point);
    if (mv) {
        // volume is already mounted
        return 0;
    }

    mkdir(mount_point, 0755);  // in case it doesn't already exist

    if (fs_mgr_is_voldmanaged(v)) {
        return vold_mount_volume(mount_point, 1) == CommandOkay ? 0 : -1;

    } else if (strcmp(v->fs_type, "yaffs2") == 0) {
        // mount an MTD partition as a YAFFS2 filesystem.
        mtd_scan_partitions();
        const MtdPartition* partition;
        partition = mtd_find_partition_by_name(v->blk_device);
        if (partition == NULL) {
            LOGE("failed to find \"%s\" partition to mount at \"%s\"\n",
                 v->blk_device, mount_point);
            return -1;
        }
        return mtd_mount_partition(partition, mount_point, v->fs_type, 0);
    } else if (strcmp(v->fs_type, "ext4") == 0 ||
               strcmp(v->fs_type, "ext3") == 0 ||
               strcmp(v->fs_type, "rfs") == 0 ||
               strcmp(v->fs_type, "vfat") == 0 ||
               strcmp(v->fs_type, "img") == 0 ||
               strcmp(v->fs_type2, "bind") == 0) {

	    const char *device = NULL;
	    const char* fs_type = NULL;
	    if (!fs_mgr_is_voldmanaged(v)) {
	    	device = v->blk_device2;
		fs_type = v->fs_type2;
            	if (device == NULL)
                    device = v->blk_device;
            	if (fs_type == NULL)
		    fs_type = v->fs_type;
	    } else {
		device = v->blk_device;
		fs_type = v->fs_type;
	    }

        if ((result = try_mount(device, mount_point, fs_type, v->fs_options)) == 0)
            return 0;
        if ((result = try_mount(v->blk_device, mount_point, v->fs_type, v->fs_options)) == 0)
            return 0;
        if ((result = try_mount(v->blk_device, mount_point, v->fs_type2, v->fs_options2)) == 0)
            return 0;
        if ((result = try_mount(v->blk_device2, mount_point, v->fs_type2, v->fs_options2)) == 0)
            return 0;
        return result;
    } else {
        // let's try mounting with the mount binary and hope for the best.
        char mount_cmd[PATH_MAX];
        sprintf(mount_cmd, "mount %s", mount_point);
        return __system(mount_cmd);
    }

    return -1;
}

static int ignore_data_media = 0;

int ensure_path_unmounted(const char* path) {
    // if we are using /data/media, do not ever unmount volumes /data or /sdcard
    /*
    if (is_data_media_volume_path(path)) {
        return ensure_path_unmounted("/data");
    }
    */

    Volume* v = volume_for_path(path);
    if (v == NULL) {
	char umount_cmd[PATH_MAX];
        sprintf(umount_cmd, "umount %s", path);
        return __system(umount_cmd);
    }

    if (is_data_media_volume_path(path)) {
	// don't allow unmounting of data/media
        //return ensure_path_unmounted("/data");
	return 0;
    }

    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // the ramdisk is always mounted; you can't unmount it.
        return -1;
    }

    int result;
    result = scan_mounted_volumes();
    if (result < 0) {
        LOGE("failed to scan mounted volumes\n");
        return -1;
    }

    const MountedVolume* mv =
        find_mounted_volume_by_mount_point(v->mount_point);
    if (mv == NULL) {
        // volume is already unmounted
        return 0;
    }

    if (fs_mgr_is_voldmanaged(volume_for_path(v->mount_point)))
        return vold_unmount_volume(v->mount_point, 0, 1) == CommandOkay ? 0 : -1;

    return unmount_mounted_volume(mv);
}

extern struct selabel_handle *sehandle;

int format_volume(const char* volume) {
    if (is_data_media_volume_path(volume)) {
        return format_unknown_device(NULL, volume, NULL);
    }
    // check to see if /data is being formatted, and if it is /data/media
    // Note: the /sdcard check is redundant probably, just being safe.
    if (strstr(volume, "/data") == volume && is_data_media() && !ignore_data_media) {
        return format_unknown_device(NULL, volume, NULL);
    }

    Volume* v = volume_for_path(volume);
    if (v == NULL) {
        // silent failure for sd-ext
        if (strcmp(volume, "/sd-ext") != 0)
            LOGE("unknown volume '%s'\n", volume);
        return -1;
    }
    // silent failure to format non existing sd-ext when defined in recovery.fstab
    if (strcmp(volume, "/sd-ext") == 0) {
        struct stat s;
        if (0 != stat(v->blk_device, &s)) {
            LOGI("Skipping format of sd-ext\n");
            return -1;
        }
    }

    int blk2 = 0;
    const char *device = NULL;
    if (fs_mgr_is_voldmanaged(v)) {
	device = v->blk_device2;
	if (device == NULL)
            device = v->blk_device;
	else
	     blk2 = 1;
    } else {
	device = v->blk_device;
    }

    if (fs_mgr_is_voldmanaged(v) && strcmp(volume, v->mount_point) == 0 && blk2 == 0) {
        if (ensure_path_unmounted(volume) != 0) {
            LOGE("format_volume failed to unmount %s", v->mount_point);
        }
        return vold_format_volume(v->mount_point, 1) == CommandOkay ? 0 : -1;
    }

    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // you can't format the ramdisk.
        LOGE("can't format_volume \"%s\"", volume);
        return -1;
    }
    if (strcmp(v->mount_point, volume) != 0) {
#if 0
        LOGE("can't give path \"%s\" to format_volume\n", volume);
        return -1;
#endif
        return format_unknown_device(v->blk_device, volume, NULL);
    }

    if (ensure_path_unmounted(volume) != 0) {
        LOGE("format_volume failed to unmount \"%s\"\n", v->mount_point);
        return -1;
    }

    if (strcmp(v->fs_type, "yaffs2") == 0 || strcmp(v->fs_type, "mtd") == 0) {
        mtd_scan_partitions();
        const MtdPartition* partition = mtd_find_partition_by_name(v->blk_device);
        if (partition == NULL) {
            LOGE("format_volume: no MTD partition \"%s\"\n", v->blk_device);
            return -1;
        }

        MtdWriteContext *write = mtd_write_partition(partition);
        if (write == NULL) {
            LOGW("format_volume: can't open MTD \"%s\"\n", v->blk_device);
            return -1;
        } else if (mtd_erase_blocks(write, -1) == (off_t) -1) {
            LOGW("format_volume: can't erase MTD \"%s\"\n", v->blk_device);
            mtd_write_close(write);
            return -1;
        } else if (mtd_write_close(write)) {
            LOGW("format_volume: can't close MTD \"%s\"\n", v->blk_device);
            return -1;
        }
        return 0;
    }

    if (strcmp(v->fs_type, "ext4") == 0) {
	int ret = 1;
	if (strstr(v->blk_device, "data.img") != NULL) {
	   ret = 0;
    	   static char tmp[PATH_MAX];
           sprintf(tmp, "mount data; rm -rf /data/*; rm -rf /data/.*");
	   __system(tmp);
	}
	if (strstr(v->blk_device, "system.img") != NULL) {
	   ret = 0;
    	   static char tmp[PATH_MAX];
           sprintf(tmp, "mount system; rm -rf /system/*; rm -rf /system/.*");
	   __system(tmp);
	}
	if (ret != 0) {
            int result = make_ext4fs(device, v->length, volume, sehandle);
            if (result != 0) {
            	LOGE("format_volume: make_extf4fs failed on %s\n", device);
            	return -1;
             }
	}
        return 0;
    }

#ifdef USE_F2FS
    if (strcmp(v->fs_type, "f2fs") == 0) {
        int result = make_f2fs_main(v->blk_device, v->mount_point);
        if (result != 0) {
            LOGE("format_volume: mkfs.f2f2 failed on %s\n", v->blk_device);
            return -1;
        }
        return 0;
    }
#endif

#if 0
    LOGE("format_volume: fs_type \"%s\" unsupported\n", v->fs_type);
    return -1;
#endif
    return format_unknown_device(v->blk_device, volume, v->fs_type);
}

void ignore_data_media_workaround(int ignore) {
  ignore_data_media = ignore;
}

void setup_legacy_storage_paths() {
    char* primary_path = get_primary_storage_path();

    if (!is_data_media_volume_path(primary_path)) {
        rmdir("/sdcard");
        symlink(primary_path, "/sdcard");
    }
}
