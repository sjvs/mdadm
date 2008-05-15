/*
 * mdadm - manage Linux "md" devices aka RAID arrays.
 *
 * Copyright (C) 2001-2006 Neil Brown <neilb@suse.de>
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
 *    Email: <neilb@cse.unsw.edu.au>
 *    Paper: Neil Brown
 *           School of Computer Science and Engineering
 *           The University of New South Wales
 *           Sydney, 2052
 *           Australia
 */

#define	_GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include	<unistd.h>
#if !defined(__dietlibc__) && !defined(__KLIBC__)
extern __off64_t lseek64 __P ((int __fd, __off64_t __offset, int __whence));
#else
# if defined(__NO_STAT64) || __WORDSIZE != 32
# define lseek64 lseek
# endif
#endif

#include	<sys/types.h>
#include	<sys/stat.h>
#include	<stdlib.h>
#include	<time.h>
#include	<sys/time.h>
#include	<getopt.h>
#include	<fcntl.h>
#include	<stdio.h>
#include	<errno.h>
#include	<string.h>
#include	<syslog.h>
#ifdef __dietlibc__
#include	<strings.h>
/* dietlibc has deprecated random and srandom!! */
#define random rand
#define srandom srand
#endif


#include	<linux/kdev_t.h>
/*#include	<linux/fs.h> */
#include	<sys/mount.h>
#include	<asm/types.h>
#include	<sys/ioctl.h>
#define	MD_MAJOR 9
#define MdpMinorShift 6

#ifndef BLKGETSIZE64
#define BLKGETSIZE64 _IOR(0x12,114,size_t) /* return device size in bytes (u64 *arg) */
#endif

#define DEFAULT_BITMAP_CHUNK 4096
#define DEFAULT_BITMAP_DELAY 5
#define DEFAULT_MAX_WRITE_BEHIND 256

#include	"md_u.h"
#include	"md_p.h"
#include	"bitmap.h"
#include	"msg.h"

#include <endian.h>
/* Redhat don't like to #include <asm/byteorder.h>, and
 * some time include <linux/byteorder/xxx_endian.h> isn't enough,
 * and there is no standard conversion function so... */
/* And dietlibc doesn't think byteswap is ok, so.. */
/*  #include <byteswap.h> */
#define bswap_16(x) (((x) & 0x00ffU) << 8 | \
		     ((x) & 0xff00U) >> 8)
#define bswap_32(x) (((x) & 0x000000ffU) << 24 | \
		     ((x) & 0xff000000U) >> 24 | \
		     ((x) & 0x0000ff00U) << 8  | \
		     ((x) & 0x00ff0000U) >> 8)
#define bswap_64(x) (((x) & 0x00000000000000ffULL) << 56 | \
		     ((x) & 0xff00000000000000ULL) >> 56 | \
		     ((x) & 0x000000000000ff00ULL) << 40 | \
		     ((x) & 0x00ff000000000000ULL) >> 40 | \
		     ((x) & 0x0000000000ff0000ULL) << 24 | \
		     ((x) & 0x0000ff0000000000ULL) >> 24 | \
		     ((x) & 0x00000000ff000000ULL) << 8 | \
		     ((x) & 0x000000ff00000000ULL) >> 8)

#if !defined(__KLIBC__)
#if BYTE_ORDER == LITTLE_ENDIAN
#define	__cpu_to_le16(_x) (_x)
#define __cpu_to_le32(_x) (_x)
#define __cpu_to_le64(_x) (_x)
#define	__le16_to_cpu(_x) (_x)
#define __le32_to_cpu(_x) (_x)
#define __le64_to_cpu(_x) (_x)

#define	__cpu_to_be16(_x) bswap_16(_x)
#define __cpu_to_be32(_x) bswap_32(_x)
#define __cpu_to_be64(_x) bswap_64(_x)
#define	__be16_to_cpu(_x) bswap_16(_x)
#define __be32_to_cpu(_x) bswap_32(_x)
#define __be64_to_cpu(_x) bswap_64(_x)
#elif BYTE_ORDER == BIG_ENDIAN
#define	__cpu_to_le16(_x) bswap_16(_x)
#define __cpu_to_le32(_x) bswap_32(_x)
#define __cpu_to_le64(_x) bswap_64(_x)
#define	__le16_to_cpu(_x) bswap_16(_x)
#define __le32_to_cpu(_x) bswap_32(_x)
#define __le64_to_cpu(_x) bswap_64(_x)

