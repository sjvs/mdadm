/*
 * mdadm - manage Linux "md" devices aka RAID arrays.
 *
 * Copyright (C) 2001-2009 Neil Brown <neilb@suse.de>
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

#include "mdadm.h"

#define REGISTER_DEV		_IO (MD_MAJOR, 1)
#define START_MD		_IO (MD_MAJOR, 2)
#define STOP_MD			_IO (MD_MAJOR, 3)

int Build(char *mddev, struct mddev_dev *devlist,
	  struct shape *s, struct context *c)
{
	/* Build a linear or raid0 arrays without superblocks
	 * We cannot really do any checks, we just do it.
	 * For md_version < 0.90.0, we call REGISTER_DEV
	 * with the device numbers, and then
	 * START_MD giving the "geometry"
	 * geometry is 0xpp00cc
	 * where pp is personality: 1==linear, 2=raid0
	 * cc = chunk size factor: 0==4k, 1==8k etc.
	 *
	 * For md_version >= 0.90.0 we call
	 * SET_ARRAY_INFO,  ADD_NEW_DISK, RUN_ARRAY
	 *
	 */
	int i;
	int vers;
	struct stat stb;
	int subdevs = 0, missing_disks = 0;
	struct mddev_dev *dv;
	int bitmap_fd;
	unsigned long long bitmapsize;
	int mdfd;
	char chosen_name[1024];
	int uuid[4] = {0,0,0,0};
	struct map_ent *map = NULL;

	/* scan all devices, make sure they really are block devices */
	for (dv = devlist; dv; dv=dv->next) {
		subdevs++;
		if (strcmp("missing", dv->devname) == 0) {
			missing_disks++;
			continue;
		}
		if (stat(dv->devname, &stb)) {
			pr_err("Cannot find %s: %s\n",
				dv->devname, strerror(errno));
			return 1;
		}
		if ((stb.st_mode & S_IFMT) != S_IFBLK) {
			pr_err("%s is not a block device.\n",
				dv->devname);
			return 1;
		}
	}

	if (s->raiddisks != subdevs) {
		pr_err("requested %d devices in array but listed %d\n",
			s->raiddisks, subdevs);
		return 1;
	}

	if (s->layout == UnSet)
		switch(s->level) {
		default: /* no layout */
			s->layout = 0;
			break;
		case 10:
			s->layout = 0x102; /* near=2, far=1 */
			if (c->verbose > 0)
				pr_err("layout defaults to n1\n");
			break;
		case 5:
		case 6:
			s->layout = map_name(r5layout, "default");
			if (c->verbose > 0)
				pr_err("layout defaults to %s\n", map_num(r5layout, s->layout));
			break;
		case LEVEL_FAULTY:
			s->layout = map_name(faultylayout, "default");

			if (c->verbose > 0)
				pr_err("layout defaults to %s\n", map_num(faultylayout, s->layout));
			break;
		}

	/* We need to create the device.  It can have no name. */
	map_lock(&map);
	mdfd = create_mddev(mddev, NULL, c->autof, LOCAL,
			    chosen_name);
	if (mdfd < 0) {
		map_unlock(&map);
		return 1;
	}
	mddev = chosen_name;

	map_update(&map, fd2devnm(mdfd), "none", uuid, chosen_name);
	map_unlock(&map);

	vers = md_get_version(mdfd);

	/* looks Ok, go for it */
	if (vers >= 9000) {
		mdu_array_info_t array;
		array.level = s->level;
		if (s->size == MAX_SIZE)
			s->size = 0;
		array.size = s->size;
		array.nr_disks = s->raiddisks;
		array.raid_disks = s->raiddisks;
		array.md_minor = 0;
		if (fstat(mdfd, &stb)==0)
			array.md_minor = minor(stb.st_rdev);
		array.not_persistent = 1;
		array.state = 0; /* not clean, but no errors */
		if (s->assume_clean)
			array.state |= 1;
		array.active_disks = s->raiddisks - missing_disks;
		array.working_disks = s->raiddisks - missing_disks;
		array.spare_disks = 0;
		array.failed_disks = missing_disks;
		if (s->chunk == 0 && (s->level==0 || s->level==LEVEL_LINEAR))
			s->chunk = 64;
		array.chunk_size = s->chunk*1024;
		array.layout = s->layout;
		if (ioctl(mdfd, SET_ARRAY_INFO, &array)) {
			pr_err("SET_ARRAY_INFO failed for %s: %s\n",
				mddev, strerror(errno));
			goto abort;
		}
	} else if (s->bitmap_file) {
		pr_err("bitmaps not supported with this kernel\n");
		goto abort;
	}

	if (s->bitmap_file && strcmp(s->bitmap_file, "none") == 0)
		s->bitmap_file = NULL;
	if (s->bitmap_file && s->level <= 0) {
		pr_err("bitmaps not meaningful with level %s\n",
			map_num(pers, s->level)?:"given");
		goto abort;
	}
	/* now add the devices */
	for ((i=0), (dv = devlist) ; dv ; i++, dv=dv->next) {
		unsigned long long dsize;
		int fd;
		if (strcmp("missing", dv->devname) == 0)
			continue;
		if (stat(dv->devname, &stb)) {
			pr_err("Weird: %s has disappeared.\n",
				dv->devname);
			goto abort;
		}
		if ((stb.st_mode & S_IFMT)!= S_IFBLK) {
			pr_err("Weird: %s is no longer a block device.\n",
				dv->devname);
			goto abort;
		}
		fd = open(dv->devname, O_RDONLY|O_EXCL);
		if (fd < 0) {
			pr_err("Cannot open %s: %s\n",
				dv->devname, strerror(errno));
			goto abort;
		}
		if (get_dev_size(fd, NULL, &dsize) &&
		    (s->size == 0 || s->size == MAX_SIZE || dsize < s->size))
				s->size = dsize;
		close(fd);
		if (vers >= 9000) {
			mdu_disk_info_t disk;
			disk.number = i;
			disk.raid_disk = i;
			disk.state = (1<<MD_DISK_SYNC) | (1<<MD_DISK_ACTIVE);
			if (dv->writemostly == 1)
				disk.state |= 1<<MD_DISK_WRITEMOSTLY;
			disk.major = major(stb.st_rdev);
			disk.minor = minor(stb.st_rdev);
			if (ioctl(mdfd, ADD_NEW_DISK, &disk)) {
				pr_err("ADD_NEW_DISK failed for %s: %s\n",
					dv->devname, strerror(errno));
				goto abort;
			}
		} else {
			if (ioctl(mdfd, REGISTER_DEV, &stb.st_rdev)) {
				pr_err("REGISTER_DEV failed for %s: %s.\n",
					dv->devname, strerror(errno));
				goto abort;
			}
		}
	}
	/* now to start it */
	if (vers >= 9000) {
		mdu_param_t param; /* not used by syscall */
		if (s->bitmap_file) {
			bitmap_fd = open(s->bitmap_file, O_RDWR);
			if (bitmap_fd < 0) {
				int major = BITMAP_MAJOR_HI;
#if 0
				if (s->bitmap_chunk == UnSet) {
					pr_err("%s cannot be openned.",
						s->bitmap_file);
					goto abort;
				}
#endif
				if (vers < 9003) {
					major = BITMAP_MAJOR_HOSTENDIAN;
#ifdef __BIG_ENDIAN
					pr_err("Warning - bitmaps created on this kernel are not portable\n"
						"  between different architectures.  Consider upgrading the Linux kernel.\n");
#endif
				}
				bitmapsize = s->size>>9; /* FIXME wrong for RAID10 */
				if (CreateBitmap(s->bitmap_file, 1, NULL, s->bitmap_chunk,
						 c->delay, s->write_behind, bitmapsize, major)) {
					goto abort;
				}
				bitmap_fd = open(s->bitmap_file, O_RDWR);
				if (bitmap_fd < 0) {
					pr_err("%s cannot be openned.",
						s->bitmap_file);
					goto abort;
				}
			}
			if (bitmap_fd >= 0) {
				if (ioctl(mdfd, SET_BITMAP_FILE, bitmap_fd) < 0) {
					pr_err("Cannot set bitmap file for %s: %s\n",
						mddev, strerror(errno));
					goto abort;
				}
			}
		}
		if (ioctl(mdfd, RUN_ARRAY, &param)) {
			pr_err("RUN_ARRAY failed: %s\n",
				strerror(errno));
			if (s->chunk & (s->chunk-1)) {
				cont_err("Problem may be that chunk size is not a power of 2\n");
			}
			goto abort;
		}
	} else {
		unsigned long arg;
		arg=0;
		while (s->chunk > 4096) {
			arg++;
			s->chunk >>= 1;
		}
		if (s->level == 0)
			arg |= 0x20000;
		else
			arg |= 0x10000;
		if (ioctl(mdfd, START_MD, arg)) {
			pr_err("START_MD failed: %s\n",
				strerror(errno));
			goto abort;
		}
	}
	if (c->verbose >= 0)
		pr_err("array %s built and started.\n",
			mddev);
	wait_for(mddev, mdfd);
	close(mdfd);
	return 0;

 abort:
	if (vers >= 9000)
	    ioctl(mdfd, STOP_ARRAY, 0);
	else
	    ioctl(mdfd, STOP_MD, 0);
	close(mdfd);
	return 1;
}
