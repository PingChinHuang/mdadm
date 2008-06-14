/*
 * sysfs - extract md related information from sysfs.  Part of:
 * mdadm - manage Linux "md" devices aka RAID arrays.
 *
 * Copyright (C) 2006 Neil Brown <neilb@suse.de>
 *
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *    Author: Neil Brown
 *    Email: <neilb@suse.de>
 */

#include	"mdadm.h"
#include	<dirent.h>

int load_sys(char *path, char *buf)
{
	int fd = open(path, O_RDONLY);
	int n;
	if (fd < 0)
		return -1;
	n = read(fd, buf, 1024);
	close(fd);
	if (n <0 || n >= 1024)
		return -1;
	buf[n] = 0;
	if (buf[n-1] == '\n')
		buf[n-1] = 0;
	return 0;
}

void sysfs_free(struct mdinfo *sra)
{
	while (sra) {
		struct mdinfo *sra2 = sra->next;
		while (sra->devs) {
			struct mdinfo *d = sra->devs;
			sra->devs = d->next;
			free(d);
		}
		free(sra);
		sra = sra2;
	}
}

int sysfs_open(int devnum, char *devname, char *attr)
{
	char fname[50];
	char sys_name[16];
	int fd;
	if (devnum >= 0)
		sprintf(sys_name, "md%d", devnum);
	else
		sprintf(sys_name, "md_d%d",
			-1-devnum);

	sprintf(fname, "/sys/block/%s/md/", sys_name);
	if (devname) {
		strcat(fname, devname);
		strcat(fname, "/");
	}
	strcat(fname, attr);
	fd = open(fname, O_RDWR);
	if (fd < 0 && errno == EACCES)
		fd = open(fname, O_RDONLY);
	return fd;
}

struct mdinfo *sysfs_read(int fd, int devnum, unsigned long options)
{
	/* Longest possible name in sysfs, mounted at /sys, is
	 *  /sys/block/md_dXXX/md/dev-XXXXX/block/dev
	 *  /sys/block/md_dXXX/md/metadata_version
	 * which is about 41 characters.  50 should do for now
	 */
	char fname[50];
	char buf[1024];
	char *base;
	char *dbase;
	struct mdinfo *sra;
	struct mdinfo *dev;
	DIR *dir = NULL;
	struct dirent *de;

	sra = malloc(sizeof(*sra));
	if (sra == NULL)
		return sra;
	sra->next = NULL;

	if (fd >= 0) {
		struct stat stb;
		mdu_version_t vers;
 		if (fstat(fd, &stb)) return NULL;
		if (ioctl(fd, RAID_VERSION, &vers) != 0)
			return NULL;
		if (major(stb.st_rdev)==9)
			sprintf(sra->sys_name, "md%d", (int)minor(stb.st_rdev));
		else
			sprintf(sra->sys_name, "md_d%d",
				(int)minor(stb.st_rdev)>>MdpMinorShift);
	} else {
		if (devnum >= 0)
			sprintf(sra->sys_name, "md%d", devnum);
		else
			sprintf(sra->sys_name, "md_d%d",
				-1-devnum);
	}
	sprintf(fname, "/sys/block/%s/md/", sra->sys_name);
	base = fname + strlen(fname);

	sra->devs = NULL;
	if (options & GET_VERSION) {
		strcpy(base, "metadata_version");
		if (load_sys(fname, buf))
			goto abort;
		if (strncmp(buf, "none", 4) == 0) {
			sra->array.major_version =
				sra->array.minor_version = -1;
			strcpy(sra->text_version, "");
		} else if (strncmp(buf, "external:", 9) == 0) {
			sra->array.major_version = -1;
			sra->array.minor_version = -2;
			strcpy(sra->text_version, buf+9);
		} else
			sscanf(buf, "%d.%d",
			       &sra->array.major_version,
			       &sra->array.minor_version);
	}
	if (options & GET_LEVEL) {
		strcpy(base, "level");
		if (load_sys(fname, buf))
			goto abort;
		sra->array.level = map_name(pers, buf);
	}
	if (options & GET_LAYOUT) {
		strcpy(base, "layout");
		if (load_sys(fname, buf))
			goto abort;
		sra->array.layout = strtoul(buf, NULL, 0);
	}
	if (options & GET_DISKS) {
		strcpy(base, "raid_disks");
		if (load_sys(fname, buf))
			goto abort;
		sra->array.raid_disks = strtoul(buf, NULL, 0);
	}
	if (options & GET_COMPONENT) {
		strcpy(base, "component_size");
		if (load_sys(fname, buf))
			goto abort;
		sra->component_size = strtoull(buf, NULL, 0);
		/* sysfs reports "K", but we want sectors */
		sra->component_size *= 2;
	}
	if (options & GET_CHUNK) {
		strcpy(base, "chunk_size");
		if (load_sys(fname, buf))
			goto abort;
		sra->array.chunk_size = strtoul(buf, NULL, 0);
	}
	if (options & GET_CACHE) {
		strcpy(base, "stripe_cache_size");
		if (load_sys(fname, buf))
			goto abort;
		sra->cache_size = strtoul(buf, NULL, 0);
	}
	if (options & GET_MISMATCH) {
		strcpy(base, "mismatch_cnt");
		if (load_sys(fname, buf))
			goto abort;
		sra->mismatch_cnt = strtoul(buf, NULL, 0);
	}