#define	__cpu_to_be16(_x) (_x)
#define __cpu_to_be32(_x) (_x)
#define __cpu_to_be64(_x) (_x)
#define	__be16_to_cpu(_x) (_x)
#define __be32_to_cpu(_x) (_x)
#define __be64_to_cpu(_x) (_x)
#else
#  error "unknown endianness."
#endif
#endif /* __KLIBC__ */



/* general information that might be extracted from a superblock */
struct mdinfo {
	mdu_array_info_t	array;
	mdu_disk_info_t		disk;
	__u64			events;
	int			uuid[4];
	char			name[33];
	unsigned long long	data_offset;
	unsigned long long	component_size; /* same as array.size, except in
						 * sectors and up to 64bits.
						 */
	int			reshape_active;
	unsigned long long	reshape_progress;
	unsigned long long	resync_start;
	int			new_level, delta_disks, new_layout, new_chunk;
	int			errors;
	int			cache_size; /* size of raid456 stripe cache*/
	int			mismatch_cnt;
	char			text_version[50];
	int container_member; /* for assembling external-metatdata arrays */

	char 		sys_name[20];
	struct mdinfo *devs;
	struct mdinfo *next;

	/* Device info for mdmon: */
	int state_fd;
	#define DS_FAULTY	1
	#define	DS_INSYNC	2
	#define	DS_WRITE_MOSTLY	4
	#define	DS_SPARE	8
	#define DS_BLOCKED	16
	#define	DS_REMOVE	1024
	int prev_state, curr_state, next_state;

};

struct createinfo {
	int	uid;
	int	gid;
	int	autof;
	int	mode;
	int	symlinks;
	struct supertype *supertype;
};

#define Name "mdadm"

enum mode {
	ASSEMBLE=1,
	BUILD,
	CREATE,
	MANAGE,
	MISC,
	MONITOR,
	GROW,
	INCREMENTAL,
	AUTODETECT,
};

extern char short_options[];
extern char short_bitmap_options[];
extern char short_bitmap_auto_options[];
extern struct option long_options[];
extern char Version[], Usage[], Help[], OptionHelp[],
	Help_create[], Help_build[], Help_assemble[], Help_grow[],
	Help_incr[],
	Help_manage[], Help_misc[], Help_monitor[], Help_config[];

/* for option that don't have short equivilents, we assign arbitrary
 * small numbers.  '1' means an undecorated option, so we start at '2'.
 */
enum special_options {
	AssumeClean = 2,
	BitmapChunk,
	WriteBehind,
	ReAdd,
	NoDegraded,
	Sparc22,
	BackupFile,
	HomeHost,
	AutoHomeHost,
	Symlinks,
	AutoDetect,
};

/* structures read from config file */
/* List of mddevice names and identifiers
 * Identifiers can be:
 *    uuid=128-hex-uuid
 *    super-minor=decimal-minor-number-from-superblock
 *    devices=comma,separated,list,of,device,names,with,wildcards
 *
 * If multiple fields are present, the intersection of all matching
 * devices is considered
 */
#define UnSet (0xfffe)
typedef struct mddev_ident_s {
	char	*devname;

	int	uuid_set;
	int	uuid[4];
	char	name[33];

	unsigned int super_minor;

	char	*devices;	/* comma separated list of device
				 * names with wild cards
				 */
	int	level;
	unsigned int raid_disks;
	unsigned int spare_disks;
	struct supertype *st;
	int	autof;		/* 1 for normal, 2 for partitioned */
	char	*spare_group;
	char	*bitmap_file;
	int	bitmap_fd;

	struct mddev_ident_s *next;
} *mddev_ident_t;

/* List of device names - wildcards expanded */
typedef struct mddev_dev_s {
	char *devname;
	char disposition;	/* 'a' for add, 'r' for remove, 'f' for fail.
				 * Not set for names read from .config
				 */
	char writemostly;
	char re_add;
	char used;		/* set when used */
	struct mddev_dev_s *next;
} *mddev_dev_t;

typedef struct mapping {
	char *name;
	int num;
} mapping_t;


struct mdstat_ent {
	char		*dev;
	int		devnum;
	int		active;
	char		*level;
	char		*pattern; /* U or up, _ for down */
	int		percent; /* -1 if no resync */
	int		resync; /* 1 if resync, 0 if recovery */
	int		devcnt;
	int		raid_disks;
	int		chunk_size;
	char *		metadata_version;
	struct mdstat_ent *next;
};

extern struct mdstat_ent *mdstat_read(int hold, int start);
extern void free_mdstat(struct mdstat_ent *ms);
extern void mdstat_wait(int seconds);
extern void mdstat_wait_fd(int fd);
extern int mddev_busy(int devnum);

