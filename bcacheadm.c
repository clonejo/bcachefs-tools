/*
 * Authors: Kent Overstreet <kmo@daterainc.com>
 *	    Gabriel de Perthuis <g2p.code@gmail.com>
 *	    Jacob Malevich <jam@datera.io>
 *
 * GPLv2
 */

#include <nih/option.h>
#include <nih/command.h>
#include <nih/main.h>
#include <nih/logging.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <blkid.h>
#include <string.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <uuid/uuid.h>
#include <dirent.h>
#include <bcache.h> //libbcache

#define PACKAGE_NAME "bcacheadm"
#define PACKAGE_VERSION "1.0"
#define PACKAGE_BUGREPORT "bugreport"

#define MAX_DEVS MAX_CACHES_PER_SET


/* bcacheadm globals */
enum exit {
	EXIT_OK = 0,		/* Ok */
	EXIT_ERROR = 1,		/* General/OS error */
	EXIT_SHELL = 2,		/* Start maintenance shell */
	EXIT_SHELL_REBOOT = 3,	/* Start maintenance shell, reboot when done */
	EXIT_REBOOT = 4,	/* System must reboot */
};


/* make-bcache globals */
int bdev = -1;
int devs = 0;
char *cache_devices[MAX_DEVS];
int tier_mapping[MAX_DEVS];
char *backing_devices[MAX_DEVS];
char *backing_dev_labels[MAX_DEVS];
size_t i, nr_backing_devices = 0, nr_cache_devices = 0;
unsigned block_size = 0;
unsigned bucket_sizes[MAX_DEVS];
int num_bucket_sizes = 0;
int writeback = 0, discard = 0, wipe_bcache = 0;
unsigned replication_set = 0, replacement_policy = 0;
uint64_t data_offset = BDEV_DATA_START_DEFAULT;
char *label = NULL;
struct cache_sb *cache_set_sb = NULL;
enum long_opts {
	CACHE_SET_UUID = 256,
	CSUM_TYPE,
	REPLICATION_SET,
	META_REPLICAS,
	DATA_REPLICAS,
};
const char *cache_set_uuid = 0;
const char *csum_type = 0;
char *metadata_replicas = 0;
char *data_replicas = 0;
char *tier = 0;


/* super-show globals */
bool force_csum = false;

/* probe globals */
bool udev = false;

/* list globals */
char *cset_dir = "/sys/fs/bcache";
bool list_devs = false;

/* status globals */
bool status_all = false;

/* stats globals */
bool stats_all = false;
bool stats_list = false;
static const char *stats_uuid = NULL;
static const char *stats_cache_num = NULL;
bool stats_five_min = false;
bool stats_hour = false;
bool stats_day = false;
bool stats_total = false;

/* make-bcache option setters */
static int set_block_size(NihOption *option, const char *arg)
{
	block_size = hatoi_validate(arg, "block size");
	return 0;
}

static int set_cache(NihOption *option, const char *arg)
{
	bdev = 0;
	cache_devices[nr_cache_devices] = (char *)malloc(sizeof(char *) *
			strlen(arg) + 1);
	strcpy(cache_devices[nr_cache_devices], arg);
	if(!tier)
		tier_mapping[nr_cache_devices] = 0;
	else
		tier_mapping[nr_cache_devices] = atoi(tier);

	devs++;
	nr_cache_devices++;
}

static int set_bdev(NihOption *option, const char *arg)
{
	bdev = 1;

	if(label) {
		backing_dev_labels[nr_backing_devices] =
			(char *)malloc(sizeof(char *) * strlen(label) + 1);
		strcpy(backing_dev_labels[nr_backing_devices], label);
	}

	backing_devices[nr_backing_devices] = (char *)malloc(sizeof(char *) *
			strlen(arg) + 1);
	strcpy(backing_devices[nr_backing_devices], arg);

	nr_backing_devices++;
	devs++;

	return 0;
}

static int set_bucket_sizes(NihOption *option, const char *arg)
{
	bucket_sizes[num_bucket_sizes]=hatoi_validate(arg, "bucket size");
	num_bucket_sizes++;
	return 0;
}

/* probe setters */
static int set_udev(NihOption *option, const char *arg)
{
	if (strcmp("udev", arg)) {
		printf("Invalid output format %s\n", arg);
		exit(EXIT_FAILURE);
	}
	udev = true;
	return 0;
}