	if (! (options & GET_DEVS))
		return sra;

	/* Get all the devices as well */
	*base = 0;
	dir = opendir(fname);
	if (!dir)
		goto abort;
	sra->array.spare_disks = 0;

	while ((de = readdir(dir)) != NULL) {
		char *ep;
		if (de->d_ino == 0 ||
		    strncmp(de->d_name, "dev-", 4) != 0)
			continue;
		strcpy(base, de->d_name);
		dbase = base + strlen(base);
		*dbase++ = '/';

		dev = malloc(sizeof(*dev));
		if (!dev)
			goto abort;
		dev->next = sra->devs;
		sra->devs = dev;
		strcpy(dev->sys_name, de->d_name);

		/* Always get slot, major, minor */
		strcpy(dbase, "slot");
		if (load_sys(fname, buf))
			goto abort;
		dev->disk.raid_disk = strtoul(buf, &ep, 10);
		if (*ep) dev->disk.raid_disk = -1;

		strcpy(dbase, "block/dev");
		if (load_sys(fname, buf))
			goto abort;
		sscanf(buf, "%d:%d", &dev->disk.major, &dev->disk.minor);

		if (options & GET_OFFSET) {
			strcpy(dbase, "offset");
			if (load_sys(fname, buf))
				goto abort;
			dev->data_offset = strtoull(buf, NULL, 0);
		}
		if (options & GET_SIZE) {
			strcpy(dbase, "size");
			if (load_sys(fname, buf))
				goto abort;
			dev->component_size = strtoull(buf, NULL, 0);
		}
		if (options & GET_STATE) {
			dev->disk.state = 0;
			strcpy(dbase, "state");
			if (load_sys(fname, buf))
				goto abort;
			if (strstr(buf, "in_sync"))
				dev->disk.state |= (1<<MD_DISK_SYNC);
			if (strstr(buf, "faulty"))
				dev->disk.state |= (1<<MD_DISK_FAULTY);
			if (dev->disk.state == 0)
				sra->array.spare_disks++;
		}
		if (options & GET_ERROR) {
			strcpy(buf, "errors");
			if (load_sys(fname, buf))
				goto abort;
			dev->errors = strtoul(buf, NULL, 0);
		}
	}
	closedir(dir);
	return sra;

 abort:
	if (dir)
		closedir(dir);
	sysfs_free(sra);
	return NULL;
}

unsigned long long get_component_size(int fd)
{
	/* Find out the component size of the array.
	 * We cannot trust GET_ARRAY_INFO ioctl as it's
	 * size field is only 32bits.
	 * So look in /sys/block/mdXXX/md/component_size
	 *
	 * This returns in units of sectors.
	 */
	struct stat stb;
	char fname[50];
	int n;
	if (fstat(fd, &stb)) return 0;
	if (major(stb.st_rdev) == 9)
		sprintf(fname, "/sys/block/md%d/md/component_size",
			(int)minor(stb.st_rdev));
	else
		sprintf(fname, "/sys/block/md_d%d/md/component_size",
			(int)minor(stb.st_rdev)>>MdpMinorShift);
	fd = open(fname, O_RDONLY);
	if (fd < 0)
		return 0;
	n = read(fd, fname, sizeof(fname));
	close(fd);
	if (n == sizeof(fname))
		return 0;
	fname[n] = 0;
	return strtoull(fname, NULL, 10) * 2;
}

int sysfs_set_str(struct mdinfo *sra, struct mdinfo *dev,
		  char *name, char *val)
{
	char fname[50];
	int n;
	int fd;

	sprintf(fname, "/sys/block/%s/md/%s/%s",
		sra->sys_name, dev?dev->sys_name:"", name);
	fd = open(fname, O_WRONLY);
	if (fd < 0)
		return -1;
	n = write(fd, val, strlen(val));
	close(fd);
	if (n != strlen(val))
		return -1;
	return 0;
}