struct map_ent {
	struct map_ent *next;
	int	devnum;
	int	major,minor;
	int	uuid[4];
	char	*path;
};
extern int map_update(struct map_ent **mpp, int devnum, int major, int minor,
		      int uuid[4], char *path);
extern struct map_ent *map_by_uuid(struct map_ent **map, int uuid[4]);
extern void map_read(struct map_ent **melp);
extern int map_write(struct map_ent *mel);
extern void map_delete(struct map_ent **mapp, int devnum);
extern void map_free(struct map_ent *map);
extern void map_add(struct map_ent **melp,
		    int devnum, int major, int minor, int uuid[4], char *path);

/* various details can be requested */
#define	GET_LEVEL	1
#define	GET_LAYOUT	2
#define	GET_COMPONENT	4
#define	GET_CHUNK	8
#define GET_CACHE	16
#define	GET_MISMATCH	32
#define	GET_VERSION	64
#define	GET_DISKS	128

#define	GET_DEVS	1024 /* gets role, major, minor */
#define	GET_OFFSET	2048
#define	GET_SIZE	4096
#define	GET_STATE	8192
#define	GET_ERROR	16384

/* If fd >= 0, get the array it is open on,
 * else use devnum. >=0 -> major9. <0.....
 */
extern int sysfs_open(int devnum, char *devname, char *attr);
extern void sysfs_free(struct mdinfo *sra);
extern struct mdinfo *sysfs_read(int fd, int devnum, unsigned long options);
extern int sysfs_set_str(struct mdinfo *sra, struct mdinfo *dev,
			 char *name, char *val);
extern int sysfs_set_num(struct mdinfo *sra, struct mdinfo *dev,
			 char *name, unsigned long long val);
extern int sysfs_get_ll(struct mdinfo *sra, struct mdinfo *dev,
			char *name, unsigned long long *val);
extern int sysfs_set_array(struct mdinfo *sra,
			   struct mdinfo *info);
extern int sysfs_add_disk(struct mdinfo *sra, int fd, struct mdinfo *sd);




extern int save_stripes(int *source, unsigned long long *offsets,
			int raid_disks, int chunk_size, int level, int layout,
			int nwrites, int *dest,
			unsigned long long start, unsigned long long length);
extern int restore_stripes(int *dest, unsigned long long *offsets,
			   int raid_disks, int chunk_size, int level, int layout,
			   int source, unsigned long long read_offset,
			   unsigned long long start, unsigned long long length);

#ifndef Sendmail
#define Sendmail "/usr/lib/sendmail -t"
#endif

#define SYSLOG_FACILITY LOG_DAEMON

extern char *map_num(mapping_t *map, int num);
extern int map_name(mapping_t *map, char *name);
extern mapping_t r5layout[], pers[], modes[], faultylayout[];

extern char *map_dev(int major, int minor, int create);

struct active_array;

extern struct superswitch {
	void (*examine_super)(struct supertype *st, char *homehost);
	void (*brief_examine_super)(struct supertype *st);
	void (*export_examine_super)(struct supertype *st);
	void (*detail_super)(struct supertype *st, char *homehost);
	void (*brief_detail_super)(struct supertype *st);
	void (*export_detail_super)(struct supertype *st);
	void (*uuid_from_super)(struct supertype *st, int uuid[4]);
	void (*getinfo_super)(struct supertype *st, struct mdinfo *info);
	void (*getinfo_super_n)(struct supertype *st, struct mdinfo *info);
	int (*match_home)(struct supertype *st, char *homehost);
	int (*update_super)(struct supertype *st, struct mdinfo *info,
			    char *update,
			    char *devname, int verbose,
			    int uuid_set, char *homehost);
	int (*init_super)(struct supertype *st, mdu_array_info_t *info,
			  unsigned long long size, char *name,
			  char *homehost, int *uuid);
	void (*add_to_super)(struct supertype *st, mdu_disk_info_t *dinfo,
			     int fd, char *devname);
	int (*store_super)(struct supertype *st, int fd);
	int (*write_init_super)(struct supertype *st);
	int (*compare_super)(struct supertype *st, struct supertype *tst);
	int (*load_super)(struct supertype *st, int fd, char *devname);
	struct supertype * (*match_metadata_desc)(char *arg);
	__u64 (*avail_size)(struct supertype *st, __u64 size);
	int (*add_internal_bitmap)(struct supertype *st, int *chunkp,
				   int delay, int write_behind,
				   unsigned long long size, int may_change, int major);
	void (*locate_bitmap)(struct supertype *st, int fd);
	int (*write_bitmap)(struct supertype *st, int fd);
	void (*free_super)(struct supertype *st);
	int (*validate_geometry)(struct supertype *st, int level, int layout,
				 int raiddisks,
				 int chunk, unsigned long long size,
				 char *subdev, unsigned long long *freesize);

	struct mdinfo *(*container_content)(struct supertype *st);

/* for mdmon */
	int (*open_new)(struct supertype *c, struct active_array *a, int inst);
	void (*mark_clean)(struct active_array *a, unsigned long long sync_pos);
	void (*mark_dirty)(struct active_array *a);
	void (*mark_sync)(struct active_array *a, unsigned long long resync);
	void (*set_disk)(struct active_array *a, int n, int state);
	void (*sync_metadata)(struct active_array *a);


	int major;
	char *text_version;
	int swapuuid; /* true if uuid is bigending rather than hostendian */
	int external;
} super0, super1, super_ddf, super_ddf_bvd, super_ddf_svd, *superlist[];