/* options */
static NihOption make_bcache_options[] = {
//	{int shortoption, char* longoption, char* help, NihOptionGroup, char* argname, void *value, NihOptionSetter}
	{'C', "cache",	N_("Format a cache device"), NULL, "dev", NULL, set_cache},
	{'B', "bdev",	N_("Format a backing device"), NULL, "dev", NULL, set_bdev},
	{'l', "label",	N_("label"), NULL, "label", &label, NULL},
	//Only one bucket_size supported until a list of bucket sizes is parsed correctly
	{'b', "bucket",	N_("bucket size"), NULL, "size", NULL, set_bucket_sizes},
	//Does the default setter automatically convert strings to an int?
	{'w', "block",	N_("block size (hard sector size of SSD, often 2k"), NULL,"size", NULL, set_block_size},
	{'t', "tier",	N_("tier of subsequent devices"), NULL,"#", &tier, NULL},
	{'p', "cache_replacement_policy", N_("one of (lru|fifo|random)"), NULL,"policy", &replacement_policy, NULL},
	{'o', "data_offset", N_("data offset in sectors"), NULL,"offset", &data_offset, NULL},

	{0, "cset-uuid",	N_("UUID for the cache set"),		NULL,	"uuid", &cache_set_uuid, NULL},
	{0, "csum-type",	N_("One of (none|crc32c|crc64)"),		NULL,	"type", &csum_type, NULL },
	{0, "replication-set",N_("replication set of subsequent devices"),	NULL,	NULL, &replication_set, NULL },
	{0, "meta-replicas",N_("number of metadata replicas"),		NULL,	"#", &metadata_replicas, NULL},
	{0, "data-replicas",N_("number of data replicas"),		NULL,	"#", &data_replicas, NULL },

	{0, "wipe-bcache",	N_("destroy existing bcache data if present"),		NULL, NULL, &wipe_bcache, NULL},
	{0, "discard",		N_("enable discards"),		NULL, NULL, &discard,		NULL},
	{0, "writeback",	N_("enable writeback"),		NULL, NULL, &writeback, 	NULL},

	NIH_OPTION_LAST
};

static NihOption probe_bcache_options[] = {
	{'o', "udev", N_("udev"), NULL, NULL, NULL, set_udev},
	NIH_OPTION_LAST
};

static NihOption bcache_register_options[] = {
	NIH_OPTION_LAST
};

static NihOption query_devs_options[] = {
	{'f', "force_csum", N_("force_csum"), NULL, NULL, &force_csum, NULL},
	NIH_OPTION_LAST
};

static NihOption list_cachesets_options[] = {
	{'d', "dir", N_("directory"), NULL, NULL, &cset_dir, NULL},
	{0, "list-devs", N_("list all devices in the cache sets as well"), NULL, NULL, &list_devs, NULL},
	NIH_OPTION_LAST
};

static NihOption status_options[] = {
	{'a', "all", N_("all"), NULL, NULL, &status_all, NULL},
	NIH_OPTION_LAST
};

static NihOption stats_options[] = {
	{'a', "all", N_("all"), NULL, NULL, &stats_all, NULL},
	{'l', "list", N_("list"), NULL, NULL, &stats_list, NULL},
	{'u', "uuid", N_("cache_set UUID"), NULL, "UUID", &stats_uuid, NULL},
	{'c', "cache", N_("cache number (starts from 0)"), NULL, "CACHE#", &stats_cache_num, NULL},
	{0, "five-min-stats", N_("stats accumulated in last 5 minutes"), NULL, NULL, &stats_five_min, NULL},
	{0, "hour-stats", N_("stats accumulated in last hour"), NULL, NULL, &stats_hour, NULL},
	{0, "day-stats", N_("stats accumulated in last day"), NULL, NULL, &stats_day, NULL},
	{0, "total-stats", N_("stats accumulated in total"), NULL, NULL, &stats_total, NULL},
	NIH_OPTION_LAST
};

static NihOption options[] = {
	NIH_OPTION_LAST
};