int sysfs_set_num(struct mdinfo *sra, struct mdinfo *dev,
		  char *name, unsigned long long val)
{
	char valstr[50];
	sprintf(valstr, "%llu", val);
	return sysfs_set_str(sra, dev, name, valstr);
}

int sysfs_get_ll(struct mdinfo *sra, struct mdinfo *dev,
		       char *name, unsigned long long *val)
{
	char fname[50];
	char buf[50];
	int n;
	int fd;
	char *ep;
	sprintf(fname, "/sys/block/%s/md/%s/%s",
		sra->sys_name, dev?dev->sys_name:"", name);
	fd = open(fname, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf));
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	*val = strtoull(buf, &ep, 0);
	if (ep == buf || (*ep != 0 && *ep != '\n' && *ep != ' '))
		return -1;
	return 0;
}

int sysfs_set_array(struct mdinfo *sra,
		    struct mdinfo *info)
{
	int rv = 0;
	sra->array = info->array;

	if (info->array.level < 0)
		return 0; /* FIXME */
	rv |= sysfs_set_str(sra, NULL, "level",
			    map_num(pers, info->array.level));
	rv |= sysfs_set_num(sra, NULL, "raid_disks", info->array.raid_disks);
	rv |= sysfs_set_num(sra, NULL, "chunk_size", info->array.chunk_size);
	rv |= sysfs_set_num(sra, NULL, "layout", info->array.layout);
	rv |= sysfs_set_num(sra, NULL, "component_size", info->component_size);
	rv |= sysfs_set_num(sra, NULL, "resync_start", info->resync_start);
	sra->array = info->array;
	return rv;
}

int sysfs_add_disk(struct mdinfo *sra, struct mdinfo *sd)
{
	char dv[100];
	char nm[100];
	struct mdinfo *sd2;
	char *dname;
	int rv;

	sprintf(dv, "%d:%d", sd->disk.major, sd->disk.minor);
	rv = sysfs_set_str(sra, NULL, "new_dev", dv);
	if (rv)
		return rv;

	memset(nm, 0, sizeof(nm));
	sprintf(dv, "/sys/dev/block/%d:%d", sd->disk.major, sd->disk.minor);
	if (readlink(dv, nm, sizeof(nm)) < 0)
		return -1;
	dname = strrchr(nm, '/');
	if (dname) dname++;
	strcpy(sd->sys_name, "dev-");
	strcpy(sd->sys_name+4, dname);

	rv |= sysfs_set_num(sra, sd, "offset", sd->data_offset);
	rv |= sysfs_set_num(sra, sd, "size", (sd->component_size+1) / 2);
	if (sra->array.level != LEVEL_CONTAINER) {
		rv |= sysfs_set_num(sra, sd, "slot", sd->disk.raid_disk);
//		rv |= sysfs_set_str(sra, sd, "state", "in_sync");
	}
	if (! rv) {
		sd2 = malloc(sizeof(*sd2));
		*sd2 = *sd;
		sd2->next = sra->devs;
		sra->devs = sd2;
	}
	return rv;
}

int sysfs_disk_to_sg(int fd)
{
	/* from an open block device, try find and open its corresponding
	 * scsi_generic interface
	 */
	struct stat st;
	char path[256];
	char sg_path[256];
	char sg_major_minor[8];
	char *c;
	DIR *dir;
	struct dirent *de;
	int major, minor, rv;

	if (fstat(fd, &st))
		return -1;

	snprintf(path, sizeof(path), "/sys/dev/block/%d:%d/device",
		 major(st.st_rdev), minor(st.st_rdev));

	dir = opendir(path);
	if (!dir)
		return -1;

	de = readdir(dir);
	while (de) {
		if (strncmp("scsi_generic:", de->d_name,
			    strlen("scsi_generic:")) == 0)
			break;
		de = readdir(dir);
	}
	closedir(dir);

	if (!de)
		return -1;

	snprintf(sg_path, sizeof(sg_path), "%s/%s/dev", path, de->d_name);
	fd = open(sg_path, O_RDONLY);
	if (fd < 0)
		return fd;

	rv = read(fd, sg_major_minor, sizeof(sg_major_minor));
	close(fd);
	if (rv < 0)
		return -1;
	else
		sg_major_minor[rv - 1] = '\0';

	c = strchr(sg_major_minor, ':');
	*c = '\0';
	c++;
	major = strtol(sg_major_minor, NULL, 10);
	minor = strtol(c, NULL, 10);
	snprintf(path, sizeof(path), "/dev/.tmp.md.%d:%d:%d",
		 (int) getpid(), major, minor);
	if (mknod(path, S_IFCHR|0600, makedev(major, minor))==0) {
			fd = open(path, O_RDONLY);
			unlink(path);
			return fd;
	}

	return -1;
}