extern struct superswitch super_imsm, super_imsm_raid;

struct supertype {
	struct superswitch *ss;
	int minor_version;
	int max_devs;
	int container_dev;    /* devnum of container */
	int container_member; /* numerical position in container */
	void *sb;
	void *info;

	/* extra stuff used by mdmon */
	struct active_array *arrays;
	int devfd;
	int sock; /* listen to external programs */
	int mgr_pipe[2]; /* communicate between threads */
	int mon_pipe[2]; /* communicate between threads */
	int devnum;
	char *devname; /* e.g. md0.  This appears in metadata_verison:
			*  external:/md0/12
			*/
	int devcnt;

	struct mdinfo *devs;

};

extern struct supertype supertype_container_member;
extern struct supertype *super_by_fd(int fd);
extern struct supertype *guess_super(int fd);
extern struct supertype *dup_super(struct supertype *st);
extern int get_dev_size(int fd, char *dname, unsigned long long *sizep);
extern void get_one_disk(int mdfd, mdu_array_info_t *ainf,
			 mdu_disk_info_t *disk);

#if __GNUC__ < 3
struct stat64;
#endif

#define HAVE_NFTW  we assume
#define HAVE_FTW

#ifdef UCLIBC
# include <features.h>
# ifndef  __UCLIBC_HAS_FTW__
#  undef HAVE_FTW
#  undef HAVE_NFTW
# endif
#endif

#ifdef __dietlibc__
# undef HAVE_NFTW
#endif

#if defined(__KLIBC__)
# undef HAVE_NFTW
# undef HAVE_FTW
#endif

#ifndef HAVE_NFTW
# define FTW_PHYS 1
# ifndef HAVE_FTW
  struct FTW {};
# endif
#endif

#ifdef HAVE_FTW
# include <ftw.h>
#endif

extern int add_dev(const char *name, const struct stat *stb, int flag, struct FTW *s);


extern int Manage_ro(char *devname, int fd, int readonly);
extern int Manage_runstop(char *devname, int fd, int runstop, int quiet);
extern int Manage_resize(char *devname, int fd, long long size, int raid_disks);
extern int Manage_reconfig(char *devname, int fd, int layout);
extern int Manage_subdevs(char *devname, int fd,
			  mddev_dev_t devlist, int verbose);
extern int autodetect(void);
extern int Grow_Add_device(char *devname, int fd, char *newdev);
extern int Grow_addbitmap(char *devname, int fd, char *file, int chunk, int delay, int write_behind, int force);
extern int Grow_reshape(char *devname, int fd, int quiet, char *backup_file,
			long long size,
			int level, int layout, int chunksize, int raid_disks);
extern int Grow_restart(struct supertype *st, struct mdinfo *info,
			int *fdlist, int cnt, char *backup_file);


extern int Assemble(struct supertype *st, char *mddev, int mdfd,
		    mddev_ident_t ident,
		    mddev_dev_t devlist, char *backup_file,
		    int readonly, int runstop,
		    char *update, char *homehost,
		    int verbose, int force);

extern int Build(char *mddev, int mdfd, int chunk, int level, int layout,
		 int raiddisks,
		 mddev_dev_t devlist, int assume_clean,
		 char *bitmap_file, int bitmap_chunk, int write_behind, int delay, int verbose);


extern int Create(struct supertype *st, char *mddev, int mdfd,
		  int chunk, int level, int layout, unsigned long long size, int raiddisks, int sparedisks,
		  char *name, char *homehost, int *uuid,
		  int subdevs, mddev_dev_t devlist,
		  int runstop, int verbose, int force, int assume_clean,
		  char *bitmap_file, int bitmap_chunk, int write_behind, int delay);