/* commands */
int make_bcache (NihCommand *command, char *const *args)
{
	int cache_dev_fd[devs];

	int backing_dev_fd[devs];

	cache_set_sb = calloc(1, sizeof(*cache_set_sb) +
				     sizeof(struct cache_member) * devs);

	if (cache_set_uuid) {
		if(uuid_parse(cache_set_uuid, cache_set_sb->set_uuid.b)) {
			fprintf(stderr, "Bad uuid\n");
			return -1;
		}
	} else {
		uuid_generate(cache_set_sb->set_uuid.b);
	}

	if (csum_type) {
		SET_CACHE_PREFERRED_CSUM_TYPE(cache_set_sb,
				read_string_list_or_die(csum_type, csum_types,
					"csum type"));
	} else {
		SET_CACHE_PREFERRED_CSUM_TYPE(cache_set_sb, BCH_CSUM_CRC32C);
	}

	if (metadata_replicas) {
		SET_CACHE_SET_META_REPLICAS_WANT(cache_set_sb,
				strtoul_or_die(metadata_replicas,
					CACHE_SET_META_REPLICAS_WANT_MAX,
					"meta replicas"));
	} else {
		SET_CACHE_SET_META_REPLICAS_WANT(cache_set_sb, 1);
	}

	if (data_replicas) {
		SET_CACHE_SET_DATA_REPLICAS_WANT(cache_set_sb,
			strtoul_or_die(data_replicas,
				CACHE_SET_DATA_REPLICAS_WANT_MAX,
				"data replicas"));
	} else {
		SET_CACHE_SET_DATA_REPLICAS_WANT(cache_set_sb, 1);
	}

	if (bdev == -1) {
		fprintf(stderr, "Please specify -C or -B\n");
		exit(EXIT_FAILURE);
	}

	if(!bucket_sizes[0]) bucket_sizes[0] = 1024;

	for(i = 0; i < nr_cache_devices; i++)
		next_cache_device(cache_set_sb,
				  replication_set,
				  tier_mapping[i],
				  replacement_policy,
				  discard);

	if (!cache_set_sb->nr_in_set && !nr_backing_devices) {
		fprintf(stderr, "Please supply a device\n");
		exit(EXIT_FAILURE);
	}

	i = 0;
	do {
		if (bucket_sizes[i] < block_size) {
			fprintf(stderr,
			"Bucket size cannot be smaller than block size\n");
			exit(EXIT_FAILURE);
		}
		i++;
	} while (i < num_bucket_sizes);

	if (!block_size) {
		for (i = 0; i < cache_set_sb->nr_in_set; i++)
			block_size = max(block_size,
					 get_blocksize(cache_devices[i]));

		for (i = 0; i < nr_backing_devices; i++)
			block_size = max(block_size,
					 get_blocksize(backing_devices[i]));
	}

	for (i = 0; i < cache_set_sb->nr_in_set; i++)
		cache_dev_fd[i] = dev_open(cache_devices[i], wipe_bcache);

	for (i = 0; i < nr_backing_devices; i++)
		backing_dev_fd[i] = dev_open(backing_devices[i], wipe_bcache);

	write_cache_sbs(cache_dev_fd, cache_set_sb, block_size,
					bucket_sizes, num_bucket_sizes);

	for (i = 0; i < nr_backing_devices; i++)
		write_backingdev_sb(backing_dev_fd[i],
				    block_size, bucket_sizes,
				    writeback, data_offset,
				    backing_dev_labels[i],
				    cache_set_sb->set_uuid);


	return 0;
}

int probe_bcache (NihCommand *command, char *const *args)
{
	int i;

	for (i = 0; args[i] != NULL; i++) {
		probe(args[i], udev);
	}

	return 0;
}

int bcache_register (NihCommand *command, char *const *args)
{
	int ret = register_bcache(args);

	return ret;
}

int bcache_list_cachesets (NihCommand *command, char *const *args)
{
	return list_cachesets(cset_dir, list_devs);
}

int bcache_query_devs (NihCommand *command, char *const *args)
{
	int i;


	for (i = 0; args[i] != NULL; i++) {
		printf("query-devs on: %s\n", args[i]);
		struct cache_sb *sb = query_dev(args[i], false);
		print_dev_info(sb, force_csum);
	}
}