extern int Detail(char *dev, int brief, int export, int test, char *homehost);
extern int Query(char *dev);
extern int Examine(mddev_dev_t devlist, int brief, int export, int scan,
		   int SparcAdjust, struct supertype *forcest, char *homehost);
extern int Monitor(mddev_dev_t devlist,
		   char *mailaddr, char *alert_cmd,
		   int period, int daemonise, int scan, int oneshot,
		   int dosyslog, int test, char *pidfile);

extern int Kill(char *dev, int force, int quiet, int noexcl);
extern int Wait(char *dev);

extern int Incremental(char *devname, int verbose, int runstop,
		       struct supertype *st, char *homehost, int autof);
extern int Incremental_container(struct supertype *st, char *devname,
				 int verbose, int runstop, int autof);
extern void RebuildMap(void);
extern int IncrementalScan(int verbose);

extern int CreateBitmap(char *filename, int force, char uuid[16],
			unsigned long chunksize, unsigned long daemon_sleep,
			unsigned long write_behind,
			unsigned long long array_size,
			int major);
extern int ExamineBitmap(char *filename, int brief, struct supertype *st);
extern int bitmap_update_uuid(int fd, int *uuid, int swap);

extern int md_get_version(int fd);
extern int get_linux_version(void);
extern int parse_uuid(char *str, int uuid[4]);
extern int check_ext2(int fd, char *name);
extern int check_reiser(int fd, char *name);
extern int check_raid(int fd, char *name);

extern int get_mdp_major(void);
extern int dev_open(char *dev, int flags);
extern int is_standard(char *dev, int *nump);

extern int parse_auto(char *str, char *msg, int config);
extern mddev_ident_t conf_get_ident(char *dev);
extern mddev_dev_t conf_get_devs(void);
extern int conf_test_dev(char *devname);
extern struct createinfo *conf_get_create_info(void);
extern void set_conffile(char *file);
extern char *conf_get_mailaddr(void);
extern char *conf_get_mailfrom(void);
extern char *conf_get_program(void);
extern char *conf_get_homehost(void);
extern char *conf_line(FILE *file);
extern char *conf_word(FILE *file, int allow_key);
extern void free_line(char *line);
extern int match_oneof(char *devices, char *devname);
extern void uuid_from_super(int uuid[4], mdp_super_t *super);
extern int same_uuid(int a[4], int b[4], int swapuuid);
extern void copy_uuid(void *a, int b[4], int swapuuid);
extern unsigned long calc_csum(void *super, int bytes);
extern int enough(int level, int raid_disks, int layout, int clean,
		   char *avail, int avail_disks);
extern int ask(char *mesg);
extern unsigned long long get_component_size(int fd);
extern void remove_partitions(int fd);
extern unsigned long long calc_array_size(int level, int raid_disks, int layout,
				   int chunksize, unsigned long long devsize);


extern char *human_size(long long bytes);
char *human_size_brief(long long bytes);

#define NoMdDev (1<<23)
extern int find_free_devnum(int use_partitions);

extern void put_md_name(char *name);
extern char *get_md_name(int dev);

extern char DefaultConfFile[];

extern int open_mddev(char *dev, int autof);
extern int open_mddev_devnum(char *devname, int devnum, char *name,
			     char *chosen_name, int parts);
extern int open_container(int fd);

extern char *devnum2devname(int num);
extern int fd2devnum(int fd);

#define	LEVEL_MULTIPATH		(-4)
#define	LEVEL_LINEAR		(-1)
#define	LEVEL_FAULTY		(-5)

/* kernel module doesn't know about these */
#define LEVEL_CONTAINER		(-100)
#define	LEVEL_UNSUPPORTED	(-200)


/* faulty stuff */

#define	WriteTransient	0
#define	ReadTransient	1
#define	WritePersistent	2
#define	ReadPersistent	3
#define	WriteAll	4 /* doesn't go to device */
#define	ReadFixable	5
#define	Modes	6

#define	ClearErrors	31
#define	ClearFaults	30

#define AllPersist	100 /* internal use only */
#define	NoPersist	101

#define	ModeMask	0x1f
#define	ModeShift	5


#ifdef __TINYC__
#undef minor
#undef major
#undef makedev
#define minor(x) ((x)&0xff)
#define major(x) (((x)>>8)&0xff)
#define makedev(M,m) (((M)<<8) | (m))
#endif

/* for raid5 */
#define ALGORITHM_LEFT_ASYMMETRIC	0
#define ALGORITHM_RIGHT_ASYMMETRIC	1
#define ALGORITHM_LEFT_SYMMETRIC	2
#define ALGORITHM_RIGHT_SYMMETRIC	3