int bcache_status (NihCommand *command, char *const *args)
{
	int i;
	struct cache_sb *sb_tier0 = NULL, *sb_tier1 = NULL;
	char *dev0 = NULL, *dev1 = NULL;

	for (i = 0; args[i] != NULL; i++) {
		struct cache_sb *sb = query_dev(args[i], false);
		struct cache_member *m = ((struct cache_member *) sb->d) +
			sb->nr_this_dev;
		long long unsigned cache_tier = CACHE_TIER(m);

		if (!cache_tier)
			if (!sb_tier0 || sb->seq > sb_tier0->seq) {
				sb_tier0 = sb;
				dev0 = args[i];
			}
		else if (cache_tier == 1)
			if (!sb_tier1 || sb->seq > sb_tier1->seq) {
				sb_tier1 = sb;
				dev1 = args[i];
			}
	}
	if (sb_tier0) sb_state(sb_tier0, dev0);
	if (sb_tier1) sb_state(sb_tier1, dev1);
}

static void stats_subdir(char* stats_dir)
{
	char tmp[50] = "/";
	if(stats_cache_num) {
		strcat(tmp, "cache");
		strcat(tmp, stats_cache_num);
	} else if (stats_five_min)
		strcat(tmp, "stats_five_minute");
	else if (stats_hour)
		strcat(tmp, "stats_hour");
	else if (stats_day)
		strcat(tmp, "stats_day");
	else if (stats_total)
		strcat(tmp, "stats_total");
	else
		return;

	strcat(stats_dir, tmp);
}

int bcache_stats (NihCommand *command, char *const *args)
{
	int i;
	char stats_dir[200];
	DIR *dir = NULL;
	struct dirent *ent = NULL;

	if (stats_uuid) {
		strcpy(stats_dir, cset_dir);
		strcat(stats_dir, "/");
		strcat(stats_dir, stats_uuid);
		stats_subdir(stats_dir);
		dir = opendir(stats_dir);
		if (!dir) {
			fprintf(stderr, "Failed to open dir %s\n", cset_dir);
			return 1;
		}
	} else {
		printf("Must provide a cacheset uuid\n");
		exit(EXIT_FAILURE);
	}

	if(stats_list || stats_all)
		while ((ent = readdir(dir)) != NULL)
			read_stat_dir(dir, stats_dir, ent->d_name, stats_all);


	for (i = 0; args[i] != NULL; i++)
		read_stat_dir(dir, stats_dir, args[i], true);

	closedir(dir);
}

static NihCommand commands[] = {
	{"format", N_("format <list of drives>"),
		  "Format one or a list of devices with bcache datastructures."
		  " You need to do this before you create a volume",
		  N_("format drive[s] with bcache"),
		  NULL, make_bcache_options, make_bcache},
	{"probe", N_("probe <list of devices>"),
		  "Does a blkid_probe on a device",
		  N_("Does a blkid_probe on a device"),
		  NULL, probe_bcache_options, probe_bcache},
	{"register", N_("register <list of devices>"),
		     "Registers a list of devices",
		     N_("Registers a list of devices"),
		     NULL, bcache_register_options, bcache_register},
	{"list-cachesets", N_("list-cachesets"),
			   "Lists cachesets in /sys/fs/bcache",
			   N_("Lists cachesets in /sys/fs/bcache"),
			   NULL, list_cachesets_options, bcache_list_cachesets},
	{"query-devs", N_("query <list of devices>"),
		       "Gives info about the superblock of a list of devices",
		       N_("show superblock on each of the listed drive"),
		       NULL, query_devs_options, bcache_query_devs},
	{"status", N_("status <list of devices>"),
		   "Finds the status of the most up to date superblock",
		   N_("Finds the status of the most up to date superblock"),
		   NULL, status_options, bcache_status},
	{"stats", N_("stats <list of devices>"),
		  "List various bcache statistics",
		  N_("List various bcache statistics"),
		  NULL, stats_options, bcache_stats},
	NIH_COMMAND_LAST
};


int main(int argc, char *argv[])
{
	int ret = 0;
	nih_main_init (argv[0]);

	nih_option_set_synopsis (_("Manage bcache devices"));
	nih_option_set_help (
			_("Helps you manage bcache devices"));

	ret = nih_command_parser (NULL, argc, argv, options, commands);
	if (ret < 0)
		exit (1);

	nih_signal_reset();
}