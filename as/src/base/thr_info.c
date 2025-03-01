/*
 * thr_info.c
 *
 * Copyright (C) 2008-2022 Aerospike, Inc.
 *
 * Portions may be licensed to Aerospike, Inc. under one or more contributor
 * license agreements.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 */

#include "base/thr_info.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <malloc.h>
#include <mcheck.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "aerospike/as_atomic.h"
#include "citrusleaf/alloc.h"
#include "citrusleaf/cf_queue.h"

#include "cf_mutex.h"
#include "cf_str.h"
#include "cf_thread.h"
#include "dns.h"
#include "dynbuf.h"
#include "fetch.h"
#include "log.h"
#include "msgpack_in.h"
#include "os.h"
#include "shash.h"
#include "socket.h"
#include "vault.h"
#include "vector.h"
#include "xmem.h"

#include "base/batch.h"
#include "base/cfg.h"
#include "base/datamodel.h"
#include "base/features.h"
#include "base/health.h"
#include "base/index.h"
#include "base/monitor.h"
#include "base/nsup.h"
#include "base/security.h"
#include "base/service.h"
#include "base/set_index.h"
#include "base/smd.h"
#include "base/stats.h"
#include "base/thr_info_port.h"
#include "base/thr_tsvc.h"
#include "base/transaction.h"
#include "base/truncate.h"
#include "base/udf_cask.h"
#include "base/xdr.h"
#include "fabric/exchange.h"
#include "fabric/fabric.h"
#include "fabric/hb.h"
#include "fabric/hlc.h"
#include "fabric/migrate.h"
#include "fabric/partition.h"
#include "fabric/partition_balance.h"
#include "fabric/roster.h"
#include "fabric/service_list.h"
#include "fabric/skew_monitor.h"
#include "query/query.h"
#include "sindex/sindex.h"
#include "storage/storage.h"
#include "transaction/proxy.h"
#include "transaction/rw_request_hash.h"


void info_set_num_info_threads(uint32_t n_threads);
int info_get_objects(char *name, cf_dyn_buf *db);
int info_get_tree_sets(char *name, char *subtree, cf_dyn_buf *db);
int info_get_tree_bins(char *name, char *subtree, cf_dyn_buf *db);
int info_get_tree_sindexes(char *name, char *subtree, cf_dyn_buf *db);


as_stats g_stats = { 0 }; // separate .c file not worth it

cf_dyn_buf g_bad_practices = { 0 };

uint64_t g_start_sec; // start time of the server

static cf_queue *g_info_work_q = 0;

//
// The dynamic list has a name, and a function to call
//

typedef struct info_static_s {
	struct info_static_s	*next;
	bool   def; // default, but default is a reserved word
	char *name;
	char *value;
	size_t	value_sz;
} info_static;


typedef struct info_dynamic_s {
	struct info_dynamic_s *next;
	bool 	def;  // default, but that's a reserved word
	char *name;
	as_info_get_value_fn	value_fn;
} info_dynamic;

typedef struct info_command_s {
	struct info_command_s *next;
	char *name;
	as_info_command_fn 		command_fn;
	as_sec_perm				required_perm; // required security permission
} info_command;

typedef struct info_tree_s {
	struct info_tree_s *next;
	char *name;
	as_info_get_tree_fn	tree_fn;
} info_tree;


#define EOL		'\n' // incoming commands are separated by EOL
#define SEP		'\t'
#define TREE_SEP		'/'

#define INFO_ERROR_RESPONSE(db, num, message)   \
	do {                                        \
		cf_dyn_buf_append_string(db, "ERROR:"); \
		cf_dyn_buf_append_int(db, num);         \
		cf_dyn_buf_append_string(db, ":");      \
		cf_dyn_buf_append_string(db, message);  \
	} while (false)

// Only for sindex-related legacy!
#define INFO_FAIL_RESPONSE(db, num, message)    \
	do {                                        \
		cf_dyn_buf_append_string(db, "FAIL:");  \
		cf_dyn_buf_append_int(db, num);         \
		cf_dyn_buf_append_string(db, ":");      \
		cf_dyn_buf_append_string(db, message);  \
	} while (false)


void
info_get_aggregated_namespace_stats(cf_dyn_buf *db)
{
	uint64_t total_objects = 0;
	uint64_t total_tombstones = 0;

	for (uint32_t i = 0; i < g_config.n_namespaces; i++) {
		as_namespace *ns = g_config.namespaces[i];

		total_objects += ns->n_objects;
		total_tombstones += ns->n_tombstones;
	}

	info_append_uint64(db, "objects", total_objects);
	info_append_uint64(db, "tombstones", total_tombstones);
}

// TODO: This function should move elsewhere.
static inline uint64_t
get_cpu_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
	return (ts.tv_sec * 1000 * 1000 * 1000) + ts.tv_nsec;
}

// TODO: This should move elsewhere.
static uint32_t g_process_cpu_pct = 0;

// TODO: This function should move elsewhere.
// Called only from the ticker thread.
uint32_t
process_cpu(void)
{
	static uint64_t prev = 0;
	static uint64_t prev_cpu = 0;

	uint64_t now = cf_getns();
	uint64_t now_cpu = get_cpu_ns();

	if (prev != 0) {
		uint64_t delta = now - prev;
		uint64_t delta_cpu = now_cpu - prev_cpu;

		g_process_cpu_pct = (uint32_t)(delta_cpu * 100 / delta);
	}

	prev = now;
	prev_cpu = now_cpu;

	return g_process_cpu_pct;
}

// TODO: This should move elsewhere.
static uint32_t g_user_cpu_pct = 0;
static uint32_t g_kernel_cpu_pct = 0;

// TODO: This function should move elsewhere.
// Called only from the ticker thread.
void
sys_cpu_info(uint32_t* user_pct, uint32_t* kernel_pct)
{
	if (user_pct != NULL) {
		*user_pct = g_user_cpu_pct;
	}

	if (kernel_pct != NULL) {
		*kernel_pct = g_kernel_cpu_pct;
	}

	FILE* fh = fopen("/proc/stat", "r");

	if (fh == NULL) {
		cf_warning(AS_INFO, "failed to open /proc/stat: %d", errno);
		return;
	}

	uint64_t user;
	uint64_t nice;
	uint64_t kernel;
	uint64_t idle;

	if (fscanf(fh, "cpu %lu %lu %lu %lu", &user, &nice, &kernel, &idle) != 4) {
		cf_warning(AS_INFO, "can't parse /proc/stat");
		fclose(fh);
		return;
	}

	fclose(fh);

	static uint64_t prev_user = 0;
	static uint64_t prev_nice = 0;
	static uint64_t prev_kernel = 0;
	static uint64_t prev_idle = 0;

	if (prev_user != 0) {
		uint32_t delta_user = (uint32_t)(user - prev_user);
		uint32_t delta_nice = (uint32_t)(nice - prev_nice);
		uint32_t delta_kernel = (uint32_t)(kernel - prev_kernel);
		uint32_t delta_idle = (uint32_t)(idle - prev_idle);

		uint32_t total = delta_user + delta_nice + delta_kernel + delta_idle;
		uint32_t n_cpus = cf_topo_count_cpus();

		g_user_cpu_pct = (delta_user + delta_nice) * 100 * n_cpus / total;
		g_kernel_cpu_pct = delta_kernel * 100 * n_cpus / total;
	}

	prev_user = user;
	prev_nice = nice;
	prev_kernel = kernel;
	prev_idle = idle;

	if (user_pct != NULL) {
		*user_pct = g_user_cpu_pct;
	}

	if (kernel_pct != NULL) {
		*kernel_pct = g_kernel_cpu_pct;
	}
}

// TODO: This function should move elsewhere.
void
sys_mem_info(uint64_t* free_mem_kbytes, uint32_t* free_mem_pct,
		uint64_t* thp_mem_kbytes)
{
	*free_mem_kbytes = 0;
	*free_mem_pct = 0;
	*thp_mem_kbytes = 0;

	int32_t fd = open("/proc/meminfo", O_RDONLY);

	if (fd < 0) {
		cf_warning(AS_INFO, "failed to open /proc/meminfo: %d", errno);
		return;
	}

	char buf[4096] = { 0 };
	size_t limit = sizeof(buf);
	size_t total = 0;

	while (total < limit) {
		ssize_t len = read(fd, buf + total, limit - total);

		if (len < 0) {
			cf_warning(AS_INFO, "couldn't read /proc/meminfo: %d", errno);
			close(fd);
			return;
		}

		if (len == 0) {
			break; // EOF
		}

		total += (size_t)len;
	}

	close(fd);

	if (total == limit) {
		cf_warning(AS_INFO, "/proc/meminfo exceeds %zu bytes", limit);
		return;
	}

	uint64_t mem_total = 0;
	uint64_t active = 0;
	uint64_t inactive = 0;
	uint64_t cached = 0;
	uint64_t buffers = 0;
	uint64_t shmem = 0;
	uint64_t anon_huge_pages = 0;

	char* cur = buf;
	char* save_ptr = NULL;

	// We split each line into two fields separated by ':'. strtoul() will
	// safely ignore the spaces and 'kB' (if present).
	while (true) {
		char* name_tok = strtok_r(cur, ":", &save_ptr);

		if (name_tok == NULL) {
			break; // no more lines
		}

		cur = NULL; // all except first name_tok use NULL

		char* value_tok = strtok_r(NULL, "\r\n", &save_ptr);

		if (value_tok == NULL) {
			cf_warning(AS_INFO, "/proc/meminfo line missing value token");
			return;
		}

		if (strcmp(name_tok, "MemTotal") == 0) {
			mem_total = strtoul(value_tok, NULL, 0);
		}
		else if (strcmp(name_tok, "Active") == 0) {
			active = strtoul(value_tok, NULL, 0);
		}
		else if (strcmp(name_tok, "Inactive") == 0) {
			inactive = strtoul(value_tok, NULL, 0);
		}
		else if (strcmp(name_tok, "Cached") == 0) {
			cached = strtoul(value_tok, NULL, 0);
		}
		else if (strcmp(name_tok, "Buffers") == 0) {
			buffers = strtoul(value_tok, NULL, 0);
		}
		else if (strcmp(name_tok, "Shmem") == 0) {
			shmem = strtoul(value_tok, NULL, 0);
		}
		else if (strcmp(name_tok, "AnonHugePages") == 0) {
			anon_huge_pages = strtoul(value_tok, NULL, 0);
		}
	}

	// Add the cached memory and buffers, which are effectively available if and
	// when needed. Caution: subtract the shared memory, which is included in
	// the cached memory, but is not available.
	uint64_t avail = mem_total - active - inactive + cached + buffers - shmem;

	*free_mem_kbytes = avail;
	*free_mem_pct = mem_total == 0 ? 0 : (avail * 100) / mem_total;
	*thp_mem_kbytes = anon_huge_pages;
}


int
info_get_stats(char *name, cf_dyn_buf *db)
{
	uint64_t now_sec = cf_get_seconds();

	info_append_bool(db, "failed_best_practices", g_bad_practices.used_sz != 0);

	as_exchange_cluster_info(db);
	info_append_uint32(db, "cluster_min_compatibility_id", as_exchange_min_compatibility_id()); // not in ticker
	info_append_uint32(db, "cluster_max_compatibility_id", as_exchange_max_compatibility_id()); // not in ticker
	info_append_bool(db, "cluster_integrity", as_clustering_has_integrity()); // not in ticker
	info_append_bool(db, "cluster_is_member", ! as_clustering_is_orphan()); // not in ticker
	as_hb_info_duplicates_get(db); // not in ticker
	info_append_uint32(db, "cluster_clock_skew_stop_writes_sec", clock_skew_stop_writes_sec()); // not in ticker
	info_append_uint64(db, "cluster_clock_skew_ms", as_skew_monitor_skew());
	as_skew_monitor_info(db);

	info_append_uint64(db, "uptime", now_sec - g_start_sec); // not in ticker

	uint32_t user_pct = as_load_uint32(&g_user_cpu_pct);
	uint32_t kernel_pct = as_load_uint32(&g_kernel_cpu_pct);

	info_append_uint32(db, "system_total_cpu_pct", user_pct + kernel_pct);
	info_append_uint32(db, "system_user_cpu_pct", user_pct);
	info_append_uint32(db, "system_kernel_cpu_pct", kernel_pct);

	uint64_t free_mem_kbytes;
	uint32_t free_mem_pct;
	uint64_t thp_mem_kbytes;

	sys_mem_info(&free_mem_kbytes, &free_mem_pct, &thp_mem_kbytes);
	info_append_uint64(db, "system_free_mem_kbytes", free_mem_kbytes);
	info_append_int(db, "system_free_mem_pct", free_mem_pct);
	info_append_uint64(db, "system_thp_mem_kbytes", thp_mem_kbytes);

	info_append_uint32(db, "process_cpu_pct", g_process_cpu_pct);

	cf_thread_stats ts;

	cf_thread_get_stats(&ts);
	info_append_uint32(db, "threads_joinable", ts.n_joinable);
	info_append_uint32(db, "threads_detached", ts.n_detached);
	info_append_uint32(db, "threads_pool_total", ts.n_pool_total);
	info_append_uint32(db, "threads_pool_active", ts.n_pool_active);

	size_t allocated_kbytes;
	size_t active_kbytes;
	size_t mapped_kbytes;
	double efficiency_pct;
	uint32_t site_count;

	cf_alloc_heap_stats(&allocated_kbytes, &active_kbytes, &mapped_kbytes, &efficiency_pct,
			&site_count);
	info_append_uint64(db, "heap_allocated_kbytes", allocated_kbytes);
	info_append_uint64(db, "heap_active_kbytes", active_kbytes);
	info_append_uint64(db, "heap_mapped_kbytes", mapped_kbytes);
	info_append_int(db, "heap_efficiency_pct", (int)(efficiency_pct + 0.5));
	info_append_uint32(db, "heap_site_count", site_count);

	info_get_aggregated_namespace_stats(db);

	info_append_uint32(db, "info_queue", as_info_queue_get_size());
	info_append_uint32(db, "rw_in_progress", rw_request_hash_count());
	info_append_uint32(db, "proxy_in_progress", as_proxy_hash_count());
	info_append_uint32(db, "tree_gc_queue", as_index_tree_gc_queue_size());

	// Read closed before opened.
	uint64_t n_proto_fds_closed = g_stats.proto_connections_closed;
	uint64_t n_hb_fds_closed = g_stats.heartbeat_connections_closed;
	uint64_t n_fabric_fds_closed = g_stats.fabric_connections_closed;
	// TODO - non-86 memory barrier.
	uint64_t n_proto_fds_opened = g_stats.proto_connections_opened;
	uint64_t n_hb_fds_opened = g_stats.heartbeat_connections_opened;
	uint64_t n_fabric_fds_opened = g_stats.fabric_connections_opened;

	uint64_t n_proto_fds_open = n_proto_fds_opened - n_proto_fds_closed;
	uint64_t n_hb_fds_open = n_hb_fds_opened - n_hb_fds_closed;
	uint64_t n_fabric_fds_open = n_fabric_fds_opened - n_fabric_fds_closed;

	info_append_uint64(db, "client_connections", n_proto_fds_open);
	info_append_uint64(db, "client_connections_opened", n_proto_fds_opened);
	info_append_uint64(db, "client_connections_closed", n_proto_fds_closed);
	info_append_uint64(db, "heartbeat_connections", n_hb_fds_open);
	info_append_uint64(db, "heartbeat_connections_opened", n_hb_fds_opened);
	info_append_uint64(db, "heartbeat_connections_closed", n_hb_fds_closed);
	info_append_uint64(db, "fabric_connections", n_fabric_fds_open);
	info_append_uint64(db, "fabric_connections_opened", n_fabric_fds_opened);
	info_append_uint64(db, "fabric_connections_closed", n_fabric_fds_closed);

	info_append_uint64(db, "heartbeat_received_self", g_stats.heartbeat_received_self);
	info_append_uint64(db, "heartbeat_received_foreign", g_stats.heartbeat_received_foreign);

	info_append_uint64(db, "reaped_fds", g_stats.reaper_count); // not in ticker

	info_append_uint64(db, "info_complete", g_stats.info_complete); // not in ticker

	info_append_uint64(db, "demarshal_error", g_stats.n_demarshal_error);
	info_append_uint64(db, "early_tsvc_client_error", g_stats.n_tsvc_client_error);
	info_append_uint64(db, "early_tsvc_from_proxy_error", g_stats.n_tsvc_from_proxy_error);
	info_append_uint64(db, "early_tsvc_batch_sub_error", g_stats.n_tsvc_batch_sub_error);
	info_append_uint64(db, "early_tsvc_from_proxy_batch_sub_error", g_stats.n_tsvc_from_proxy_batch_sub_error);
	info_append_uint64(db, "early_tsvc_udf_sub_error", g_stats.n_tsvc_udf_sub_error);
	info_append_uint64(db, "early_tsvc_ops_sub_error", g_stats.n_tsvc_ops_sub_error);

	info_append_uint32(db, "long_queries_active", as_query_get_active_job_count());

	info_append_uint64(db, "batch_index_initiate", g_stats.batch_index_initiate); // not in ticker

	cf_dyn_buf_append_string(db, "batch_index_queue=");
	as_batch_queues_info(db); // not in ticker
	cf_dyn_buf_append_char(db, ';');

	info_append_uint64(db, "batch_index_complete", g_stats.batch_index_complete);
	info_append_uint64(db, "batch_index_error", g_stats.batch_index_errors);
	info_append_uint64(db, "batch_index_timeout", g_stats.batch_index_timeout);
	info_append_uint64(db, "batch_index_delay", g_stats.batch_index_delay);

	// Everything below is not in ticker...

	info_append_uint32(db, "batch_index_unused_buffers", as_batch_unused_buffers());
	info_append_uint64(db, "batch_index_huge_buffers", g_stats.batch_index_huge_buffers);
	info_append_uint64(db, "batch_index_created_buffers", g_stats.batch_index_created_buffers);
	info_append_uint64(db, "batch_index_destroyed_buffers", g_stats.batch_index_destroyed_buffers);

	double batch_orig_sz = as_load_double(&g_stats.batch_comp_stat.avg_orig_sz);
	double batch_ratio = batch_orig_sz > 0.0 ? g_stats.batch_comp_stat.avg_comp_sz / batch_orig_sz : 1.0;

	info_append_format(db, "batch_index_proto_uncompressed_pct", "%.3f", g_stats.batch_comp_stat.uncomp_pct);
	info_append_format(db, "batch_index_proto_compression_ratio", "%.3f", batch_ratio);

	char paxos_principal[16 + 1];
	sprintf(paxos_principal, "%lX", as_exchange_principal());
	info_append_string(db, "paxos_principal", paxos_principal);

	info_append_uint64(db, "time_since_rebalance", now_sec - g_rebalance_sec);

	info_append_bool(db, "migrate_allowed", as_partition_balance_are_migrations_allowed());
	info_append_uint64(db, "migrate_partitions_remaining", as_partition_balance_remaining_migrations());

	info_append_uint64(db, "fabric_bulk_send_rate", g_stats.fabric_bulk_s_rate);
	info_append_uint64(db, "fabric_bulk_recv_rate", g_stats.fabric_bulk_r_rate);
	info_append_uint64(db, "fabric_ctrl_send_rate", g_stats.fabric_ctrl_s_rate);
	info_append_uint64(db, "fabric_ctrl_recv_rate", g_stats.fabric_ctrl_r_rate);
	info_append_uint64(db, "fabric_meta_send_rate", g_stats.fabric_meta_s_rate);
	info_append_uint64(db, "fabric_meta_recv_rate", g_stats.fabric_meta_r_rate);
	info_append_uint64(db, "fabric_rw_send_rate", g_stats.fabric_rw_s_rate);
	info_append_uint64(db, "fabric_rw_recv_rate", g_stats.fabric_rw_r_rate);

	cf_dyn_buf_chomp(db);

	return 0;
}

int
info_get_best_practices(char *name, cf_dyn_buf *db)
{
	cf_dyn_buf_append_string(db, "failed_best_practices=");

	if (g_bad_practices.used_sz == 0) {
		cf_dyn_buf_append_string(db, "none");
	}
	else {
		cf_dyn_buf_append_buf(db, g_bad_practices.buf, g_bad_practices.used_sz);
	}

	return 0;
}

void
info_get_printable_cluster_name(char *cluster_name)
{
	as_config_cluster_name_get(cluster_name);
	if (cluster_name[0] == '\0'){
		strcpy(cluster_name, "null");
	}
}

int
info_get_cluster_name(char *name, cf_dyn_buf *db)
{
	char cluster_name[AS_CLUSTER_NAME_SZ];
	info_get_printable_cluster_name(cluster_name);
	cf_dyn_buf_append_string(db, cluster_name);

	return 0;
}

int
info_get_features(char *name, cf_dyn_buf *db)
{
	cf_dyn_buf_append_string(db, as_features_info());

	return 0;
}

static cf_ip_port
bind_to_port(cf_serv_cfg *cfg, cf_sock_owner owner)
{
	for (uint32_t i = 0; i < cfg->n_cfgs; ++i) {
		if (cfg->cfgs[i].owner == owner) {
			return cfg->cfgs[i].port;
		}
	}

	return 0;
}

char *
as_info_bind_to_string(const cf_serv_cfg *cfg, cf_sock_owner owner)
{
	cf_dyn_buf_define_size(db, 2500);
	uint32_t count = 0;

	for (uint32_t i = 0; i < cfg->n_cfgs; ++i) {
		if (cfg->cfgs[i].owner != owner) {
			continue;
		}

		if (count > 0) {
			cf_dyn_buf_append_char(&db, ',');
		}

		cf_dyn_buf_append_string(&db, cf_ip_addr_print(&cfg->cfgs[i].addr));
		++count;
	}

	char *string = cf_dyn_buf_strdup(&db);
	cf_dyn_buf_free(&db);
	return string != NULL ? string : cf_strdup("null");
}

static char *
access_to_string(cf_addr_list *addrs)
{
	cf_dyn_buf_define_size(db, 2500);

	for (uint32_t i = 0; i < addrs->n_addrs; ++i) {
		if (i > 0) {
			cf_dyn_buf_append_char(&db, ',');
		}

		cf_dyn_buf_append_string(&db, addrs->addrs[i]);
	}

	char *string = cf_dyn_buf_strdup(&db);
	cf_dyn_buf_free(&db);
	return string != NULL ? string : cf_strdup("null");
}

int
info_get_endpoints(char *name, cf_dyn_buf *db)
{
	cf_ip_port port = bind_to_port(&g_service_bind, CF_SOCK_OWNER_SERVICE);
	info_append_int(db, "service.port", port);

	char *string = as_info_bind_to_string(&g_service_bind, CF_SOCK_OWNER_SERVICE);
	info_append_string(db, "service.addresses", string);
	cf_free(string);

	info_append_int(db, "service.access-port", g_access.service.port);

	string = access_to_string(&g_access.service.addrs);
	info_append_string(db, "service.access-addresses", string);
	cf_free(string);

	info_append_int(db, "service.alternate-access-port", g_access.alt_service.port);

	string = access_to_string(&g_access.alt_service.addrs);
	info_append_string(db, "service.alternate-access-addresses", string);
	cf_free(string);

	port = bind_to_port(&g_service_bind, CF_SOCK_OWNER_SERVICE_TLS);
	info_append_int(db, "service.tls-port", port);

	string = as_info_bind_to_string(&g_service_bind, CF_SOCK_OWNER_SERVICE_TLS);
	info_append_string(db, "service.tls-addresses", string);
	cf_free(string);

	info_append_int(db, "service.tls-access-port", g_access.tls_service.port);

	string = access_to_string(&g_access.tls_service.addrs);
	info_append_string(db, "service.tls-access-addresses", string);
	cf_free(string);

	info_append_int(db, "service.tls-alternate-access-port", g_access.alt_tls_service.port);

	string = access_to_string(&g_access.alt_tls_service.addrs);
	info_append_string(db, "service.tls-alternate-access-addresses", string);
	cf_free(string);

	as_hb_info_endpoints_get(db);

	port = bind_to_port(&g_fabric_bind, CF_SOCK_OWNER_FABRIC);
	info_append_int(db, "fabric.port", port);

	string = as_info_bind_to_string(&g_fabric_bind, CF_SOCK_OWNER_FABRIC);
	info_append_string(db, "fabric.addresses", string);
	cf_free(string);

	port = bind_to_port(&g_fabric_bind, CF_SOCK_OWNER_FABRIC_TLS);
	info_append_int(db, "fabric.tls-port", port);

	string = as_info_bind_to_string(&g_fabric_bind, CF_SOCK_OWNER_FABRIC_TLS);
	info_append_string(db, "fabric.tls-addresses", string);
	cf_free(string);

	as_fabric_info_peer_endpoints_get(db);

	info_append_int(db, "info.port", g_info_port);

	string = as_info_bind_to_string(&g_info_bind, CF_SOCK_OWNER_INFO);
	info_append_string(db, "info.addresses", string);
	cf_free(string);

	cf_dyn_buf_chomp(db);
	return(0);
}

int
info_get_partition_generation(char *name, cf_dyn_buf *db)
{
	cf_dyn_buf_append_int(db, (int)g_partition_generation);

	return(0);
}

int
info_get_partition_info(char *name, cf_dyn_buf *db)
{
	as_partition_getinfo_str(db);

	return(0);
}

int
info_get_rack_ids(char *name, cf_dyn_buf *db)
{
	if (as_info_error_enterprise_only()) {
		cf_dyn_buf_append_string(db, "ERROR::enterprise-only");
		return 0;
	}

	as_partition_balance_effective_rack_ids(db);

	return 0;
}

int
info_get_rebalance_generation(char *name, cf_dyn_buf *db)
{
	cf_dyn_buf_append_uint64(db, g_rebalance_generation);

	return 0;
}

int
info_get_replicas_master(char *name, cf_dyn_buf *db)
{
	as_partition_get_replicas_master_str(db);

	return(0);
}

int
info_get_replicas_all(char *name, cf_dyn_buf *db)
{
	as_partition_get_replicas_all_str(db, false, 0);

	return(0);
}

int
info_get_replicas(char *name, cf_dyn_buf *db)
{
	as_partition_get_replicas_all_str(db, true, 0);

	return(0);
}

//
// COMMANDS
//

int
info_command_replicas(char *name, char *params, cf_dyn_buf *db)
{
	char max_str[4] = { 0 };
	int len = (int)sizeof(max_str);
	int rv = as_info_parameter_get(params, "max", max_str, &len);

	if (rv == -2) {
		cf_warning(AS_INFO, "max parameter value too long");
		cf_dyn_buf_append_string(db, "ERROR::bad-max");
		return 0;
	}

	uint32_t max_repls = 0;

	if (rv == 0 && cf_str_atoi_u32(max_str, &max_repls) != 0) {
		cf_warning(AS_INFO, "non-integer max parameter");
		cf_dyn_buf_append_string(db, "ERROR::bad-max");
		return 0;
	}

	as_partition_get_replicas_all_str(db, true, max_repls);

	return 0;
}

int
info_command_cluster_stable(char *name, char *params, cf_dyn_buf *db)
{
	// Command format:
	// "cluster-stable:[size=<target-size>];[ignore-migrations=<bool>];[namespace=<namespace-name>]"

	uint64_t begin_cluster_key = as_exchange_cluster_key();

	if (! as_partition_balance_are_migrations_allowed()) {
		cf_dyn_buf_append_string(db, "ERROR::unstable-cluster");
		return 0;
	}

	char size_str[4] = { 0 }; // max cluster size is 256
	int size_str_len = (int)sizeof(size_str);
	int rv = as_info_parameter_get(params, "size", size_str, &size_str_len);

	if (rv == -2) {
		cf_warning(AS_INFO, "size parameter value too long");
		cf_dyn_buf_append_string(db, "ERROR::bad-size");
		return 0;
	}

	if (rv == 0) {
		uint32_t target_size;

		if (cf_str_atoi_u32(size_str, &target_size) != 0) {
			cf_warning(AS_INFO, "non-integer size parameter");
			cf_dyn_buf_append_string(db, "ERROR::bad-size");
			return 0;
		}

		if (target_size != as_exchange_cluster_size()) {
			cf_dyn_buf_append_string(db, "ERROR::cluster-not-specified-size");
			return 0;
		}
	}

	bool ignore_migrations = false;

	char ignore_migrations_str[6] = { 0 };
	int ignore_migrations_str_len = (int)sizeof(ignore_migrations_str);

	rv = as_info_parameter_get(params, "ignore-migrations",
			ignore_migrations_str, &ignore_migrations_str_len);

	if (rv == -2) {
		cf_warning(AS_INFO, "ignore-migrations value too long");
		cf_dyn_buf_append_string(db, "ERROR::bad-ignore-migrations");
		return 0;
	}

	if (rv == 0) {
		if (strcmp(ignore_migrations_str, "true") == 0 ||
				strcmp(ignore_migrations_str, "yes") == 0) {
			ignore_migrations = true;
		}
		else if (strcmp(ignore_migrations_str, "false") == 0 ||
				strcmp(ignore_migrations_str, "no") == 0) {
			ignore_migrations = false;
		}
		else {
			cf_warning(AS_INFO, "ignore-migrations value invalid");
			cf_dyn_buf_append_string(db, "ERROR::bad-ignore-migrations");
			return 0;
		}
	}

	if (! ignore_migrations) {
		char ns_name[AS_ID_NAMESPACE_SZ] = { 0 };
		int ns_name_len = (int)sizeof(ns_name);

		rv = as_info_parameter_get(params, "namespace", ns_name, &ns_name_len);

		if (rv == -2) {
			cf_warning(AS_INFO, "namespace parameter value too long");
			cf_dyn_buf_append_string(db, "ERROR::bad-namespace");
			return 0;
		}

		if (rv == -1) {
			// Ensure migrations are complete for all namespaces.

			if (as_partition_balance_remaining_migrations() != 0) {
				cf_dyn_buf_append_string(db, "ERROR::unstable-cluster");
				return 0;
			}
		}
		else {
			// Ensure migrations are complete for the requested namespace only.
			as_namespace *ns = as_namespace_get_byname(ns_name);

			if (! ns) {
				cf_warning(AS_INFO, "unknown namespace %s", ns_name);
				cf_dyn_buf_append_string(db, "ERROR::unknown-namespace");
				return 0;
			}

			if (ns->migrate_tx_partitions_remaining +
					ns->migrate_rx_partitions_remaining +
					ns->n_unavailable_partitions +
					ns->n_dead_partitions != 0) {
				cf_dyn_buf_append_string(db, "ERROR::unstable-cluster");
				return 0;
			}
		}
	}

	if (begin_cluster_key != as_exchange_cluster_key()) {
		// Verify that the cluster didn't change while during the collection.
		cf_dyn_buf_append_string(db, "ERROR::unstable-cluster");
	}

	cf_dyn_buf_append_uint64_x(db, begin_cluster_key);

	return 0;
}

int
info_command_get_sl(char *name, char *params, cf_dyn_buf *db)
{
	// Command Format:  "get-sl:"

	as_exchange_info_get_succession(db);

	return 0;
}

int
info_command_tip(char *name, char *params, cf_dyn_buf *db)
{
	cf_debug(AS_INFO, "tip command received: params %s", params);

	char host_str[DNS_NAME_MAX_SIZE];
	int  host_str_len = sizeof(host_str);

	char port_str[50];
	int  port_str_len = sizeof(port_str);
	int rv = -1;

	char tls_str[50];
	int  tls_str_len = sizeof(tls_str);

	/*
	 *  Command Format:  "tip:host=<IPAddr>;port=<PortNum>[;tls=<Bool>]"
	 *
	 *  where <IPAddr> is an IP address and <PortNum> is a valid TCP port number.
	 */

	if (0 != as_info_parameter_get(params, "host", host_str, &host_str_len)) {
		cf_warning(AS_INFO, "tip command: no host, must add a host parameter - maximum %d characters", DNS_NAME_MAX_LEN);
		goto Exit;
	}

	if (0 != as_info_parameter_get(params, "port", port_str, &port_str_len)) {
		cf_warning(AS_INFO, "tip command: no port, must have port");
		goto Exit;
	}

	if (0 != as_info_parameter_get(params, "tls", tls_str, &tls_str_len)) {
		strcpy(tls_str, "false");
	}

	int port = 0;
	if (0 != cf_str_atoi(port_str, &port)) {
		cf_warning(AS_INFO, "tip command: port must be an integer in: %s", port_str);
		goto Exit;
	}

	bool tls;
	if (strcmp(tls_str, "true") == 0) {
		tls = true;
	}
	else if (strcmp(tls_str, "false") == 0) {
		tls = false;
	}
	else {
		cf_warning(AS_INFO, "The \"%s:\" command argument \"tls\" value must be one of {\"true\", \"false\"}, not \"%s\"", name, tls_str);
		goto Exit;
	}

	rv = as_hb_mesh_tip(host_str, port, tls);

Exit:
	if (0 == rv) {
		cf_dyn_buf_append_string(db, "ok");
	} else {
		cf_dyn_buf_append_string(db, "error");
	}

	return(0);
}

/*
 *  Command Format:  "tip-clear:{host-port-list=<hpl>}"
 *
 *  where <hpl> is either "all" or else a comma-separated list of items of the form: <HostIPAddr>:<PortNum>
 */
int32_t
info_command_tip_clear(char* name, char* params, cf_dyn_buf* db)
{
	cf_info(AS_INFO, "tip clear command received: params %s", params);

	// Command Format:  "tip-clear:{host-port-list=<hpl>}" [the
	// "host-port-list" argument is optional]
	// where <hpl> is either "all" or else a comma-separated list of items
	// of the form: <HostIPv4Addr>:<PortNum> or [<HostIPv6Addr>]:<PortNum>

	char host_port_list[3000];
	int host_port_list_len = sizeof(host_port_list);
	host_port_list[0] = '\0';
	bool success = true;
	uint32_t cleared = 0, not_found = 0;

	if (as_info_parameter_get(params, "host-port-list", host_port_list,
				  &host_port_list_len) == 0) {
		char* save_ptr = NULL;
		int port = -1;
		char* host_port = strtok_r(host_port_list, ",", &save_ptr);

		while (host_port != NULL) {
			char* host_port_delim = ":";
			if (*host_port == '[') {
				// Parse IPv6 address differently.
				host_port++;
				host_port_delim = "]";
			}

			char* host_port_save_ptr = NULL;
			char* host =
					strtok_r(host_port, host_port_delim, &host_port_save_ptr);

			if (host == NULL) {
				cf_warning(AS_INFO, "tip clear command: invalid host:port string: %s", host_port);
				success = false;
				break;
			}

			char* port_str =
					strtok_r(NULL, host_port_delim, &host_port_save_ptr);

			if (port_str != NULL && *port_str == ':') {
				// IPv6 case
				port_str++;
			}
			if (port_str == NULL ||
					0 != cf_str_atoi(port_str, &port)) {
				cf_warning(AS_INFO, "tip clear command: port must be an integer in: %s", port_str);
				success = false;
				break;
			}

			if (as_hb_mesh_tip_clear(host, port) == -1) {
				success = false;
				not_found++;
				cf_warning(AS_INFO, "seed node %s:%d does not exist", host, port);
			} else {
				cleared++;
			}

			host_port = strtok_r(NULL, ",", &save_ptr);
		}
	} else {
		success = false;
	}

	if (success) {
		cf_info(AS_INFO, "tip clear command executed: cleared %"PRIu32", params %s", cleared, params);
		cf_dyn_buf_append_string(db, "ok");
	} else {
		cf_info(AS_INFO, "tip clear command failed: cleared %"PRIu32", params %s", cleared, params);
		char error_msg[1024];
		sprintf(error_msg, "error: %"PRIu32" cleared, %"PRIu32" not found", cleared, not_found);
		cf_dyn_buf_append_string(db, error_msg);
	}

	return (0);
}

int
info_command_dump_cluster(char *name, char *params, cf_dyn_buf *db)
{
	bool verbose = false;
	char param_str[100];
	int param_str_len = sizeof(param_str);

	/*
	 *  Command Format:  "dump-cluster:{verbose=<opt>}" [the "verbose" argument is optional]
	 *
	 *  where <opt> is one of:  {"true" | "false"} and defaults to "false".
	 */
	param_str[0] = '\0';
	if (!as_info_parameter_get(params, "verbose", param_str, &param_str_len)) {
		if (!strncmp(param_str, "true", 5)) {
			verbose = true;
		} else if (!strncmp(param_str, "false", 6)) {
			verbose = false;
		} else {
			cf_warning(AS_INFO, "The \"%s:\" command argument \"verbose\" value must be one of {\"true\", \"false\"}, not \"%s\"", name, param_str);
			cf_dyn_buf_append_string(db, "error");
			return 0;
		}
	}
	as_clustering_dump(verbose);
	as_exchange_dump(verbose);
	cf_dyn_buf_append_string(db, "ok");
	return(0);
}

int
info_command_dump_fabric(char *name, char *params, cf_dyn_buf *db)
{
	bool verbose = false;
	char param_str[100];
	int param_str_len = sizeof(param_str);

	/*
	 *  Command Format:  "dump-fabric:{verbose=<opt>}" [the "verbose" argument is optional]
	 *
	 *  where <opt> is one of:  {"true" | "false"} and defaults to "false".
	 */
	param_str[0] = '\0';
	if (!as_info_parameter_get(params, "verbose", param_str, &param_str_len)) {
		if (!strncmp(param_str, "true", 5)) {
			verbose = true;
		} else if (!strncmp(param_str, "false", 6)) {
			verbose = false;
		} else {
			cf_warning(AS_INFO, "The \"%s:\" command argument \"verbose\" value must be one of {\"true\", \"false\"}, not \"%s\"", name, param_str);
			cf_dyn_buf_append_string(db, "error");
			return 0;
		}
	}
	as_fabric_dump(verbose);
	cf_dyn_buf_append_string(db, "ok");
	return(0);
}

int
info_command_dump_hb(char *name, char *params, cf_dyn_buf *db)
{
	bool verbose = false;
	char param_str[100];
	int param_str_len = sizeof(param_str);

	/*
	 *  Command Format:  "dump-hb:{verbose=<opt>}" [the "verbose" argument is optional]
	 *
	 *  where <opt> is one of:  {"true" | "false"} and defaults to "false".
	 */
	param_str[0] = '\0';
	if (!as_info_parameter_get(params, "verbose", param_str, &param_str_len)) {
		if (!strncmp(param_str, "true", 5)) {
			verbose = true;
		} else if (!strncmp(param_str, "false", 6)) {
			verbose = false;
		} else {
			cf_warning(AS_INFO, "The \"%s:\" command argument \"verbose\" value must be one of {\"true\", \"false\"}, not \"%s\"", name, param_str);
			cf_dyn_buf_append_string(db, "error");
			return 0;
		}
	}
	as_hb_dump(verbose);
	cf_dyn_buf_append_string(db, "ok");
	return(0);
}

int
info_command_dump_hlc(char *name, char *params, cf_dyn_buf *db)
{
	bool verbose = false;
	char param_str[100];
	int param_str_len = sizeof(param_str);

	/*
	 *  Command Format:  "dump-hlc:{verbose=<opt>}" [the "verbose" argument is optional]
	 *
	 *  where <opt> is one of:  {"true" | "false"} and defaults to "false".
	 */
	param_str[0] = '\0';
	if (!as_info_parameter_get(params, "verbose", param_str, &param_str_len)) {
		if (!strncmp(param_str, "true", 5)) {
			verbose = true;
		} else if (!strncmp(param_str, "false", 6)) {
			verbose = false;
		} else {
			cf_warning(AS_INFO, "The \"%s:\" command argument \"verbose\" value must be one of {\"true\", \"false\"}, not \"%s\"", name, param_str);
			cf_dyn_buf_append_string(db, "error");
			return 0;
		}
	}
	as_hlc_dump(verbose);
	cf_dyn_buf_append_string(db, "ok");
	return(0);
}


int
info_command_dump_migrates(char *name, char *params, cf_dyn_buf *db)
{
	bool verbose = false;
	char param_str[100];
	int param_str_len = sizeof(param_str);

	/*
	 *  Command Format:  "dump-migrates:{verbose=<opt>}" [the "verbose" argument is optional]
	 *
	 *  where <opt> is one of:  {"true" | "false"} and defaults to "false".
	 */
	param_str[0] = '\0';
	if (!as_info_parameter_get(params, "verbose", param_str, &param_str_len)) {
		if (!strncmp(param_str, "true", 5)) {
			verbose = true;
		} else if (!strncmp(param_str, "false", 6)) {
			verbose = false;
		} else {
			cf_warning(AS_INFO, "The \"%s:\" command argument \"verbose\" value must be one of {\"true\", \"false\"}, not \"%s\"", name, param_str);
			cf_dyn_buf_append_string(db, "error");
			return 0;
		}
	}
	as_migrate_dump(verbose);
	cf_dyn_buf_append_string(db, "ok");
	return(0);
}

int
info_command_dump_wb_summary(char *name, char *params, cf_dyn_buf *db)
{
	as_namespace *ns;
	char param_str[100];
	int param_str_len = sizeof(param_str);

	/*
	 *  Command Format:  "dump-wb-summary:ns=<Namespace>"
	 *
	 *  where <Namespace> is the name of an existing namespace.
	 */
	param_str[0] = '\0';
	if (!as_info_parameter_get(params, "ns", param_str, &param_str_len)) {
		if (!(ns = as_namespace_get_byname(param_str))) {
			cf_warning(AS_INFO, "The \"%s:\" command argument \"ns\" value must be the name of an existing namespace, not \"%s\"", name, param_str);
			cf_dyn_buf_append_string(db, "error");
			return(0);
		}
	} else {
		cf_warning(AS_INFO, "The \"%s:\" command requires an argument of the form \"ns=<Namespace>\"", name);
		cf_dyn_buf_append_string(db, "error");
		return 0;
	}

	as_storage_dump_wb_summary(ns);

	cf_dyn_buf_append_string(db, "ok");

	return(0);
}

int
info_command_dump_rw_request_hash(char *name, char *params, cf_dyn_buf *db)
{
	rw_request_hash_dump();
	cf_dyn_buf_append_string(db, "ok");
	return(0);
}

int
info_command_physical_devices(char *name, char *params, cf_dyn_buf *db)
{
	// Command format: "physical-devices:path=<path>"
	//
	// <path> can specify a device partition, file path, mount directory, etc.
	// ... anything backed by one or more physical devices.

	char path_str[1024] = { 0 };
	int path_str_len = (int)sizeof(path_str);
	int rv = as_info_parameter_get(params, "path", path_str, &path_str_len);

	if (rv == -2) {
		cf_warning(AS_INFO, "path too long");
		cf_dyn_buf_append_string(db, "ERROR::bad-path");
		return 0;
	}

	// For now path is mandatory.
	if (rv == -1) {
		cf_warning(AS_INFO, "path not specified");
		cf_dyn_buf_append_string(db, "ERROR::no-path");
		return 0;
	}

	cf_storage_device_info *device_info = cf_storage_get_device_info(path_str);

	if (device_info == NULL) {
		cf_warning(AS_INFO, "can't get device info for %s", path_str);
		cf_dyn_buf_append_string(db, "ERROR::no-device-info");
		return 0;
	}

	for (uint32_t i = 0; i < device_info->n_phys; i++) {
		cf_dyn_buf_append_string(db, "physical-device=");
		cf_dyn_buf_append_string(db, device_info->phys[i].dev_path);
		cf_dyn_buf_append_char(db, ':');
		cf_dyn_buf_append_string(db, "age=");
		cf_dyn_buf_append_int(db, device_info->phys[i].nvme_age);

		cf_dyn_buf_append_char(db, ';');
	}

	cf_dyn_buf_chomp(db);

	return 0;
}

int
info_command_quiesce(char *name, char *params, cf_dyn_buf *db)
{
	// Command format: "quiesce:"

	if (as_info_error_enterprise_only()) {
		cf_dyn_buf_append_string(db, "ERROR::enterprise-only");
		return 0;
	}

	if (g_config.stay_quiesced) {
		cf_dyn_buf_append_string(db, "ERROR::permanently-quiesced");
		return 0;
	}

	for (uint32_t ns_ix = 0; ns_ix < g_config.n_namespaces; ns_ix++) {
		g_config.namespaces[ns_ix]->pending_quiesce = true;
	}

	cf_dyn_buf_append_string(db, "ok");

	cf_info(AS_INFO, "quiesced this node");

	return 0;
}

int
info_command_quiesce_undo(char *name, char *params, cf_dyn_buf *db)
{
	// Command format: "quiesce-undo:"

	if (as_info_error_enterprise_only()) {
		cf_dyn_buf_append_string(db, "ERROR::enterprise-only");
		return 0;
	}

	if (g_config.stay_quiesced) {
		cf_dyn_buf_append_string(db, "ignored-permanently-quiesced");
		return 0;
	}

	for (uint32_t ns_ix = 0; ns_ix < g_config.n_namespaces; ns_ix++) {
		g_config.namespaces[ns_ix]->pending_quiesce = false;
	}

	cf_dyn_buf_append_string(db, "ok");

	cf_info(AS_INFO, "un-quiesced this node");

	return 0;
}

typedef struct rack_node_s {
	uint32_t rack_id;
	cf_node node;
} rack_node;

// A comparison_fn_t used with qsort() - yields ascending rack-id order.
static inline int
compare_rack_nodes(const void* pa, const void* pb)
{
	uint32_t a = ((const rack_node*)pa)->rack_id;
	uint32_t b = ((const rack_node*)pb)->rack_id;

	return a > b ? 1 : (a == b ? 0 : -1);
}

void
namespace_rack_info(as_namespace *ns, cf_dyn_buf *db, uint32_t *rack_ids,
		uint32_t n_nodes, cf_node node_seq[], const char *tag)
{
	if (n_nodes == 0) {
		return;
	}

	rack_node rack_nodes[n_nodes];

	for (uint32_t n = 0; n < n_nodes; n++) {
		rack_nodes[n].rack_id = rack_ids[n];
		rack_nodes[n].node = node_seq[n];
	}

	qsort(rack_nodes, n_nodes, sizeof(rack_node), compare_rack_nodes);

	uint32_t cur_id = rack_nodes[0].rack_id;

	cf_dyn_buf_append_string(db, tag);
	cf_dyn_buf_append_uint32(db, cur_id);
	cf_dyn_buf_append_char(db, '=');
	cf_dyn_buf_append_uint64_x(db, rack_nodes[0].node);

	for (uint32_t n = 1; n < n_nodes; n++) {
		if (rack_nodes[n].rack_id == cur_id) {
			cf_dyn_buf_append_char(db, ',');
			cf_dyn_buf_append_uint64_x(db, rack_nodes[n].node);
			continue;
		}

		cur_id = rack_nodes[n].rack_id;

		cf_dyn_buf_append_char(db, ':');
		cf_dyn_buf_append_string(db, tag);
		cf_dyn_buf_append_uint32(db, cur_id);
		cf_dyn_buf_append_char(db, '=');
		cf_dyn_buf_append_uint64_x(db, rack_nodes[n].node);
	}
}

int
info_command_racks(char *name, char *params, cf_dyn_buf *db)
{
	// Command format: "racks:{namespace=<namespace-name>}"

	if (as_info_error_enterprise_only()) {
		cf_dyn_buf_append_string(db, "ERROR::enterprise-only");
		return 0;
	}

	char param_str[AS_ID_NAMESPACE_SZ] = { 0 };
	int param_str_len = (int)sizeof(param_str);
	int rv = as_info_parameter_get(params, "namespace", param_str,
			&param_str_len);

	if (rv == -2) {
		cf_warning(AS_INFO, "namespace parameter value too long");
		cf_dyn_buf_append_string(db, "ERROR::bad-namespace");
		return 0;
	}

	if (rv == 0) {
		as_namespace *ns = as_namespace_get_byname(param_str);

		if (! ns) {
			cf_warning(AS_INFO, "unknown namespace %s", param_str);
			cf_dyn_buf_append_string(db, "ERROR::unknown-namespace");
			return 0;
		}

		as_exchange_info_lock();

		namespace_rack_info(ns, db, ns->rack_ids, ns->cluster_size,
				ns->succession, "rack_");

		if (ns->roster_count != 0) {
			cf_dyn_buf_append_char(db, ':');
			namespace_rack_info(ns, db, ns->roster_rack_ids, ns->roster_count,
					ns->roster, "roster_rack_");
		}

		as_exchange_info_unlock();

		return 0;
	}

	for (uint32_t ns_ix = 0; ns_ix < g_config.n_namespaces; ns_ix++) {
		as_namespace *ns = g_config.namespaces[ns_ix];

		cf_dyn_buf_append_string(db, "ns=");
		cf_dyn_buf_append_string(db, ns->name);
		cf_dyn_buf_append_char(db, ':');

		as_exchange_info_lock();

		namespace_rack_info(ns, db, ns->rack_ids, ns->cluster_size,
				ns->succession, "rack_");

		if (ns->roster_count != 0) {
			cf_dyn_buf_append_char(db, ':');
			namespace_rack_info(ns, db, ns->roster_rack_ids, ns->roster_count,
					ns->roster, "roster_rack_");
		}

		as_exchange_info_unlock();

		cf_dyn_buf_append_char(db, ';');
	}

	cf_dyn_buf_chomp(db);

	return 0;
}

int
info_command_recluster(char *name, char *params, cf_dyn_buf *db)
{
	// Command format: "recluster:"

	int rv = as_clustering_cluster_reform();

	// TODO - resolve error condition further?
	cf_dyn_buf_append_string(db,
			rv == 0 ? "ok" : (rv == 1 ? "ignored-by-non-principal" : "ERROR"));

	return 0;
}

int
info_command_jem_stats(char *name, char *params, cf_dyn_buf *db)
{
	cf_debug(AS_INFO, "jem_stats command received: params %s", params);

	/*
	 *	Command Format:	 "jem-stats:{file=<string>;options=<string>;sites=<string>}" [the "file", "options", and "sites" arguments are optional]
	 *
	 *  Logs the JEMalloc statistics to the console or an optionally-specified file pathname.
	 *  Options may be a string containing any of the characters "gmablh", as defined by jemalloc(3) man page.
	 *  The "sites" parameter optionally specifies a file to dump memory accounting information to.
	 *  [Note:  Any options are only used if an output file is specified.]
	 */

	char param_str[100];
	int param_str_len = sizeof(param_str);
	char *file = NULL, *options = NULL, *sites = NULL;

	param_str[0] = '\0';
	if (!as_info_parameter_get(params, "file", param_str, &param_str_len)) {
		file = cf_strdup(param_str);
	}

	param_str[0] = '\0';
	param_str_len = sizeof(param_str);
	if (!as_info_parameter_get(params, "options", param_str, &param_str_len)) {
		options = cf_strdup(param_str);
	}

	param_str[0] = '\0';
	param_str_len = sizeof(param_str);
	if (!as_info_parameter_get(params, "sites", param_str, &param_str_len)) {
		sites = cf_strdup(param_str);
	}

	cf_alloc_log_stats(file, options);

	if (file) {
		cf_free(file);
	}

	if (options) {
		cf_free(options);
	}

	if (sites) {
		cf_alloc_log_site_infos(sites);
		cf_free(sites);
	}

	cf_dyn_buf_append_string(db, "ok");
	return 0;
}

/*
 *  Print out clock skew information.
 */
int
info_command_dump_skew(char *name, char *params, cf_dyn_buf *db)
{
	cf_debug(AS_INFO, "dump-skew command received: params %s", params);

	/*
	 *  Command Format:  "dump-skew:"
	 */
	as_skew_monitor_dump();
	cf_dyn_buf_append_string(db, "ok");
	return 0;
}

int
info_command_mon_cmd(char *name, char *params, cf_dyn_buf *db)
{
	cf_debug(AS_INFO, "add-module command received: params %s", params);

	/*
	 *  Command Format:  "jobs:[module=<string>;cmd=<command>;<parameters>]"
	 *                   asinfo -v 'jobs'              -> list all jobs
	 *                   asinfo -v 'jobs:module=query' -> list all jobs for query module
	 *                   asinfo -v 'jobs:module=query;cmd=kill-job;trid=<trid>'
	 *                   asinfo -v 'jobs:module=query;cmd=set-priority;trid=<trid>;value=<val>'
	 *
	 *  where <module> is one of following:
	 *      - query
	 *      - scan
	 */

	char cmd[13];
	char module[21];
	char job_id[24];
	char val_str[11];
	int cmd_len       = sizeof(cmd);
	int module_len    = sizeof(module);
	int job_id_len    = sizeof(job_id);
	int val_len       = sizeof(val_str);
	uint64_t trid     = 0;
	uint32_t value    = 0;

	cmd[0]     = '\0';
	module[0]  = '\0';
	job_id[0]  = '\0';
	val_str[0] = '\0';

	// Read the parameters: module cmd trid value
	int rv = as_info_parameter_get(params, "module", module, &module_len);
	if (rv == -1) {
		as_mon_info_cmd(NULL, NULL, 0, 0, db);
		return 0;
	}
	else if (rv == -2) {
		cf_dyn_buf_append_string(db, "ERROR:");
		cf_dyn_buf_append_int(db, AS_ERR_PARAMETER);
		cf_dyn_buf_append_string(db, ":\"module\" parameter too long (> ");
		cf_dyn_buf_append_int(db, module_len-1);
		cf_dyn_buf_append_string(db, " chars)");
		return 0;
	}

	// For backward compatibility:
	if (strcmp(module, "scan") == 0) {
		strcpy(module, "query");
	}

	rv = as_info_parameter_get(params, "cmd", cmd, &cmd_len);
	if (rv == -1) {
		as_mon_info_cmd(module, NULL, 0, 0, db);
		return 0;
	}
	else if (rv == -2) {
		cf_dyn_buf_append_string(db, "ERROR:");
		cf_dyn_buf_append_int(db, AS_ERR_PARAMETER);
		cf_dyn_buf_append_string(db, ":\"cmd\" parameter too long (> ");
		cf_dyn_buf_append_int(db, cmd_len-1);
		cf_dyn_buf_append_string(db, " chars)");
		return 0;
	}

	rv = as_info_parameter_get(params, "trid", job_id, &job_id_len);
	if (rv == 0) {
		trid  = strtoull(job_id, NULL, 10);
	}
	else if (rv == -1) {
		cf_dyn_buf_append_string(db, "ERROR:");
		cf_dyn_buf_append_int(db, AS_ERR_PARAMETER);
		cf_dyn_buf_append_string(db, ":no \"trid\" parameter specified");
		return 0;
	}
	else if (rv == -2) {
		cf_dyn_buf_append_string(db, "ERROR:");
		cf_dyn_buf_append_int(db, AS_ERR_PARAMETER);
		cf_dyn_buf_append_string(db, ":\"trid\" parameter too long (> ");
		cf_dyn_buf_append_int(db, job_id_len-1);
		cf_dyn_buf_append_string(db, " chars)");
		return 0;
	}

	rv = as_info_parameter_get(params, "value", val_str, &val_len);
	if (rv == 0) {
		value = strtoul(val_str, NULL, 10);
	}
	else if (rv == -2) {
		cf_dyn_buf_append_string(db, "ERROR:");
		cf_dyn_buf_append_int(db, AS_ERR_PARAMETER);
		cf_dyn_buf_append_string(db, ":\"value\" parameter too long (> ");
		cf_dyn_buf_append_int(db, val_len-1);
		cf_dyn_buf_append_string(db, " chars)");
		return 0;
	}

	cf_info(AS_INFO, "%s %s %lu %u", module, cmd, trid, value);
	as_mon_info_cmd(module, cmd, trid, value, db);
	return 0;
}


static const char *
debug_allocations_string(void)
{
	switch (g_config.debug_allocations) {
	case CF_ALLOC_DEBUG_NONE:
		return "none";

	case CF_ALLOC_DEBUG_TRANSIENT:
		return "transient";

	case CF_ALLOC_DEBUG_PERSISTENT:
		return "persistent";

	case CF_ALLOC_DEBUG_ALL:
		return "all";

	default:
		cf_crash(CF_ALLOC, "invalid CF_ALLOC_DEBUG_* value");
		return NULL;
	}
}

static const char *
auto_pin_string(void)
{
	switch (g_config.auto_pin) {
	case CF_TOPO_AUTO_PIN_NONE:
		return "none";

	case CF_TOPO_AUTO_PIN_CPU:
		return "cpu";

	case CF_TOPO_AUTO_PIN_NUMA:
		return "numa";

	case CF_TOPO_AUTO_PIN_ADQ:
		return "adq";

	default:
		cf_crash(CF_ALLOC, "invalid CF_TOPO_AUTO_* value");
		return NULL;
	}
}

void
info_service_config_get(cf_dyn_buf *db)
{
	// Note - no user, group.

	info_append_bool(db, "advertise-ipv6", cf_socket_advertises_ipv6());
	info_append_string(db, "auto-pin", auto_pin_string());
	info_append_uint32(db, "batch-index-threads", g_config.n_batch_index_threads);
	info_append_uint32(db, "batch-max-buffers-per-queue", g_config.batch_max_buffers_per_queue);
	info_append_uint32(db, "batch-max-requests", g_config.batch_max_requests);
	info_append_uint32(db, "batch-max-unused-buffers", g_config.batch_max_unused_buffers);

	char cluster_name[AS_CLUSTER_NAME_SZ];
	info_get_printable_cluster_name(cluster_name);
	info_append_string(db, "cluster-name", cluster_name);

	info_append_string(db, "debug-allocations", debug_allocations_string());
	info_append_bool(db, "disable-udf-execution", g_config.udf_execution_disabled);
	info_append_bool(db, "downgrading", g_config.downgrading);
	info_append_bool(db, "enable-benchmarks-fabric", g_config.fabric_benchmarks_enabled);
	info_append_bool(db, "enable-health-check", g_config.health_check_enabled);
	info_append_bool(db, "enable-hist-info", g_config.info_hist_enabled);
	info_append_bool(db, "enforce-best-practices", g_config.enforce_best_practices);

	for (uint32_t i = 0; i < g_config.n_feature_key_files; i++) {
		info_append_indexed_string(db, "feature-key-file", i, NULL, g_config.feature_key_files[i]);
	}

	info_append_bool(db, "indent-allocations", g_config.indent_allocations);
	info_append_uint32(db, "info-threads", g_config.n_info_threads);
	info_append_bool(db, "keep-caps-ssd-health", g_config.keep_caps_ssd_health);
	info_append_bool(db, "log-local-time", cf_log_is_using_local_time());
	info_append_bool(db, "log-millis", cf_log_is_using_millis());
	info_append_bool(db, "microsecond-histograms", g_config.microsecond_histograms);
	info_append_uint32(db, "migrate-fill-delay", g_config.migrate_fill_delay);
	info_append_uint32(db, "migrate-max-num-incoming", g_config.migrate_max_num_incoming);
	info_append_uint32(db, "migrate-threads", g_config.n_migrate_threads);
	info_append_uint32(db, "min-cluster-size", g_config.clustering_config.cluster_size_min);
	info_append_uint64_x(db, "node-id", g_config.self_node); // may be configured or auto-generated
	info_append_string_safe(db, "node-id-interface", g_config.node_id_interface);
	info_append_bool(db, "os-group-perms", cf_os_is_using_group_perms());
	info_append_string_safe(db, "pidfile", g_config.pidfile);
	info_append_int(db, "proto-fd-idle-ms", g_config.proto_fd_idle_ms);
	info_append_uint32(db, "proto-fd-max", g_config.n_proto_fd_max);
	info_append_uint32(db, "query-max-done", g_config.query_max_done);
	info_append_uint32(db, "query-threads-limit", g_config.n_query_threads_limit);
	info_append_bool(db, "run-as-daemon", g_config.run_as_daemon);
	info_append_bool(db, "salt-allocations", g_config.salt_allocations);
	info_append_uint32(db, "service-threads", g_config.n_service_threads);
	info_append_uint32(db, "sindex-builder-threads", g_config.sindex_builder_threads);
	info_append_uint32(db, "sindex-gc-period", g_config.sindex_gc_period);
	info_append_bool(db, "stay-quiesced", g_config.stay_quiesced);
	info_append_uint32(db, "ticker-interval", g_config.ticker_interval);
	info_append_int(db, "transaction-max-ms", (int)(g_config.transaction_max_ns / 1000000));
	info_append_uint32(db, "transaction-retry-ms", g_config.transaction_retry_ms);
	info_append_string_safe(db, "vault-ca", g_vault_cfg.ca);
	info_append_string_safe(db, "vault-path", g_vault_cfg.path);
	info_append_string_safe(db, "vault-token-file", g_vault_cfg.token_file);
	info_append_string_safe(db, "vault-url", g_vault_cfg.url);
	info_append_string_safe(db, "work-directory", g_config.work_directory);
}

static void
append_addrs(cf_dyn_buf *db, const char *name, const cf_addr_list *list)
{
	for (uint32_t i = 0; i < list->n_addrs; ++i) {
		info_append_string(db, name, list->addrs[i]);
	}
}

void
info_network_config_get(cf_dyn_buf *db)
{
	// Service:

	info_append_int(db, "service.access-port", g_config.service.std_port);
	append_addrs(db, "service.access-address", &g_config.service.std);
	append_addrs(db, "service.address", &g_config.service.bind);
	info_append_int(db, "service.alternate-access-port", g_config.service.alt_port);
	append_addrs(db, "service.alternate-access-address", &g_config.service.alt);
	info_append_int(db, "service.port", g_config.service.bind_port);

	info_append_int(db, "service.tls-port", g_config.tls_service.bind_port);
	append_addrs(db, "service.tls-address", &g_config.tls_service.bind);
	info_append_int(db, "service.tls-access-port", g_config.tls_service.std_port);
	append_addrs(db, "service.tls-access-address", &g_config.tls_service.std);
	info_append_int(db, "service.tls-alternate-access-port", g_config.tls_service.alt_port);
	append_addrs(db, "service.tls-alternate-access-address", &g_config.tls_service.alt);
	info_append_string_safe(db, "service.tls-name", g_config.tls_service.tls_our_name);

	for (uint32_t i = 0; i < g_config.tls_service.n_tls_peer_names; ++i) {
		info_append_string(db, "service.tls-authenticate-client",
				g_config.tls_service.tls_peer_names[i]);
	}

	info_append_bool(db, "service.disable-localhost", g_config.service_localhost_disabled);

	// Heartbeat:

	as_hb_info_config_get(db);

	// Fabric:

	append_addrs(db, "fabric.address", &g_config.fabric.bind);
	append_addrs(db, "fabric.tls-address", &g_config.tls_fabric.bind);
	info_append_int(db, "fabric.tls-port", g_config.tls_fabric.bind_port);
	info_append_string_safe(db, "fabric.tls-name", g_config.tls_fabric.tls_our_name);
	info_append_uint32(db, "fabric.channel-bulk-fds", g_config.n_fabric_channel_fds[AS_FABRIC_CHANNEL_BULK]);
	info_append_uint32(db, "fabric.channel-bulk-recv-threads", g_config.n_fabric_channel_recv_threads[AS_FABRIC_CHANNEL_BULK]);
	info_append_uint32(db, "fabric.channel-ctrl-fds", g_config.n_fabric_channel_fds[AS_FABRIC_CHANNEL_CTRL]);
	info_append_uint32(db, "fabric.channel-ctrl-recv-threads", g_config.n_fabric_channel_recv_threads[AS_FABRIC_CHANNEL_CTRL]);
	info_append_uint32(db, "fabric.channel-meta-fds", g_config.n_fabric_channel_fds[AS_FABRIC_CHANNEL_META]);
	info_append_uint32(db, "fabric.channel-meta-recv-threads", g_config.n_fabric_channel_recv_threads[AS_FABRIC_CHANNEL_META]);
	info_append_uint32(db, "fabric.channel-rw-fds", g_config.n_fabric_channel_fds[AS_FABRIC_CHANNEL_RW]);
	info_append_uint32(db, "fabric.channel-rw-recv-pools", g_config.n_fabric_channel_recv_pools[AS_FABRIC_CHANNEL_RW]);
	info_append_uint32(db, "fabric.channel-rw-recv-threads", g_config.n_fabric_channel_recv_threads[AS_FABRIC_CHANNEL_RW]);
	info_append_bool(db, "fabric.keepalive-enabled", g_config.fabric_keepalive_enabled);
	info_append_int(db, "fabric.keepalive-intvl", g_config.fabric_keepalive_intvl);
	info_append_int(db, "fabric.keepalive-probes", g_config.fabric_keepalive_probes);
	info_append_int(db, "fabric.keepalive-time", g_config.fabric_keepalive_time);
	info_append_int(db, "fabric.latency-max-ms", g_config.fabric_latency_max_ms);
	info_append_int(db, "fabric.port", g_config.fabric.bind_port);
	info_append_int(db, "fabric.recv-rearm-threshold", g_config.fabric_recv_rearm_threshold);
	info_append_int(db, "fabric.send-threads", g_config.n_fabric_send_threads);

	// Info:

	append_addrs(db, "info.address", &g_config.info.bind);
	info_append_int(db, "info.port", g_config.info.bind_port);

	// TLS:

	for (uint32_t i = 0; i < g_config.n_tls_specs; ++i) {
		cf_tls_spec *spec = g_config.tls_specs + i;
		char key[100];

		snprintf(key, sizeof(key), "tls[%u].name", i);
		info_append_string_safe(db, key, spec->name);

		snprintf(key, sizeof(key), "tls[%u].ca-file", i);
		info_append_string_safe(db, key, spec->ca_file);

		snprintf(key, sizeof(key), "tls[%u].ca-path", i);
		info_append_string_safe(db, key, spec->ca_path);

		snprintf(key, sizeof(key), "tls[%u].cert-blacklist", i);
		info_append_string_safe(db, key, spec->cert_blacklist);

		snprintf(key, sizeof(key), "tls[%u].cert-file", i);
		info_append_string_safe(db, key, spec->cert_file);

		snprintf(key, sizeof(key), "tls[%u].cipher-suite", i);
		info_append_string_safe(db, key, spec->cipher_suite);

		snprintf(key, sizeof(key), "tls[%u].key-file", i);
		info_append_string_safe(db, key, spec->key_file);

		snprintf(key, sizeof(key), "tls[%u].key-file-password", i);
		info_append_string_safe(db, key, spec->key_file_password);

		snprintf(key, sizeof(key), "tls[%u].protocols", i);
		info_append_string_safe(db, key, spec->protocols);
	}
}


void
info_namespace_config_get(char* context, cf_dyn_buf *db)
{
	as_namespace *ns = as_namespace_get_byname(context);

	if (! ns) {
		cf_dyn_buf_append_string(db, "ERROR::namespace not found");
		return;
	}

	info_append_bool(db, "allow-ttl-without-nsup", ns->allow_ttl_without_nsup);
	info_append_uint32(db, "background-query-max-rps", ns->background_query_max_rps);

	if (ns->conflict_resolution_policy == AS_NAMESPACE_CONFLICT_RESOLUTION_POLICY_GENERATION) {
		info_append_string(db, "conflict-resolution-policy", "generation");
	}
	else if (ns->conflict_resolution_policy == AS_NAMESPACE_CONFLICT_RESOLUTION_POLICY_LAST_UPDATE_TIME) {
		info_append_string(db, "conflict-resolution-policy", "last-update-time");
	}
	else {
		info_append_string(db, "conflict-resolution-policy", "undefined");
	}

	info_append_bool(db, "conflict-resolve-writes", ns->conflict_resolve_writes);
	info_append_bool(db, "data-in-index", ns->data_in_index);
	info_append_uint32(db, "default-ttl", ns->default_ttl);
	info_append_bool(db, "disable-cold-start-eviction", ns->cold_start_eviction_disabled);
	info_append_bool(db, "disable-write-dup-res", ns->write_dup_res_disabled);
	info_append_bool(db, "disallow-null-setname", ns->disallow_null_setname);
	info_append_bool(db, "enable-benchmarks-batch-sub", ns->batch_sub_benchmarks_enabled);
	info_append_bool(db, "enable-benchmarks-ops-sub", ns->ops_sub_benchmarks_enabled);
	info_append_bool(db, "enable-benchmarks-read", ns->read_benchmarks_enabled);
	info_append_bool(db, "enable-benchmarks-udf", ns->udf_benchmarks_enabled);
	info_append_bool(db, "enable-benchmarks-udf-sub", ns->udf_sub_benchmarks_enabled);
	info_append_bool(db, "enable-benchmarks-write", ns->write_benchmarks_enabled);
	info_append_bool(db, "enable-hist-proxy", ns->proxy_hist_enabled);
	info_append_uint32(db, "evict-hist-buckets", ns->evict_hist_buckets);
	info_append_uint32(db, "evict-tenths-pct", ns->evict_tenths_pct);
	info_append_uint32(db, "high-water-disk-pct", ns->hwm_disk_pct);
	info_append_uint32(db, "high-water-memory-pct", ns->hwm_memory_pct);
	info_append_bool(db, "ignore-migrate-fill-delay", ns->ignore_migrate_fill_delay);
	info_append_uint64(db, "index-stage-size", ns->index_stage_size);

	info_append_string(db, "index-type",
			ns->xmem_type == CF_XMEM_TYPE_MEM ? "mem" :
					(ns->xmem_type == CF_XMEM_TYPE_SHMEM ? "shmem" :
							(ns->xmem_type == CF_XMEM_TYPE_PMEM ? "pmem" :
									(ns->xmem_type == CF_XMEM_TYPE_FLASH ? "flash" :
											"illegal"))));

	info_append_uint32(db, "max-record-size", ns->max_record_size);
	info_append_uint64(db, "memory-size", ns->memory_size);
	info_append_uint32(db, "migrate-order", ns->migrate_order);
	info_append_uint32(db, "migrate-retransmit-ms", ns->migrate_retransmit_ms);
	info_append_uint32(db, "migrate-sleep", ns->migrate_sleep);
	info_append_uint32(db, "nsup-hist-period", ns->nsup_hist_period);
	info_append_uint32(db, "nsup-period", ns->nsup_period);
	info_append_uint32(db, "nsup-threads", ns->n_nsup_threads);
	info_append_uint32(db, "partition-tree-sprigs", ns->tree_shared.n_sprigs);
	info_append_bool(db, "prefer-uniform-balance", ns->cfg_prefer_uniform_balance);
	info_append_uint32(db, "rack-id", ns->rack_id);
	info_append_string(db, "read-consistency-level-override", NS_READ_CONSISTENCY_LEVEL_NAME());
	info_append_bool(db, "reject-non-xdr-writes", ns->reject_non_xdr_writes);
	info_append_bool(db, "reject-xdr-writes", ns->reject_xdr_writes);
	info_append_uint32(db, "replication-factor", ns->cfg_replication_factor);
	info_append_uint64(db, "sindex-stage-size", ns->sindex_stage_size);
	info_append_bool(db, "single-bin", ns->single_bin);
	info_append_uint32(db, "single-query-threads", ns->n_single_query_threads);
	info_append_uint32(db, "stop-writes-pct", ns->stop_writes_pct);
	info_append_bool(db, "strong-consistency", ns->cp);
	info_append_bool(db, "strong-consistency-allow-expunge", ns->cp_allow_drops);
	info_append_uint32(db, "tomb-raider-eligible-age", ns->tomb_raider_eligible_age);
	info_append_uint32(db, "tomb-raider-period", ns->tomb_raider_period);
	info_append_uint32(db, "transaction-pending-limit", ns->transaction_pending_limit);
	info_append_uint32(db, "truncate-threads", ns->n_truncate_threads);
	info_append_string(db, "write-commit-level-override", NS_WRITE_COMMIT_LEVEL_NAME());
	info_append_uint64(db, "xdr-bin-tombstone-ttl", ns->xdr_bin_tombstone_ttl_ms / 1000);
	info_append_uint32(db, "xdr-tomb-raider-period", ns->xdr_tomb_raider_period);
	info_append_uint32(db, "xdr-tomb-raider-threads", ns->n_xdr_tomb_raider_threads);

	for (uint32_t i = 0; i < ns->n_xmem_mounts; i++) {
		info_append_indexed_string(db, "index-type.mount", i, NULL, ns->xmem_mounts[i]);
	}

	if (as_namespace_index_persisted(ns)) {
		info_append_uint32(db, "index-type.mounts-high-water-pct", ns->mounts_hwm_pct);
		info_append_uint64(db, "index-type.mounts-size-limit", ns->mounts_size_limit);
	}

	info_append_string(db, "storage-engine",
			(ns->storage_type == AS_STORAGE_ENGINE_MEMORY ? "memory" :
				(ns->storage_type == AS_STORAGE_ENGINE_PMEM ? "pmem" :
					(ns->storage_type == AS_STORAGE_ENGINE_SSD ? "device" : "illegal"))));

	if (ns->storage_type == AS_STORAGE_ENGINE_PMEM) {
		uint32_t n = as_namespace_device_count(ns);

		for (uint32_t i = 0; i < n; i++) {
			info_append_indexed_string(db, "storage-engine.file", i, NULL, ns->storage_devices[i]);

			if (ns->n_storage_shadows != 0) {
				info_append_indexed_string(db, "storage-engine.file", i, "shadow", ns->storage_shadows[i]);
			}
		}

		info_append_bool(db, "storage-engine.commit-to-device", ns->storage_commit_to_device);
		info_append_string(db, "storage-engine.compression", NS_COMPRESSION());
		info_append_uint32(db, "storage-engine.compression-level", NS_COMPRESSION_LEVEL());
		info_append_uint32(db, "storage-engine.defrag-lwm-pct", ns->storage_defrag_lwm_pct);
		info_append_uint32(db, "storage-engine.defrag-queue-min", ns->storage_defrag_queue_min);
		info_append_uint32(db, "storage-engine.defrag-sleep", ns->storage_defrag_sleep);
		info_append_uint32(db, "storage-engine.defrag-startup-minimum", ns->storage_defrag_startup_minimum);
		info_append_bool(db, "storage-engine.direct-files", ns->storage_direct_files);
		info_append_bool(db, "storage-engine.disable-odsync", ns->storage_disable_odsync);
		info_append_bool(db, "storage-engine.enable-benchmarks-storage", ns->storage_benchmarks_enabled);

		if (ns->storage_encryption_key_file != NULL) {
			info_append_string(db, "storage-engine.encryption",
				ns->storage_encryption == AS_ENCRYPTION_AES_128 ? "aes-128" :
					(ns->storage_encryption == AS_ENCRYPTION_AES_256 ? "aes-256" :
						"illegal"));
		}

		info_append_string_safe(db, "storage-engine.encryption-key-file", ns->storage_encryption_key_file);
		info_append_string_safe(db, "storage-engine.encryption-old-key-file", ns->storage_encryption_old_key_file);
		info_append_uint64(db, "storage-engine.filesize", ns->storage_filesize);
		info_append_uint64(db, "storage-engine.flush-max-ms", ns->storage_flush_max_us / 1000);
		info_append_uint64(db, "storage-engine.max-write-cache", ns->storage_max_write_cache);
		info_append_uint32(db, "storage-engine.min-avail-pct", ns->storage_min_avail_pct);
		info_append_bool(db, "storage-engine.serialize-tomb-raider", ns->storage_serialize_tomb_raider);
		info_append_uint32(db, "storage-engine.tomb-raider-sleep", ns->storage_tomb_raider_sleep);
	}
	else if (ns->storage_type == AS_STORAGE_ENGINE_SSD) {
		uint32_t n = as_namespace_device_count(ns);
		const char* tag = ns->n_storage_devices != 0 ?
				"storage-engine.device" : "storage-engine.file";

		for (uint32_t i = 0; i < n; i++) {
			info_append_indexed_string(db, tag, i, NULL, ns->storage_devices[i]);

			if (ns->n_storage_shadows != 0) {
				info_append_indexed_string(db, tag, i, "shadow", ns->storage_shadows[i]);
			}
		}

		info_append_bool(db, "storage-engine.cache-replica-writes", ns->storage_cache_replica_writes);
		info_append_bool(db, "storage-engine.cold-start-empty", ns->storage_cold_start_empty);
		info_append_bool(db, "storage-engine.commit-to-device", ns->storage_commit_to_device);
		info_append_uint32(db, "storage-engine.commit-min-size", ns->storage_commit_min_size);
		info_append_string(db, "storage-engine.compression", NS_COMPRESSION());
		info_append_uint32(db, "storage-engine.compression-level", NS_COMPRESSION_LEVEL());
		info_append_bool(db, "storage-engine.data-in-memory", ns->storage_data_in_memory);
		info_append_uint32(db, "storage-engine.defrag-lwm-pct", ns->storage_defrag_lwm_pct);
		info_append_uint32(db, "storage-engine.defrag-queue-min", ns->storage_defrag_queue_min);
		info_append_uint32(db, "storage-engine.defrag-sleep", ns->storage_defrag_sleep);
		info_append_uint32(db, "storage-engine.defrag-startup-minimum", ns->storage_defrag_startup_minimum);
		info_append_bool(db, "storage-engine.direct-files", ns->storage_direct_files);
		info_append_bool(db, "storage-engine.disable-odsync", ns->storage_disable_odsync);
		info_append_bool(db, "storage-engine.enable-benchmarks-storage", ns->storage_benchmarks_enabled);

		if (ns->storage_encryption_key_file != NULL) {
			info_append_string(db, "storage-engine.encryption",
				ns->storage_encryption == AS_ENCRYPTION_AES_128 ? "aes-128" :
					(ns->storage_encryption == AS_ENCRYPTION_AES_256 ? "aes-256" :
						"illegal"));
		}

		info_append_string_safe(db, "storage-engine.encryption-key-file", ns->storage_encryption_key_file);
		info_append_string_safe(db, "storage-engine.encryption-old-key-file", ns->storage_encryption_old_key_file);
		info_append_uint64(db, "storage-engine.filesize", ns->storage_filesize);
		info_append_uint64(db, "storage-engine.flush-max-ms", ns->storage_flush_max_us / 1000);
		info_append_uint64(db, "storage-engine.max-write-cache", ns->storage_max_write_cache);
		info_append_uint32(db, "storage-engine.min-avail-pct", ns->storage_min_avail_pct);
		info_append_uint32(db, "storage-engine.post-write-queue", ns->storage_post_write_queue);
		info_append_bool(db, "storage-engine.read-page-cache", ns->storage_read_page_cache);
		info_append_string_safe(db, "storage-engine.scheduler-mode", ns->storage_scheduler_mode);
		info_append_bool(db, "storage-engine.serialize-tomb-raider", ns->storage_serialize_tomb_raider);
		info_append_bool(db, "storage-engine.sindex-startup-device-scan", ns->storage_sindex_startup_device_scan);
		info_append_uint32(db, "storage-engine.tomb-raider-sleep", ns->storage_tomb_raider_sleep);
		info_append_uint32(db, "storage-engine.write-block-size", ns->storage_write_block_size);
	}

	info_append_bool(db, "geo2dsphere-within.strict", ns->geo2dsphere_within_strict);
	info_append_uint32(db, "geo2dsphere-within.min-level", (uint32_t)ns->geo2dsphere_within_min_level);
	info_append_uint32(db, "geo2dsphere-within.max-level", (uint32_t)ns->geo2dsphere_within_max_level);
	info_append_uint32(db, "geo2dsphere-within.max-cells", (uint32_t)ns->geo2dsphere_within_max_cells);
	info_append_uint32(db, "geo2dsphere-within.level-mod", (uint32_t)ns->geo2dsphere_within_level_mod);
	info_append_uint32(db, "geo2dsphere-within.earth-radius-meters", ns->geo2dsphere_within_earth_radius_meters);
}


void
info_command_config_get_with_params(char *name, char *params, cf_dyn_buf *db)
{
	char context[1024];
	int context_len = sizeof(context);

	if (as_info_parameter_get(params, "context", context, &context_len) != 0) {
		cf_dyn_buf_append_string(db, "Error::invalid get-config parameter");
		return;
	}

	if (strcmp(context, "service") == 0) {
		info_service_config_get(db);
	}
	else if (strcmp(context, "network") == 0) {
		info_network_config_get(db);
	}
	else if (strcmp(context, "namespace") == 0) {
		context_len = sizeof(context);

		if (as_info_parameter_get(params, "id", context, &context_len) != 0) {
			cf_dyn_buf_append_string(db, "Error::invalid id");
			return;
		}

		info_namespace_config_get(context, db);
	}
	else if (strcmp(context, "security") == 0) {
		as_security_get_config(db);
	}
	else if (strcmp(context, "xdr") == 0) {
		as_xdr_get_config(params, db);
	}
	else {
		cf_dyn_buf_append_string(db, "Error::invalid context");
	}
}


int
info_command_config_get(char *name, char *params, cf_dyn_buf *db)
{
	if (params && *params != 0) {
		cf_debug(AS_INFO, "config-get command received: params %s", params);

		info_command_config_get_with_params(name, params, db);
		// Response may be an error string (without a semicolon).
		cf_dyn_buf_chomp_char(db, ';');
		return 0;
	}

	cf_debug(AS_INFO, "config-get command received");

	// We come here when context is not mentioned.
	// In that case we want to print everything.
	info_service_config_get(db);
	info_network_config_get(db);
	as_security_get_config(db);

	cf_dyn_buf_chomp(db);

	return 0;
}

int
info_command_get_stats(char *name, char *params, cf_dyn_buf *db)
{
	char context[1024];
	int context_len = sizeof(context);

	if (as_info_parameter_get(params, "context", context, &context_len) != 0) {
		cf_dyn_buf_append_string(db, "ERROR::missing-context");
		return 0;
	}

	if (strcmp(context, "xdr") == 0) {
		as_xdr_get_stats(params, db);
	}
	else {
		cf_dyn_buf_append_string(db, "ERROR::unknown-context");
	}

	return 0;
}


//
// Dynamic enable/disable histogram helpers.
//

static void
fabric_histogram_clear_all(void)
{
	histogram_scale scale = as_config_histogram_scale();

	histogram_rescale(g_stats.fabric_send_init_hists[AS_FABRIC_CHANNEL_BULK], scale);
	histogram_rescale(g_stats.fabric_send_fragment_hists[AS_FABRIC_CHANNEL_BULK], scale);
	histogram_rescale(g_stats.fabric_recv_fragment_hists[AS_FABRIC_CHANNEL_BULK], scale);
	histogram_rescale(g_stats.fabric_recv_cb_hists[AS_FABRIC_CHANNEL_BULK], scale);
	histogram_rescale(g_stats.fabric_send_init_hists[AS_FABRIC_CHANNEL_CTRL], scale);
	histogram_rescale(g_stats.fabric_send_fragment_hists[AS_FABRIC_CHANNEL_CTRL], scale);
	histogram_rescale(g_stats.fabric_recv_fragment_hists[AS_FABRIC_CHANNEL_CTRL], scale);
	histogram_rescale(g_stats.fabric_recv_cb_hists[AS_FABRIC_CHANNEL_CTRL], scale);
	histogram_rescale(g_stats.fabric_send_init_hists[AS_FABRIC_CHANNEL_META], scale);
	histogram_rescale(g_stats.fabric_send_fragment_hists[AS_FABRIC_CHANNEL_META], scale);
	histogram_rescale(g_stats.fabric_recv_fragment_hists[AS_FABRIC_CHANNEL_META], scale);
	histogram_rescale(g_stats.fabric_recv_cb_hists[AS_FABRIC_CHANNEL_META], scale);
	histogram_rescale(g_stats.fabric_send_init_hists[AS_FABRIC_CHANNEL_RW], scale);
	histogram_rescale(g_stats.fabric_send_fragment_hists[AS_FABRIC_CHANNEL_RW], scale);
	histogram_rescale(g_stats.fabric_recv_fragment_hists[AS_FABRIC_CHANNEL_RW], scale);
	histogram_rescale(g_stats.fabric_recv_cb_hists[AS_FABRIC_CHANNEL_RW], scale);
}

static void
read_benchmarks_histogram_clear_all(as_namespace* ns)
{
	histogram_scale scale = as_config_histogram_scale();

	histogram_rescale(ns->read_start_hist, scale);
	histogram_rescale(ns->read_restart_hist, scale);
	histogram_rescale(ns->read_dup_res_hist, scale);
	histogram_rescale(ns->read_repl_ping_hist, scale);
	histogram_rescale(ns->read_local_hist, scale);
	histogram_rescale(ns->read_response_hist, scale);
}

static void
write_benchmarks_histogram_clear_all(as_namespace* ns)
{
	histogram_scale scale = as_config_histogram_scale();

	histogram_rescale(ns->write_start_hist, scale);
	histogram_rescale(ns->write_restart_hist, scale);
	histogram_rescale(ns->write_dup_res_hist, scale);
	histogram_rescale(ns->write_master_hist, scale);
	histogram_rescale(ns->write_repl_write_hist, scale);
	histogram_rescale(ns->write_response_hist, scale);
}

static void
udf_benchmarks_histogram_clear_all(as_namespace* ns)
{
	histogram_scale scale = as_config_histogram_scale();

	histogram_rescale(ns->udf_start_hist, scale);
	histogram_rescale(ns->udf_restart_hist, scale);
	histogram_rescale(ns->udf_dup_res_hist, scale);
	histogram_rescale(ns->udf_master_hist, scale);
	histogram_rescale(ns->udf_repl_write_hist, scale);
	histogram_rescale(ns->udf_response_hist, scale);
}

static void
batch_sub_benchmarks_histogram_clear_all(as_namespace* ns)
{
	histogram_scale scale = as_config_histogram_scale();

	histogram_rescale(ns->batch_sub_prestart_hist, scale);
	histogram_rescale(ns->batch_sub_start_hist, scale);
	histogram_rescale(ns->batch_sub_restart_hist, scale);
	histogram_rescale(ns->batch_sub_dup_res_hist, scale);
	histogram_rescale(ns->batch_sub_repl_ping_hist, scale);
	histogram_rescale(ns->batch_sub_read_local_hist, scale);
	histogram_rescale(ns->batch_sub_write_master_hist, scale);
	histogram_rescale(ns->batch_sub_udf_master_hist, scale);
	histogram_rescale(ns->batch_sub_repl_write_hist, scale);
	histogram_rescale(ns->batch_sub_response_hist, scale);
}

static void
udf_sub_benchmarks_histogram_clear_all(as_namespace* ns)
{
	histogram_scale scale = as_config_histogram_scale();

	histogram_rescale(ns->udf_sub_start_hist, scale);
	histogram_rescale(ns->udf_sub_restart_hist, scale);
	histogram_rescale(ns->udf_sub_dup_res_hist, scale);
	histogram_rescale(ns->udf_sub_master_hist, scale);
	histogram_rescale(ns->udf_sub_repl_write_hist, scale);
	histogram_rescale(ns->udf_sub_response_hist, scale);
}

static void
ops_sub_benchmarks_histogram_clear_all(as_namespace* ns)
{
	histogram_scale scale = as_config_histogram_scale();

	histogram_rescale(ns->ops_sub_start_hist, scale);
	histogram_rescale(ns->ops_sub_restart_hist, scale);
	histogram_rescale(ns->ops_sub_dup_res_hist, scale);
	histogram_rescale(ns->ops_sub_master_hist, scale);
	histogram_rescale(ns->ops_sub_repl_write_hist, scale);
	histogram_rescale(ns->ops_sub_response_hist, scale);
}

static bool
any_benchmarks_enabled(void)
{
	for (uint32_t ns_ix = 0; ns_ix < g_config.n_namespaces; ns_ix++) {
		as_namespace* ns = g_config.namespaces[ns_ix];

		if (ns->read_benchmarks_enabled ||
				ns->write_benchmarks_enabled ||
				ns->udf_benchmarks_enabled ||
				ns->batch_sub_benchmarks_enabled ||
				ns->udf_sub_benchmarks_enabled ||
				ns->ops_sub_benchmarks_enabled) {
			return true;
		}
	}

	return g_config.fabric_benchmarks_enabled;
}


//
// config-set:context=service;variable=value;
// config-set:context=network;variable=heartbeat.value;
// config-set:context=namespace;id=test;variable=value;
//
int
info_command_config_set_threadsafe(char *name, char *params, cf_dyn_buf *db)
{
	cf_debug(AS_INFO, "config-set command received: params %s", params);

	char context[1024];
	int  context_len = sizeof(context);
	int val;
	char bool_val[2][6] = {"false", "true"};

	if (0 != as_info_parameter_get(params, "context", context, &context_len))
		goto Error;
	if (strcmp(context, "service") == 0) {
		context_len = sizeof(context);
		if (0 == as_info_parameter_get(params, "advertise-ipv6", context, &context_len)) {
			if (strcmp(context, "true") == 0 || strcmp(context, "yes") == 0) {
				cf_socket_set_advertise_ipv6(true);
			}
			else if (strcmp(context, "false") == 0 || strcmp(context, "no") == 0) {
				cf_socket_set_advertise_ipv6(false);
			}
			else {
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "service-threads", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val)) {
				goto Error;
			}
			if (val < 1 || val > MAX_SERVICE_THREADS) {
				cf_warning(AS_INFO, "service-threads must be between 1 and %u", MAX_SERVICE_THREADS);
				goto Error;
			}
			uint16_t n_cpus = cf_topo_count_cpus();
			if (g_config.auto_pin != CF_TOPO_AUTO_PIN_NONE && val % n_cpus != 0) {
				cf_warning(AS_INFO, "with auto-pin, service-threads must be a multiple of the number of CPUs (%hu)", n_cpus);
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of service-threads from %u to %d ", g_config.n_service_threads, val);
			as_service_set_threads((uint32_t)val);
		}
		else if (0 == as_info_parameter_get(params, "transaction-retry-ms", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val))
				goto Error;
			if (val == 0)
				goto Error;
			cf_info(AS_INFO, "Changing value of transaction-retry-ms from %d to %d ", g_config.transaction_retry_ms, val);
			g_config.transaction_retry_ms = val;
		}
		else if (0 == as_info_parameter_get(params, "transaction-max-ms", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val))
				goto Error;
			cf_info(AS_INFO, "Changing value of transaction-max-ms from %"PRIu64" to %d ", (g_config.transaction_max_ns / 1000000), val);
			g_config.transaction_max_ns = (uint64_t)val * 1000000;
		}
		else if (0 == as_info_parameter_get(params, "ticker-interval", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val))
				goto Error;
			cf_info(AS_INFO, "Changing value of ticker-interval from %d to %d ", g_config.ticker_interval, val);
			g_config.ticker_interval = val;
		}
		else if (0 == as_info_parameter_get(params, "query-max-done", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val))
				goto Error;
			if (val < 0 || val > 10000) {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of query-max-done from %d to %d ", g_config.query_max_done, val);
			g_config.query_max_done = val;
			as_query_limit_finished_jobs();
		}
		else if (0 == as_info_parameter_get(params, "query-threads-limit", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val) || val < 1 || val > 1024) {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of query-threads-limit from %u to %d ", g_config.n_query_threads_limit, val);
			g_config.n_query_threads_limit = (uint32_t)val;
		}
		else if (0 == as_info_parameter_get(params, "batch-index-threads", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val))
				goto Error;
			if (0 != as_batch_threads_resize(val))
				goto Error;
		}
		else if (0 == as_info_parameter_get(params, "batch-max-requests", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val))
				goto Error;
			cf_info(AS_INFO, "Changing value of batch-max-requests from %d to %d ", g_config.batch_max_requests, val);
			g_config.batch_max_requests = val;
		}
		else if (0 == as_info_parameter_get(params, "batch-max-buffers-per-queue", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val))
				goto Error;
			cf_info(AS_INFO, "Changing value of batch-max-buffers-per-queue from %d to %d ", g_config.batch_max_buffers_per_queue, val);
			g_config.batch_max_buffers_per_queue = val;
		}
		else if (0 == as_info_parameter_get(params, "batch-max-unused-buffers", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val))
				goto Error;
			cf_info(AS_INFO, "Changing value of batch-max-unused-buffers from %d to %d ", g_config.batch_max_unused_buffers, val);
			g_config.batch_max_unused_buffers = val;
		}
		else if (0 == as_info_parameter_get(params, "proto-fd-max", context, &context_len)) {
			if (cf_str_atoi(context, &val) != 0 || val < MIN_PROTO_FD_MAX || val > MAX_PROTO_FD_MAX) {
				cf_warning(AS_INFO, "invalid proto-fd-max %d", val);
				goto Error;
			}
			uint32_t prev_val = g_config.n_proto_fd_max;
			if (! as_service_set_proto_fd_max((uint32_t)val)) {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of proto-fd-max from %u to %d ", prev_val, val);
		}
		else if (0 == as_info_parameter_get(params, "proto-fd-idle-ms", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val))
				goto Error;
			cf_info(AS_INFO, "Changing value of proto-fd-idle-ms from %d to %d ", g_config.proto_fd_idle_ms, val);
			g_config.proto_fd_idle_ms = val;
		}
		else if (0 == as_info_parameter_get( params, "cluster-name", context, &context_len)){
			if (!as_config_cluster_name_set(context)) {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of cluster-name to '%s'", context);
		}
		else if (0 == as_info_parameter_get(params, "info-threads", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val)) {
				goto Error;
			}
			if (val < 1 || val > MAX_INFO_THREADS) {
				cf_warning(AS_INFO, "info-threads %d must be between 1 and %u", val, MAX_INFO_THREADS);
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of info-threads from %u to %d ", g_config.n_info_threads, val);
			info_set_num_info_threads((uint32_t)val);
		}
		else if (0 == as_info_parameter_get(params, "migrate-fill-delay", context, &context_len)) {
			if (as_config_error_enterprise_only()) {
				cf_warning(AS_INFO, "migrate-fill-delay is enterprise-only");
				goto Error;
			}
			uint32_t val;
			if (0 != cf_str_atoi_seconds(context, &val)) {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of migrate-fill-delay from %u to %u ", g_config.migrate_fill_delay, val);
			g_config.migrate_fill_delay = val;
		}
		else if (0 == as_info_parameter_get(params, "migrate-max-num-incoming", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val)) {
				goto Error;
			}
			if ((uint32_t)val > AS_MIGRATE_LIMIT_MAX_NUM_INCOMING) {
				cf_warning(AS_INFO, "migrate-max-num-incoming %d must be >= 0 and <= %u", val, AS_MIGRATE_LIMIT_MAX_NUM_INCOMING);
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of migrate-max-num-incoming from %u to %d ", g_config.migrate_max_num_incoming, val);
			g_config.migrate_max_num_incoming = (uint32_t)val;
		}
		else if (0 == as_info_parameter_get(params, "migrate-threads", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val)) {
				goto Error;
			}
			if ((uint32_t)val > MAX_NUM_MIGRATE_XMIT_THREADS) {
				cf_warning(AS_INFO, "migrate-threads %d must be >= 0 and <= %u", val, MAX_NUM_MIGRATE_XMIT_THREADS);
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of migrate-threads from %u to %d ", g_config.n_migrate_threads, val);
			as_migrate_set_num_xmit_threads(val);
		}
		else if (0 == as_info_parameter_get(params, "min-cluster-size", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val) || (0 > val) || (as_clustering_cluster_size_min_set(val) < 0))
				goto Error;
		}
		else if (0 == as_info_parameter_get(params, "sindex-builder-threads", context, &context_len)) {
			int val = 0;
			if (0 != cf_str_atoi(context, &val) || (val > 32)) {
				cf_warning(AS_INFO, "sindex-builder-threads: value must be <= 32, not %s", context);
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of sindex-builder-threads from %u to %d", g_config.sindex_builder_threads, val);
			g_config.sindex_builder_threads = (uint32_t)val;
		}
		else if (0 == as_info_parameter_get(params, "sindex-gc-period", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val))
				goto Error;
			cf_info(AS_INFO, "Changing value of sindex-gc-period from %d to %d ", g_config.sindex_gc_period, val);
			g_config.sindex_gc_period = (uint32_t)val;
		}
		else if (0 == as_info_parameter_get(params, "microsecond-histograms", context, &context_len)) {
			if (any_benchmarks_enabled()) {
				cf_warning(AS_INFO, "microsecond-histograms can only be changed if all microbenchmark histograms are disabled");
				goto Error;
			}
			if (strncmp(context, "true", 4) == 0 || strncmp(context, "yes", 3) == 0) {
				cf_info(AS_INFO, "Changing value of microsecond-histograms to %s", context);
				g_config.microsecond_histograms = true;
			}
			else if (strncmp(context, "false", 5) == 0 || strncmp(context, "no", 2) == 0) {
				cf_info(AS_INFO, "Changing value of microsecond-histograms to %s", context);
				g_config.microsecond_histograms = false;
			}
			else {
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "enable-benchmarks-fabric", context, &context_len)) {
			if (strncmp(context, "true", 4) == 0 || strncmp(context, "yes", 3) == 0) {
				cf_info(AS_INFO, "Changing value of enable-benchmarks-fabric to %s", context);
				if (! g_config.fabric_benchmarks_enabled) {
					fabric_histogram_clear_all();
				}
				g_config.fabric_benchmarks_enabled = true;
			}
			else if (strncmp(context, "false", 5) == 0 || strncmp(context, "no", 2) == 0) {
				cf_info(AS_INFO, "Changing value of enable-benchmarks-fabric to %s", context);
				g_config.fabric_benchmarks_enabled = false;
				fabric_histogram_clear_all();
			}
			else {
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "enable-health-check", context, &context_len)) {
			if (strncmp(context, "true", 4) == 0 || strncmp(context, "yes", 3) == 0) {
				cf_info(AS_INFO, "Changing value of enable-health-check to %s", context);
				g_config.health_check_enabled = true;
			}
			else if (strncmp(context, "false", 5) == 0 || strncmp(context, "no", 2) == 0) {
				cf_info(AS_INFO, "Changing value of enable-health-check to %s", context);
				g_config.health_check_enabled = false;
			}
			else {
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "enable-hist-info", context, &context_len)) {
			if (strncmp(context, "true", 4) == 0 || strncmp(context, "yes", 3) == 0) {
				cf_info(AS_INFO, "Changing value of enable-hist-info to %s", context);
				if (! g_config.info_hist_enabled) {
					histogram_clear(g_stats.info_hist);
				}
				g_config.info_hist_enabled = true;
			}
			else if (strncmp(context, "false", 5) == 0 || strncmp(context, "no", 2) == 0) {
				cf_info(AS_INFO, "Changing value of enable-hist-info to %s", context);
				g_config.info_hist_enabled = false;
				histogram_clear(g_stats.info_hist);
			}
			else {
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "downgrading", context, &context_len)) {
			if (strncmp(context, "true", 4) == 0 || strncmp(context, "yes", 3) == 0) {
				cf_info(AS_INFO, "Changing value of downgrading to %s", context);
				g_config.downgrading = true;
			}
			else if (strncmp(context, "false", 5) == 0 || strncmp(context, "no", 2) == 0) {
				cf_info(AS_INFO, "Changing value of downgrading to %s", context);
				g_config.downgrading = false;
			}
			else {
				goto Error;
			}
		}
		else {
			goto Error;
		}
	}
	else if (strcmp(context, "network") == 0) {
		context_len = sizeof(context);
		if (0 == as_info_parameter_get(params, "heartbeat.interval", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val))
				goto Error;
			if (as_hb_tx_interval_set(val) != 0) {
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "heartbeat.timeout", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val))
				goto Error;
			if (as_hb_max_intervals_missed_set(val) != 0){
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "heartbeat.connect-timeout-ms", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val))
				goto Error;
			if (as_hb_connect_timeout_set(val) != 0){
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "heartbeat.mtu", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val))
				goto Error;
			as_hb_override_mtu_set(val);
		}
		else if (0 == as_info_parameter_get(params, "heartbeat.protocol", context, &context_len)) {
			as_hb_protocol protocol =	(!strcmp(context, "v3") ? AS_HB_PROTOCOL_V3 :
											(!strcmp(context, "reset") ? AS_HB_PROTOCOL_RESET :
												(!strcmp(context, "none") ? AS_HB_PROTOCOL_NONE :
													AS_HB_PROTOCOL_UNDEF)));
			if (AS_HB_PROTOCOL_UNDEF == protocol) {
				cf_warning(AS_INFO, "heartbeat protocol version %s not supported", context);
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of heartbeat protocol version to %s", context);
			if (0 > as_hb_protocol_set(protocol))
				goto Error;
		}
		else if (0 == as_info_parameter_get(params, "fabric.channel-bulk-recv-threads", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val)) {
				goto Error;
			}
			if (val < 1 || val > MAX_FABRIC_CHANNEL_THREADS) {
				cf_warning(AS_INFO, "fabric.channel-bulk-recv-threads must be between 1 and %u", MAX_FABRIC_CHANNEL_THREADS);
				goto Error;
			}
			cf_info(AS_FABRIC, "changing fabric.channel-bulk-recv-threads from %u to %d", g_config.n_fabric_channel_recv_threads[AS_FABRIC_CHANNEL_BULK], val);
			as_fabric_set_recv_threads(AS_FABRIC_CHANNEL_BULK, val);
		}
		else if (0 == as_info_parameter_get(params, "fabric.channel-ctrl-recv-threads", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val)) {
				goto Error;
			}
			if (val < 1 || val > MAX_FABRIC_CHANNEL_THREADS) {
				cf_warning(AS_INFO, "fabric.channel-ctrl-recv-threads must be between 1 and %u", MAX_FABRIC_CHANNEL_THREADS);
				goto Error;
			}
			cf_info(AS_FABRIC, "changing fabric.channel-ctrl-recv-threads from %u to %d", g_config.n_fabric_channel_recv_threads[AS_FABRIC_CHANNEL_CTRL], val);
			as_fabric_set_recv_threads(AS_FABRIC_CHANNEL_CTRL, val);
		}
		else if (0 == as_info_parameter_get(params, "fabric.channel-meta-recv-threads", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val)) {
				goto Error;
			}
			if (val < 1 || val > MAX_FABRIC_CHANNEL_THREADS) {
				cf_warning(AS_INFO, "fabric.channel-meta-recv-threads must be between 1 and %u", MAX_FABRIC_CHANNEL_THREADS);
				goto Error;
			}
			cf_info(AS_FABRIC, "changing fabric.channel-meta-recv-threads from %u to %d", g_config.n_fabric_channel_recv_threads[AS_FABRIC_CHANNEL_META], val);
			as_fabric_set_recv_threads(AS_FABRIC_CHANNEL_META, val);
		}
		else if (0 == as_info_parameter_get(params, "fabric.channel-rw-recv-threads", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val)) {
				goto Error;
			}
			if (val < 1 || val > MAX_FABRIC_CHANNEL_THREADS) {
				cf_warning(AS_INFO, "fabric.channel-rw-recv-threads must be between 1 and %u", MAX_FABRIC_CHANNEL_THREADS);
				goto Error;
			}
			if (val % g_config.n_fabric_channel_recv_pools[AS_FABRIC_CHANNEL_RW] != 0) {
				cf_warning(AS_INFO, "'fabric.channel-rw-recv-threads' must be a multiple of 'fabric.channel-rw-recv-pools'");
				goto Error;
			}
			cf_info(AS_FABRIC, "changing fabric.channel-rw-recv-threads from %u to %d", g_config.n_fabric_channel_recv_threads[AS_FABRIC_CHANNEL_RW], val);
			as_fabric_set_recv_threads(AS_FABRIC_CHANNEL_RW, val);
		}
		else if (0 == as_info_parameter_get(params, "fabric.recv-rearm-threshold", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val)) {
				goto Error;
			}

			if (val < 0 || val > 1024 * 1024) {
				goto Error;
			}

			g_config.fabric_recv_rearm_threshold = (uint32_t)val;
		}
		else
			goto Error;
	}
	else if (strcmp(context, "namespace") == 0) {
		context_len = sizeof(context);
		if (0 != as_info_parameter_get(params, "id", context, &context_len))
			goto Error;
		as_namespace *ns = as_namespace_get_byname(context);
		if (!ns)
			goto Error;

		context_len = sizeof(context);
		// configure namespace/set related parameters:
		if (0 == as_info_parameter_get(params, "set", context, &context_len)) {
			if (context_len == 0 || context_len >= AS_SET_NAME_MAX_SIZE) {
				cf_warning(AS_INFO, "illegal length %d for set name %s",
						context_len, context);
				goto Error;
			}

			char set_name[AS_SET_NAME_MAX_SIZE];
			size_t set_name_len = (size_t)context_len;

			strcpy(set_name, context);

			// configurations should create set if it doesn't exist.
			// checks if there is a vmap set with the same name and if so returns
			// a ptr to it. if not, it creates an set structure, initializes it
			// and returns a ptr to it.
			as_set *p_set = NULL;
			uint16_t set_id;
			if (as_namespace_get_create_set_w_len(ns, set_name, set_name_len,
					&p_set, &set_id) != 0) {
				goto Error;
			}

			context_len = sizeof(context);

			if (0 == as_info_parameter_get(params, "disable-eviction", context, &context_len)) {
				if ((strncmp(context, "true", 4) == 0) || (strncmp(context, "yes", 3) == 0)) {
					cf_info(AS_INFO, "Changing value of disable-eviction of ns %s set %s to %s", ns->name, p_set->name, context);
					p_set->eviction_disabled = true;
				}
				else if ((strncmp(context, "false", 5) == 0) || (strncmp(context, "no", 2) == 0)) {
					cf_info(AS_INFO, "Changing value of disable-eviction of ns %s set %s to %s", ns->name, p_set->name, context);
					p_set->eviction_disabled = false;
				}
				else {
					goto Error;
				}
			}
			else if (0 == as_info_parameter_get(params, "enable-index", context, &context_len)) {
				if ((strncmp(context, "true", 4) == 0) || (strncmp(context, "yes", 3) == 0)) {
					cf_info(AS_INFO, "Changing value of enable-index of ns %s set %s to %s", ns->name, p_set->name, context);
					as_set_index_enable(ns, p_set, set_id);
				}
				else if ((strncmp(context, "false", 5) == 0) || (strncmp(context, "no", 2) == 0)) {
					cf_info(AS_INFO, "Changing value of enable-index of ns %s set %s to %s", ns->name, p_set->name, context);
					as_set_index_disable(ns, p_set, set_id);
				}
				else {
					goto Error;
				}
			}
			else if (0 == as_info_parameter_get(params, "stop-writes-count", context, &context_len)) {
				uint64_t val = atoll(context);
				cf_info(AS_INFO, "Changing value of stop-writes-count of ns %s set %s to %lu", ns->name, p_set->name, val);
				cf_atomic64_set(&p_set->stop_writes_count, val);
			}
			else {
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "memory-size", context, &context_len)) {
			uint64_t val;

			if (0 != cf_str_atoi_u64(context, &val)) {
				goto Error;
			}
			cf_debug(AS_INFO, "memory-size = %"PRIu64"", val);
			if (val > ns->memory_size)
				ns->memory_size = val;
			if (val < (ns->memory_size / 2L)) { // protect so someone does not reduce memory to below 1/2 current value
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of memory-size of ns %s from %"PRIu64" to %"PRIu64, ns->name, ns->memory_size, val);
			ns->memory_size = val;
		}
		else if (0 == as_info_parameter_get(params, "high-water-disk-pct", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val) || val < 0 || val > 100) {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of high-water-disk-pct of ns %s from %u to %d ", ns->name, ns->hwm_disk_pct, val);
			ns->hwm_disk_pct = (uint32_t)val;
		}
		else if (0 == as_info_parameter_get(params, "high-water-memory-pct", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val) || val < 0 || val > 100) {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of high-water-memory-pct memory of ns %s from %u to %d ", ns->name, ns->hwm_memory_pct, val);
			ns->hwm_memory_pct = (uint32_t)val;
		}
		else if (0 == as_info_parameter_get(params, "evict-tenths-pct", context, &context_len)) {
			cf_info(AS_INFO, "Changing value of evict-tenths-pct memory of ns %s from %d to %d ", ns->name, ns->evict_tenths_pct, atoi(context));
			ns->evict_tenths_pct = atoi(context);
		}
		else if (0 == as_info_parameter_get(params, "evict-hist-buckets", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val) || val < 100 || val > 10000000) {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of evict-hist-buckets of ns %s from %u to %d ", ns->name, ns->evict_hist_buckets, val);
			ns->evict_hist_buckets = (uint32_t)val;
		}
		else if (0 == as_info_parameter_get(params, "background-query-max-rps", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val) || val < 1 || val > 1000000) {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of background-query-max-rps of ns %s from %u to %d ", ns->name, ns->background_query_max_rps, val);
			ns->background_query_max_rps = (uint32_t)val;
		}
		else if (0 == as_info_parameter_get(params, "single-query-threads", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val) || val < 1 || val > 128) {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of single-query-threads of ns %s from %u to %d ", ns->name, ns->n_single_query_threads, val);
			ns->n_single_query_threads = (uint32_t)val;
		}
		else if (0 == as_info_parameter_get(params, "stop-writes-pct", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val) || val < 0 || val > 100) {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of stop-writes-pct memory of ns %s from %u to %d ", ns->name, ns->stop_writes_pct, val);
			ns->stop_writes_pct = (uint32_t)val;
		}
		else if (0 == as_info_parameter_get(params, "default-ttl", context, &context_len)) {
			uint32_t val;
			if (cf_str_atoi_seconds(context, &val) != 0) {
				cf_warning(AS_INFO, "default-ttl must be an unsigned number with time unit (s, m, h, or d)");
				goto Error;
			}
			if (val > MAX_ALLOWED_TTL) {
				cf_warning(AS_INFO, "default-ttl must be <= %u seconds", MAX_ALLOWED_TTL);
				goto Error;
			}
			if (val != 0 && ns->nsup_period == 0 && ! ns->allow_ttl_without_nsup) {
				cf_warning(AS_INFO, "must configure non-zero nsup-period or allow-ttl-without-nsup true to set non-zero default-ttl");
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of default-ttl of ns %s from %u to %u", ns->name, ns->default_ttl, val);
			ns->default_ttl = val;
		}
		else if (0 == as_info_parameter_get(params, "max-record-size", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val) || val < 0) {
				goto Error;
			}
			if (val != 0) {
				if (ns->storage_type == AS_STORAGE_ENGINE_MEMORY && val > 128 * 1024 * 1024) { // PROTO_SIZE_MAX
					cf_warning(AS_INFO, "max-record-size can't be bigger than 128M");
					goto Error;
				}
				if (ns->storage_type == AS_STORAGE_ENGINE_PMEM && val > 8 * 1024 * 1024) { // PMEM_WRITE_BLOCK_SIZE
					cf_warning(AS_INFO, "max-record-size can't be bigger than 8M");
					goto Error;
				}
				if (ns->storage_type == AS_STORAGE_ENGINE_SSD && val > ns->storage_write_block_size) {
					cf_warning(AS_INFO, "max-record-size can't be bigger than write-block-size");
					goto Error;
				}
			}
			cf_info(AS_INFO, "Changing value of max-record-size of ns %s from %u to %d", ns->name, ns->max_record_size, val);
			ns->max_record_size = (uint32_t)val;
		}
		else if (0 == as_info_parameter_get(params, "migrate-order", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val) || val < 1 || val > 10) {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of migrate-order of ns %s from %u to %d", ns->name, ns->migrate_order, val);
			ns->migrate_order = (uint32_t)val;
		}
		else if (0 == as_info_parameter_get(params, "migrate-retransmit-ms", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val)) {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of migrate-retransmit-ms of ns %s from %u to %d", ns->name, ns->migrate_retransmit_ms, val);
			ns->migrate_retransmit_ms = (uint32_t)val;
		}
		else if (0 == as_info_parameter_get(params, "migrate-sleep", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val)) {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of migrate-sleep of ns %s from %u to %d", ns->name, ns->migrate_sleep, val);
			ns->migrate_sleep = (uint32_t)val;
		}
		else if (0 == as_info_parameter_get(params, "nsup-hist-period", context, &context_len)) {
			uint32_t val;
			if (cf_str_atoi_seconds(context, &val) != 0) {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of nsup-hist-period of ns %s from %u to %u", ns->name, ns->nsup_hist_period, val);
			ns->nsup_hist_period = val;
		}
		else if (0 == as_info_parameter_get(params, "nsup-period", context, &context_len)) {
			uint32_t val;
			if (cf_str_atoi_seconds(context, &val) != 0) {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of nsup-period of ns %s from %u to %u", ns->name, ns->nsup_period, val);
			ns->nsup_period = val;
		}
		else if (0 == as_info_parameter_get(params, "nsup-threads", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val) || val < 1 || val > 128) {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of nsup-threads of ns %s from %u to %d", ns->name, ns->n_nsup_threads, val);
			ns->n_nsup_threads = (uint32_t)val;
		}
		else if (0 == as_info_parameter_get(params, "replication-factor", context, &context_len)) {
			if (ns->cp) {
				cf_warning(AS_INFO, "{%s} 'replication-factor' is not yet dynamic with 'strong-consistency'", ns->name);
				goto Error;
			}
			if (0 != cf_str_atoi(context, &val) || val < 1 || val > AS_CLUSTER_SZ) {
				cf_warning(AS_INFO, "replication-factor must be between 1 and %u", AS_CLUSTER_SZ);
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of replication-factor of ns %s from %u to %d", ns->name, ns->cfg_replication_factor, val);
			ns->cfg_replication_factor = (uint32_t)val;
		}
		else if (0 == as_info_parameter_get(params, "conflict-resolve-writes", context, &context_len)) {
			if (as_config_error_enterprise_only()) {
				cf_warning(AS_INFO, "conflict-resolve-writes is enterprise-only");
				goto Error;
			}
			if (ns->single_bin) {
				cf_warning(AS_INFO, "conflict-resolve-writes can't be set for single-bin namespace");
				goto Error;
			}
			if (strncmp(context, "true", 4) == 0 || strncmp(context, "yes", 3) == 0) {
				cf_info(AS_INFO, "Changing value of conflict-resolve-writes of ns %s to %s", ns->name, context);
				ns->conflict_resolve_writes = true;
			}
			else if ((strncmp(context, "false", 5) == 0) || (strncmp(context, "no", 2) == 0)) {
				cf_info(AS_INFO, "Changing value of conflict-resolve-writes of ns %s to %s", ns->name, context);
				ns->conflict_resolve_writes = false;
			}
			else {
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "xdr-bin-tombstone-ttl", context, &context_len)) {
			if (as_config_error_enterprise_only()) {
				cf_warning(AS_INFO, "xdr-bin-tombstone-ttl is enterprise-only");
				goto Error;
			}
			uint32_t val;
			if (cf_str_atoi_seconds(context, &val) != 0) {
				cf_warning(AS_INFO, "xdr-bin-tombstone-ttl must be an unsigned number with time unit (s, m, h, or d)");
				goto Error;
			}
			if (val > MAX_ALLOWED_TTL) {
				cf_warning(AS_INFO, "xdr-bin-tombstone-ttl must be <= %u seconds", MAX_ALLOWED_TTL);
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of xdr-bin-tombstone-ttl of ns %s from %lu to %u", ns->name, ns->xdr_bin_tombstone_ttl_ms / 1000, val);
			ns->xdr_bin_tombstone_ttl_ms = val * 1000UL;
		}
		else if (0 == as_info_parameter_get(params, "xdr-tomb-raider-period", context, &context_len)) {
			if (as_config_error_enterprise_only()) {
				cf_warning(AS_INFO, "xdr-tomb-raider-period is enterprise-only");
				goto Error;
			}
			uint32_t val;
			if (0 != cf_str_atoi_seconds(context, &val)) {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of xdr-tomb-raider-period of ns %s from %u to %u", ns->name, ns->xdr_tomb_raider_period, val);
			ns->xdr_tomb_raider_period = val;
		}
		else if (0 == as_info_parameter_get(params, "xdr-tomb-raider-threads", context, &context_len)) {
			if (as_config_error_enterprise_only()) {
				cf_warning(AS_INFO, "xdr-tomb-raider-threads is enterprise-only");
				goto Error;
			}
			if (0 != cf_str_atoi(context, &val) || val < 1 || val > 128) {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of xdr-tomb-raider-threads of ns %s from %u to %d", ns->name, ns->n_xdr_tomb_raider_threads, val);
			ns->n_xdr_tomb_raider_threads = (uint32_t)val;
		}
		else if (0 == as_info_parameter_get(params, "tomb-raider-eligible-age", context, &context_len)) {
			if (as_config_error_enterprise_only()) {
				cf_warning(AS_INFO, "tomb-raider-eligible-age is enterprise-only");
				goto Error;
			}
			uint32_t val;
			if (cf_str_atoi_seconds(context, &val) != 0) {
				cf_warning(AS_INFO, "tomb-raider-eligible-age must be an unsigned number with time unit (s, m, h, or d)");
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of tomb-raider-eligible-age of ns %s from %u to %u", ns->name, ns->tomb_raider_eligible_age, val);
			ns->tomb_raider_eligible_age = val;
		}
		else if (0 == as_info_parameter_get(params, "tomb-raider-period", context, &context_len)) {
			if (as_config_error_enterprise_only()) {
				cf_warning(AS_INFO, "tomb-raider-period is enterprise-only");
				goto Error;
			}
			uint32_t val;
			if (cf_str_atoi_seconds(context, &val) != 0) {
				cf_warning(AS_INFO, "tomb-raider-period must be an unsigned number with time unit (s, m, h, or d)");
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of tomb-raider-period of ns %s from %u to %u", ns->name, ns->tomb_raider_period, val);
			ns->tomb_raider_period = val;
		}
		else if (0 == as_info_parameter_get(params, "tomb-raider-sleep", context, &context_len)) {
			if (as_config_error_enterprise_only()) {
				cf_warning(AS_INFO, "tomb-raider-sleep is enterprise-only");
				goto Error;
			}
			if (0 != cf_str_atoi(context, &val)) {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of tomb-raider-sleep of ns %s from %u to %d", ns->name, ns->storage_tomb_raider_sleep, val);
			ns->storage_tomb_raider_sleep = (uint32_t)val;
		}
		else if (0 == as_info_parameter_get(params, "transaction-pending-limit", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val)) {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of transaction-pending-limit of ns %s from %d to %d ", ns->name, ns->transaction_pending_limit, val);
			ns->transaction_pending_limit = val;
		}
		else if (0 == as_info_parameter_get(params, "truncate-threads", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val)) {
				goto Error;
			}
			if (val > MAX_TRUNCATE_THREADS || val < 1) {
				cf_warning(AS_INFO, "truncate-threads %d must be >= 1 and <= %u", val, MAX_TRUNCATE_THREADS);
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of truncate-threads of ns %s from %u to %d ", ns->name, ns->n_truncate_threads, val);
			ns->n_truncate_threads = (uint32_t)val;
		}
		else if (0 == as_info_parameter_get(params, "rack-id", context, &context_len)) {
			if (as_config_error_enterprise_only()) {
				cf_warning(AS_INFO, "rack-id is enterprise-only");
				goto Error;
			}
			if (0 != cf_str_atoi(context, &val)) {
				goto Error;
			}
			if ((uint32_t)val > MAX_RACK_ID) {
				cf_warning(AS_INFO, "rack-id %d must be >= 0 and <= %u", val, MAX_RACK_ID);
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of rack-id of ns %s from %u to %d", ns->name, ns->rack_id, val);
			ns->rack_id = (uint32_t)val;
		}
		else if (0 == as_info_parameter_get(params, "conflict-resolution-policy", context, &context_len)) {
			if (ns->cp) {
				cf_warning(AS_INFO, "{%s} 'conflict-resolution-policy' is not applicable with 'strong-consistency'", ns->name);
				goto Error;
			}
			if (strncmp(context, "generation", 10) == 0) {
				cf_info(AS_INFO, "Changing value of conflict-resolution-policy of ns %s from %d to %s", ns->name, ns->conflict_resolution_policy, context);
				ns->conflict_resolution_policy = AS_NAMESPACE_CONFLICT_RESOLUTION_POLICY_GENERATION;
			}
			else if (strncmp(context, "last-update-time", 16) == 0) {
				cf_info(AS_INFO, "Changing value of conflict-resolution-policy of ns %s from %d to %s", ns->name, ns->conflict_resolution_policy, context);
				ns->conflict_resolution_policy = AS_NAMESPACE_CONFLICT_RESOLUTION_POLICY_LAST_UPDATE_TIME;
			}
			else {
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "mounts-high-water-pct", context, &context_len)) {
			if (! as_namespace_index_persisted(ns)) {
				cf_warning(AS_INFO, "mounts-high-water-pct is not relevant for this index-type");
				goto Error;
			}

			if (0 != cf_str_atoi(context, &val) || val < 0 || val > 100) {
				goto Error;
			}

			cf_info(AS_INFO, "Changing value of mounts-high-water-pct of ns %s from %u to %d ", ns->name, ns->mounts_hwm_pct, val);
			ns->mounts_hwm_pct = (uint32_t)val;
		}
		else if (0 == as_info_parameter_get(params, "mounts-size-limit", context, &context_len)) {
			if (! as_namespace_index_persisted(ns)) {
				cf_warning(AS_INFO, "mounts-size-limit is not relevant for this index-type");
				goto Error;
			}

			uint64_t val;
			uint64_t min = (ns->xmem_type == CF_XMEM_TYPE_FLASH ? 4 : 1) * 1024UL * 1024UL *1024UL;

			if (0 != cf_str_atoi_u64(context, &val) || val < min) {
				goto Error;
			}

			cf_info(AS_INFO, "Changing value of mounts-size-limit of ns %s from %"PRIu64" to %"PRIu64, ns->name, ns->mounts_size_limit, val);
			ns->mounts_size_limit = val;
		}
		else if (0 == as_info_parameter_get(params, "compression", context, &context_len)) {
			if (as_config_error_enterprise_only()) {
				cf_warning(AS_INFO, "compression is enterprise-only");
				goto Error;
			}
			if (as_config_error_enterprise_feature_only("compression")) {
				cf_warning(AS_INFO, "{%s} feature key does not allow compression", ns->name);
				goto Error;
			}
			if (ns->storage_type == AS_STORAGE_ENGINE_MEMORY) {
				// Note - harmful to configure compression for memory-only!
				cf_warning(AS_INFO, "{%s} compression is not available for storage-engine memory", ns->name);
				goto Error;
			}
			const char* orig = NS_COMPRESSION();
			if (strcmp(context, "none") == 0) {
				ns->storage_compression = AS_COMPRESSION_NONE;
			}
			else if (strcmp(context, "lz4") == 0) {
				ns->storage_compression = AS_COMPRESSION_LZ4;
			}
			else if (strcmp(context, "snappy") == 0) {
				ns->storage_compression = AS_COMPRESSION_SNAPPY;
			}
			else if (strcmp(context, "zstd") == 0) {
				ns->storage_compression = AS_COMPRESSION_ZSTD;
			}
			else {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of compression of ns %s from %s to %s", ns->name, orig, context);
		}
		else if (0 == as_info_parameter_get(params, "compression-level", context, &context_len)) {
			if (as_config_error_enterprise_only()) {
				cf_warning(AS_INFO, "compression-level is enterprise-only");
				goto Error;
			}
			if (0 != cf_str_atoi(context, &val) || val < 1 || val > 9) {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of compression-level of ns %s from %u to %d", ns->name, ns->storage_compression_level, val);
			ns->storage_compression_level = (uint32_t)val;
		}
		else if (0 == as_info_parameter_get(params, "cache-replica-writes", context, &context_len)) {
			if (ns->storage_data_in_memory) {
				cf_warning(AS_INFO, "ns %s, can't set cache-replica-writes if data-in-memory", ns->name);
				goto Error;
			}
			if (strncmp(context, "true", 4) == 0 || strncmp(context, "yes", 3) == 0) {
				cf_info(AS_INFO, "Changing value of cache-replica-writes of ns %s to %s", ns->name, context);
				ns->storage_cache_replica_writes = true;
			}
			else if ((strncmp(context, "false", 5) == 0) || (strncmp(context, "no", 2) == 0)) {
				cf_info(AS_INFO, "Changing value of cache-replica-writes of ns %s to %s", ns->name, context);
				ns->storage_cache_replica_writes = false;
			}
			else {
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "defrag-lwm-pct", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val)) {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of defrag-lwm-pct of ns %s from %d to %d ", ns->name, ns->storage_defrag_lwm_pct, val);

			uint32_t old_val = ns->storage_defrag_lwm_pct;

			ns->storage_defrag_lwm_pct = val;
			ns->defrag_lwm_size = (ns->storage_write_block_size * ns->storage_defrag_lwm_pct) / 100;

			if (ns->storage_defrag_lwm_pct > old_val) {
				as_storage_defrag_sweep(ns);
			}
		}
		else if (0 == as_info_parameter_get(params, "defrag-queue-min", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val)) {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of defrag-queue-min of ns %s from %u to %d", ns->name, ns->storage_defrag_queue_min, val);
			ns->storage_defrag_queue_min = (uint32_t)val;
		}
		else if (0 == as_info_parameter_get(params, "defrag-sleep", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val)) {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of defrag-sleep of ns %s from %u to %d", ns->name, ns->storage_defrag_sleep, val);
			ns->storage_defrag_sleep = (uint32_t)val;
		}
		else if (0 == as_info_parameter_get(params, "flush-max-ms", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val)) {
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of flush-max-ms of ns %s from %lu to %d", ns->name, ns->storage_flush_max_us / 1000, val);
			ns->storage_flush_max_us = (uint64_t)val * 1000;
		}
		else if (0 == as_info_parameter_get(params, "reject-non-xdr-writes", context, &context_len)) {
			if (strncmp(context, "true", 4) == 0 || strncmp(context, "yes", 3) == 0) {
				cf_info(AS_INFO, "Changing value of reject-non-xdr-writes of ns %s from %s to %s", ns->name, bool_val[ns->reject_non_xdr_writes], context);
				ns->reject_non_xdr_writes = true;
			}
			else if (strncmp(context, "false", 5) == 0 || strncmp(context, "no", 2) == 0) {
				cf_info(AS_INFO, "Changing value of reject-non-xdr-writes of ns %s from %s to %s", ns->name, bool_val[ns->reject_non_xdr_writes], context);
				ns->reject_non_xdr_writes = false;
			}
			else {
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "reject-xdr-writes", context, &context_len)) {
			if (strncmp(context, "true", 4) == 0 || strncmp(context, "yes", 3) == 0) {
				cf_info(AS_INFO, "Changing value of reject-xdr-writes of ns %s from %s to %s", ns->name, bool_val[ns->reject_xdr_writes], context);
				ns->reject_xdr_writes = true;
			}
			else if (strncmp(context, "false", 5) == 0 || strncmp(context, "no", 2) == 0) {
				cf_info(AS_INFO, "Changing value of reject-xdr-writes of ns %s from %s to %s", ns->name, bool_val[ns->reject_xdr_writes], context);
				ns->reject_xdr_writes = false;
			}
			else {
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "allow-ttl-without-nsup", context, &context_len)) {
			if (strncmp(context, "true", 4) == 0 || strncmp(context, "yes", 3) == 0) {
				cf_info(AS_INFO, "Changing value of allow-ttl-without-nsup of ns %s from %s to %s", ns->name, bool_val[ns->allow_ttl_without_nsup], context);
				ns->allow_ttl_without_nsup = true;
			}
			else if (strncmp(context, "false", 5) == 0 || strncmp(context, "no", 2) == 0) {
				cf_info(AS_INFO, "Changing value of allow-ttl-without-nsup of ns %s from %s to %s", ns->name, bool_val[ns->allow_ttl_without_nsup], context);
				ns->allow_ttl_without_nsup = false;
			}
			else {
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "ignore-migrate-fill-delay", context, &context_len)) {
			if (as_config_error_enterprise_only()) {
				cf_warning(AS_INFO, "ignore-migrate-fill-delay is enterprise-only");
				goto Error;
			}
			if (strncmp(context, "true", 4) == 0 || strncmp(context, "yes", 3) == 0) {
				cf_info(AS_INFO, "Changing value of ignore-migrate-fill-delay of ns %s to %s", ns->name, context);
				ns->ignore_migrate_fill_delay = true;
			}
			else if ((strncmp(context, "false", 5) == 0) || (strncmp(context, "no", 2) == 0)) {
				cf_info(AS_INFO, "Changing value of ignore-migrate-fill-delay of ns %s to %s", ns->name, context);
				ns->ignore_migrate_fill_delay = false;
			}
			else {
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "strong-consistency-allow-expunge", context, &context_len)) {
			if (! ns->cp) {
				cf_warning(AS_INFO, "{%s} 'strong-consistency-allow-expunge' is only applicable with 'strong-consistency'", ns->name);
				goto Error;
			}
			if (strncmp(context, "true", 4) == 0 || strncmp(context, "yes", 3) == 0) {
				cf_info(AS_INFO, "Changing value of strong-consistency-allow-expunge of ns %s from %s to %s", ns->name, bool_val[ns->cp_allow_drops], context);
				ns->cp_allow_drops = true;
			}
			else if (strncmp(context, "false", 5) == 0 || strncmp(context, "no", 2) == 0) {
				cf_info(AS_INFO, "Changing value of strong-consistency-allow-expunge of ns %s from %s to %s", ns->name, bool_val[ns->cp_allow_drops], context);
				ns->cp_allow_drops = false;
			}
			else {
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "disable-write-dup-res", context, &context_len)) {
			if (ns->cp) {
				cf_warning(AS_INFO, "{%s} 'disable-write-dup-res' is not applicable with 'strong-consistency'", ns->name);
				goto Error;
			}
			if (strncmp(context, "true", 4) == 0 || strncmp(context, "yes", 3) == 0) {
				cf_info(AS_INFO, "Changing value of disable-write-dup-res of ns %s from %s to %s", ns->name, bool_val[ns->write_dup_res_disabled], context);
				ns->write_dup_res_disabled = true;
			}
			else if (strncmp(context, "false", 5) == 0 || strncmp(context, "no", 2) == 0) {
				cf_info(AS_INFO, "Changing value of disable-write-dup-res of ns %s from %s to %s", ns->name, bool_val[ns->write_dup_res_disabled], context);
				ns->write_dup_res_disabled = false;
			}
			else {
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "disallow-null-setname", context, &context_len)) {
			if (strncmp(context, "true", 4) == 0 || strncmp(context, "yes", 3) == 0) {
				cf_info(AS_INFO, "Changing value of disallow-null-setname of ns %s from %s to %s", ns->name, bool_val[ns->disallow_null_setname], context);
				ns->disallow_null_setname = true;
			}
			else if (strncmp(context, "false", 5) == 0 || strncmp(context, "no", 2) == 0) {
				cf_info(AS_INFO, "Changing value of disallow-null-setname of ns %s from %s to %s", ns->name, bool_val[ns->disallow_null_setname], context);
				ns->disallow_null_setname = false;
			}
			else {
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "enable-benchmarks-batch-sub", context, &context_len)) {
			if (strncmp(context, "true", 4) == 0 || strncmp(context, "yes", 3) == 0) {
				cf_info(AS_INFO, "Changing value of enable-benchmarks-batch-sub of ns %s from %s to %s", ns->name, bool_val[ns->batch_sub_benchmarks_enabled], context);
				if (! ns->batch_sub_benchmarks_enabled) {
					batch_sub_benchmarks_histogram_clear_all(ns);
				}
				ns->batch_sub_benchmarks_enabled = true;
			}
			else if (strncmp(context, "false", 5) == 0 || strncmp(context, "no", 2) == 0) {
				cf_info(AS_INFO, "Changing value of enable-benchmarks-batch-sub of ns %s from %s to %s", ns->name, bool_val[ns->batch_sub_benchmarks_enabled], context);
				ns->batch_sub_benchmarks_enabled = false;
				batch_sub_benchmarks_histogram_clear_all(ns);
			}
			else {
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "enable-benchmarks-ops-sub", context, &context_len)) {
			if (strncmp(context, "true", 4) == 0 || strncmp(context, "yes", 3) == 0) {
				cf_info(AS_INFO, "Changing value of enable-benchmarks-ops-sub of ns %s from %s to %s", ns->name, bool_val[ns->ops_sub_benchmarks_enabled], context);
				if (! ns->ops_sub_benchmarks_enabled) {
					ops_sub_benchmarks_histogram_clear_all(ns);
				}
				ns->ops_sub_benchmarks_enabled = true;
			}
			else if (strncmp(context, "false", 5) == 0 || strncmp(context, "no", 2) == 0) {
				cf_info(AS_INFO, "Changing value of enable-benchmarks-ops-sub of ns %s from %s to %s", ns->name, bool_val[ns->ops_sub_benchmarks_enabled], context);
				ns->ops_sub_benchmarks_enabled = false;
				ops_sub_benchmarks_histogram_clear_all(ns);
			}
			else {
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "enable-benchmarks-read", context, &context_len)) {
			if (strncmp(context, "true", 4) == 0 || strncmp(context, "yes", 3) == 0) {
				cf_info(AS_INFO, "Changing value of enable-benchmarks-read of ns %s from %s to %s", ns->name, bool_val[ns->read_benchmarks_enabled], context);
				if (! ns->read_benchmarks_enabled) {
					read_benchmarks_histogram_clear_all(ns);
				}
				ns->read_benchmarks_enabled = true;
			}
			else if (strncmp(context, "false", 5) == 0 || strncmp(context, "no", 2) == 0) {
				cf_info(AS_INFO, "Changing value of enable-benchmarks-read of ns %s from %s to %s", ns->name, bool_val[ns->read_benchmarks_enabled], context);
				ns->read_benchmarks_enabled = false;
				read_benchmarks_histogram_clear_all(ns);
			}
			else {
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "enable-benchmarks-storage", context, &context_len)) {
			if (strncmp(context, "true", 4) == 0 || strncmp(context, "yes", 3) == 0) {
				cf_info(AS_INFO, "Changing value of enable-benchmarks-storage of ns %s from %s to %s", ns->name, bool_val[ns->storage_benchmarks_enabled], context);
				if (! ns->storage_benchmarks_enabled) {
					as_storage_histogram_clear_all(ns);
				}
				ns->storage_benchmarks_enabled = true;
			}
			else if (strncmp(context, "false", 5) == 0 || strncmp(context, "no", 2) == 0) {
				cf_info(AS_INFO, "Changing value of enable-benchmarks-storage of ns %s from %s to %s", ns->name, bool_val[ns->storage_benchmarks_enabled], context);
				ns->storage_benchmarks_enabled = false;
				as_storage_histogram_clear_all(ns);
			}
			else {
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "enable-benchmarks-udf", context, &context_len)) {
			if (strncmp(context, "true", 4) == 0 || strncmp(context, "yes", 3) == 0) {
				cf_info(AS_INFO, "Changing value of enable-benchmarks-udf of ns %s from %s to %s", ns->name, bool_val[ns->udf_benchmarks_enabled], context);
				if (! ns->udf_benchmarks_enabled) {
					udf_benchmarks_histogram_clear_all(ns);
				}
				ns->udf_benchmarks_enabled = true;
			}
			else if (strncmp(context, "false", 5) == 0 || strncmp(context, "no", 2) == 0) {
				cf_info(AS_INFO, "Changing value of enable-benchmarks-udf of ns %s from %s to %s", ns->name, bool_val[ns->udf_benchmarks_enabled], context);
				ns->udf_benchmarks_enabled = false;
				udf_benchmarks_histogram_clear_all(ns);
			}
			else {
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "enable-benchmarks-udf-sub", context, &context_len)) {
			if (strncmp(context, "true", 4) == 0 || strncmp(context, "yes", 3) == 0) {
				cf_info(AS_INFO, "Changing value of enable-benchmarks-udf-sub of ns %s from %s to %s", ns->name, bool_val[ns->udf_sub_benchmarks_enabled], context);
				if (! ns->udf_sub_benchmarks_enabled) {
					udf_sub_benchmarks_histogram_clear_all(ns);
				}
				ns->udf_sub_benchmarks_enabled = true;
			}
			else if (strncmp(context, "false", 5) == 0 || strncmp(context, "no", 2) == 0) {
				cf_info(AS_INFO, "Changing value of enable-benchmarks-udf-sub of ns %s from %s to %s", ns->name, bool_val[ns->udf_sub_benchmarks_enabled], context);
				ns->udf_sub_benchmarks_enabled = false;
				udf_sub_benchmarks_histogram_clear_all(ns);
			}
			else {
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "enable-benchmarks-write", context, &context_len)) {
			if (strncmp(context, "true", 4) == 0 || strncmp(context, "yes", 3) == 0) {
				cf_info(AS_INFO, "Changing value of enable-benchmarks-write of ns %s from %s to %s", ns->name, bool_val[ns->write_benchmarks_enabled], context);
				if (! ns->write_benchmarks_enabled) {
					write_benchmarks_histogram_clear_all(ns);
				}
				ns->write_benchmarks_enabled = true;
			}
			else if (strncmp(context, "false", 5) == 0 || strncmp(context, "no", 2) == 0) {
				cf_info(AS_INFO, "Changing value of enable-benchmarks-write of ns %s from %s to %s", ns->name, bool_val[ns->write_benchmarks_enabled], context);
				ns->write_benchmarks_enabled = false;
				write_benchmarks_histogram_clear_all(ns);
			}
			else {
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "enable-hist-proxy", context, &context_len)) {
			if (strncmp(context, "true", 4) == 0 || strncmp(context, "yes", 3) == 0) {
				cf_info(AS_INFO, "Changing value of enable-hist-proxy of ns %s from %s to %s", ns->name, bool_val[ns->proxy_hist_enabled], context);
				if (! ns->proxy_hist_enabled) {
					histogram_clear(ns->proxy_hist);
				}
				ns->proxy_hist_enabled = true;
			}
			else if (strncmp(context, "false", 5) == 0 || strncmp(context, "no", 2) == 0) {
				cf_info(AS_INFO, "Changing value of enable-hist-proxy of ns %s from %s to %s", ns->name, bool_val[ns->proxy_hist_enabled], context);
				ns->proxy_hist_enabled = false;
				histogram_clear(ns->proxy_hist);
			}
			else {
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "read-page-cache", context, &context_len)) {
			if (strncmp(context, "true", 4) == 0 || strncmp(context, "yes", 3) == 0) {
				cf_info(AS_INFO, "Changing value of read-page-cache of ns %s from %s to %s", ns->name, bool_val[ns->storage_read_page_cache], context);
				ns->storage_read_page_cache = true;
			}
			else if (strncmp(context, "false", 5) == 0 || strncmp(context, "no", 2) == 0) {
				cf_info(AS_INFO, "Changing value of read-page-cache of ns %s from %s to %s", ns->name, bool_val[ns->storage_read_page_cache], context);
				ns->storage_read_page_cache = false;
			}
			else {
				goto Error;
			}
		}
		else if (0 == as_info_parameter_get(params, "max-write-cache", context, &context_len)) {
			uint64_t val_u64;

			if (0 != cf_str_atoi_u64(context, &val_u64)) {
				goto Error;
			}
			if (val_u64 < DEFAULT_MAX_WRITE_CACHE) {
				cf_warning(AS_INFO, "can't set max-write-cache < %luM", DEFAULT_MAX_WRITE_CACHE / (1024 * 1024));
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of max-write-cache of ns %s from %lu to %lu ", ns->name, ns->storage_max_write_cache, val_u64);
			ns->storage_max_write_cache = val_u64;
			ns->storage_max_write_q = (uint32_t)(as_namespace_device_count(ns) *
					ns->storage_max_write_cache / ns->storage_write_block_size);
		}
		else if (0 == as_info_parameter_get(params, "min-avail-pct", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val)) {
				cf_warning(AS_INFO, "ns %s, min-avail-pct %s is not a number", ns->name, context);
				goto Error;
			}
			if (val > 100 || val < 0) {
				cf_warning(AS_INFO, "ns %s, min-avail-pct %d must be between 0 and 100", ns->name, val);
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of min-avail-pct of ns %s from %u to %d ", ns->name, ns->storage_min_avail_pct, val);
			ns->storage_min_avail_pct = (uint32_t)val;
		}
		else if (0 == as_info_parameter_get(params, "post-write-queue", context, &context_len)) {
			if (ns->storage_data_in_memory) {
				cf_warning(AS_INFO, "ns %s, can't set post-write-queue if data-in-memory", ns->name);
				goto Error;
			}
			if (0 != cf_str_atoi(context, &val)) {
				cf_warning(AS_INFO, "ns %s, post-write-queue %s is not a number", ns->name, context);
				goto Error;
			}
			if ((uint32_t)val > MAX_POST_WRITE_QUEUE) {
				cf_warning(AS_INFO, "ns %s, post-write-queue %u must be < %u", ns->name, val, MAX_POST_WRITE_QUEUE);
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of post-write-queue of ns %s from %d to %d ", ns->name, ns->storage_post_write_queue, val);
			cf_atomic32_set(&ns->storage_post_write_queue, (uint32_t)val);
		}
		else if (0 == as_info_parameter_get(params, "read-consistency-level-override", context, &context_len)) {
			if (ns->cp) {
				cf_warning(AS_INFO, "{%s} 'read-consistency-level-override' is not applicable with 'strong-consistency'", ns->name);
				goto Error;
			}
			char *original_value = NS_READ_CONSISTENCY_LEVEL_NAME();
			if (strcmp(context, "all") == 0) {
				ns->read_consistency_level = AS_READ_CONSISTENCY_LEVEL_ALL;
			}
			else if (strcmp(context, "off") == 0) {
				ns->read_consistency_level = AS_READ_CONSISTENCY_LEVEL_PROTO;
			}
			else if (strcmp(context, "one") == 0) {
				ns->read_consistency_level = AS_READ_CONSISTENCY_LEVEL_ONE;
			}
			else {
				goto Error;
			}
			if (strcmp(original_value, context)) {
				cf_info(AS_INFO, "Changing value of read-consistency-level-override of ns %s from %s to %s", ns->name, original_value, context);
			}
		}
		else if (0 == as_info_parameter_get(params, "write-commit-level-override", context, &context_len)) {
			if (ns->cp) {
				cf_warning(AS_INFO, "{%s} 'write-commit-level-override' is not applicable with 'strong-consistency'", ns->name);
				goto Error;
			}
			char *original_value = NS_WRITE_COMMIT_LEVEL_NAME();
			if (strcmp(context, "all") == 0) {
				ns->write_commit_level = AS_WRITE_COMMIT_LEVEL_ALL;
			}
			else if (strcmp(context, "master") == 0) {
				ns->write_commit_level = AS_WRITE_COMMIT_LEVEL_MASTER;
			}
			else if (strcmp(context, "off") == 0) {
				ns->write_commit_level = AS_WRITE_COMMIT_LEVEL_PROTO;
			}
			else {
				goto Error;
			}
			if (strcmp(original_value, context)) {
				cf_info(AS_INFO, "Changing value of write-commit-level-override of ns %s from %s to %s", ns->name, original_value, context);
			}
		}
		else if (0 == as_info_parameter_get(params, "geo2dsphere-within-min-level", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val)) {
				cf_warning(AS_INFO, "ns %s, geo2dsphere-within-min-level %s is not a number", ns->name, context);
				goto Error;
			}
			if (val < 0 || val > MAX_REGION_LEVELS) {
				cf_warning(AS_INFO, "ns %s, geo2dsphere-within-min-level %d must be between %u and %u",
						ns->name, val, 0, MAX_REGION_LEVELS);
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of geo2dsphere-within-min-level of ns %s from %u to %d ",
					ns->name, ns->geo2dsphere_within_min_level, val);
			ns->geo2dsphere_within_min_level = val;
		}
		else if (0 == as_info_parameter_get(params, "geo2dsphere-within-max-level", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val)) {
				cf_warning(AS_INFO, "ns %s, geo2dsphere-within-max-level %s is not a number", ns->name, context);
				goto Error;
			}
			if (val < 0 || val > MAX_REGION_LEVELS) {
				cf_warning(AS_INFO, "ns %s, geo2dsphere-within-max-level %d must be between %u and %u",
						ns->name, val, 0, MAX_REGION_LEVELS);
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of geo2dsphere-within-max-level of ns %s from %u to %d ",
					ns->name, ns->geo2dsphere_within_max_level, val);
			ns->geo2dsphere_within_max_level = val;
		}
		else if (0 == as_info_parameter_get(params, "geo2dsphere-within-max-cells", context, &context_len)) {
			if (0 != cf_str_atoi(context, &val)) {
				cf_warning(AS_INFO, "ns %s, geo2dsphere-within-max-cells %s is not a number", ns->name, context);
				goto Error;
			}
			if (val < 1 || val > MAX_REGION_CELLS) {
				cf_warning(AS_INFO, "ns %s, geo2dsphere-within-max-cells %d must be between %u and %u",
						ns->name, val, 1, MAX_REGION_CELLS);
				goto Error;
			}
			cf_info(AS_INFO, "Changing value of geo2dsphere-within-max-cells of ns %s from %u to %d ",
					ns->name, ns->geo2dsphere_within_max_cells, val);
			ns->geo2dsphere_within_max_cells = val;
		}
		else if (0 == as_info_parameter_get(params, "prefer-uniform-balance", context, &context_len)) {
			if (as_config_error_enterprise_only()) {
				cf_warning(AS_INFO, "prefer-uniform-balance is enterprise-only");
				goto Error;
			}
			if (strncmp(context, "true", 4) == 0 || strncmp(context, "yes", 3) == 0) {
				cf_info(AS_INFO, "Changing value of prefer-uniform-balance of ns %s from %s to %s", ns->name, bool_val[ns->cfg_prefer_uniform_balance], context);
				ns->cfg_prefer_uniform_balance = true;
			}
			else if (strncmp(context, "false", 5) == 0 || strncmp(context, "no", 2) == 0) {
				cf_info(AS_INFO, "Changing value of prefer-uniform-balance of ns %s from %s to %s", ns->name, bool_val[ns->cfg_prefer_uniform_balance], context);
				ns->cfg_prefer_uniform_balance = false;
			}
			else {
				goto Error;
			}
		}
		else {
			goto Error;
		}
	} // end of namespace stanza
	else if (strcmp(context, "security") == 0) {
		if (as_config_error_enterprise_only()) {
			cf_warning(AS_INFO, "security is enterprise-only");
			goto Error;
		}

		if (! as_security_set_config(params)) {
			goto Error;
		}
	}
	else if (strcmp(context, "xdr") == 0) {
		if (as_config_error_enterprise_only()) {
			cf_warning(AS_INFO, "XDR is enterprise-only");
			goto Error;
		}

		if (! as_xdr_set_config(params)) {
			goto Error;
		}
	}
	else
		goto Error;

	cf_info(AS_INFO, "config-set command completed: params %s",params);
	cf_dyn_buf_append_string(db, "ok");
	return(0);

Error:
	cf_dyn_buf_append_string(db, "error");
	return(0);
}

// Protect all set-config commands from concurrency issues.
static cf_mutex g_set_cfg_lock = CF_MUTEX_INIT;

int
info_command_config_set(char *name, char *params, cf_dyn_buf *db)
{
	cf_mutex_lock(&g_set_cfg_lock);

	int result = info_command_config_set_threadsafe(name, params, db);

	cf_mutex_unlock(&g_set_cfg_lock);

	return result;
}

// log-set:id=<id>;<context>=<level>
// e.g., log-set:id=0;service=detail
int
info_command_log_set(char *name, char *params, cf_dyn_buf *db)
{
	cf_debug(AS_INFO, "received log-set:%s", params);

	char* save_ptr = NULL;
	const char* tok = strtok_r(params, "=", &save_ptr);

	if (tok == NULL || strcmp(tok, "id") != 0) {
		cf_warning(AS_INFO, "log-set: missing id");
		cf_dyn_buf_append_string(db, "ERROR::missing-id");
		return 0;
	}

	const char* id_str = strtok_r(NULL, ";", &save_ptr);
	uint32_t id;

	if (id_str == NULL || cf_strtoul_u32(id_str, &id) != 0) {
		cf_warning(AS_INFO, "log-set: bad id");
		cf_dyn_buf_append_string(db, "ERROR::bad-id");
		return 0;
	}

	const char* context_str = strtok_r(NULL, "=", &save_ptr);

	if (context_str == NULL) {
		cf_warning(AS_INFO, "log-set: missing context");
		cf_dyn_buf_append_string(db, "ERROR::missing-context");
		return 0;
	}

	const char* level_str = strtok_r(NULL, ";", &save_ptr);

	if (level_str == NULL) {
		cf_warning(AS_INFO, "log-set: bad level");
		cf_dyn_buf_append_string(db, "ERROR::bad-level");
		return 0;
	}

	if (! cf_log_set_level(id, context_str, level_str)) {
		cf_dyn_buf_append_string(db, "ERROR::bad-parameter");
		return 0;
	}

	cf_info(AS_INFO, "log-set:id=%s:%s=%s", id_str, context_str, level_str);
	cf_dyn_buf_append_string(db, "ok");

	return 0;
}


// latencies:[hist=<name>]
//
// If no hist param, command applies to ?
//
// e.g.:
// latencies:hist={test}-reads
// output:
// {test}-reads:msec,30618.2,0.05,0.01,0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00
//
// explanation:
// <name>,units,TPS, ...
// Values following the TPS are percentages exceeding logarithmic thresholds.
//
int
info_command_latencies(char* name, char* params, cf_dyn_buf* db)
{
	cf_debug(AS_INFO, "%s command received: params %s", name, params);

	char value_str[100];
	int  value_str_len = sizeof(value_str);

	if (as_info_parameter_get(params, "hist", value_str, &value_str_len) != 0) {
		// Canonical histograms.

		histogram_get_latencies(g_stats.batch_index_hist, db);

		for (uint32_t i = 0; i < g_config.n_namespaces; i++) {
			as_namespace* ns = g_config.namespaces[i];

			histogram_get_latencies(ns->read_hist, db);
			histogram_get_latencies(ns->write_hist, db);
			histogram_get_latencies(ns->udf_hist, db);
			histogram_get_latencies(ns->pi_query_hist, db);
			histogram_get_latencies(ns->si_query_hist, db);
		}
	}
	else {
		// Named histograms.

		if (strcmp(value_str, "batch-index") == 0) {
			histogram_get_latencies(g_stats.batch_index_hist, db);
		}
		else if (strcmp(value_str, "info") == 0) {
			histogram_get_latencies(g_stats.info_hist, db);
		}
		else if (strcmp(value_str, "benchmarks-fabric") == 0) {
			histogram_get_latencies(g_stats.fabric_send_init_hists[AS_FABRIC_CHANNEL_BULK], db);
			histogram_get_latencies(g_stats.fabric_send_fragment_hists[AS_FABRIC_CHANNEL_BULK], db);
			histogram_get_latencies(g_stats.fabric_recv_fragment_hists[AS_FABRIC_CHANNEL_BULK], db);
			histogram_get_latencies(g_stats.fabric_recv_cb_hists[AS_FABRIC_CHANNEL_BULK], db);
			histogram_get_latencies(g_stats.fabric_send_init_hists[AS_FABRIC_CHANNEL_CTRL], db);
			histogram_get_latencies(g_stats.fabric_send_fragment_hists[AS_FABRIC_CHANNEL_CTRL], db);
			histogram_get_latencies(g_stats.fabric_recv_fragment_hists[AS_FABRIC_CHANNEL_CTRL], db);
			histogram_get_latencies(g_stats.fabric_recv_cb_hists[AS_FABRIC_CHANNEL_CTRL], db);
			histogram_get_latencies(g_stats.fabric_send_init_hists[AS_FABRIC_CHANNEL_META], db);
			histogram_get_latencies(g_stats.fabric_send_fragment_hists[AS_FABRIC_CHANNEL_META], db);
			histogram_get_latencies(g_stats.fabric_recv_fragment_hists[AS_FABRIC_CHANNEL_META], db);
			histogram_get_latencies(g_stats.fabric_recv_cb_hists[AS_FABRIC_CHANNEL_META], db);
			histogram_get_latencies(g_stats.fabric_send_init_hists[AS_FABRIC_CHANNEL_RW], db);
			histogram_get_latencies(g_stats.fabric_send_fragment_hists[AS_FABRIC_CHANNEL_RW], db);
			histogram_get_latencies(g_stats.fabric_recv_fragment_hists[AS_FABRIC_CHANNEL_RW], db);
			histogram_get_latencies(g_stats.fabric_recv_cb_hists[AS_FABRIC_CHANNEL_RW], db);
		}
		else if (*value_str == '{') {
			// Named namespace-scoped histogram - parse '{namespace}-' prefix.

			char* ns_name = value_str + 1;
			char* ns_name_end = strchr(ns_name, '}');
			as_namespace* ns = as_namespace_get_bybuf((uint8_t*)ns_name, ns_name_end - ns_name);

			if (ns == NULL) {
				cf_info(AS_INFO, "%s command: unrecognized histogram: %s", name, value_str);
				cf_dyn_buf_append_string(db, "error-bad-hist-name");
				return 0;
			}

			char* hist_name = ns_name_end + 1;

			if (*hist_name++ != '-') {
				cf_info(AS_INFO, "%s command: unrecognized histogram: %s", name, value_str);
				cf_dyn_buf_append_string(db, "error-bad-hist-name");
				return 0;
			}

			if (strcmp(hist_name, "read") == 0) {
				histogram_get_latencies(ns->read_hist, db);
			}
			else if (strcmp(hist_name, "write") == 0) {
				histogram_get_latencies(ns->write_hist, db);
			}
			else if (strcmp(hist_name, "udf") == 0) {
				histogram_get_latencies(ns->udf_hist, db);
			}
			else if (strcmp(hist_name, "pi-query") == 0) {
				histogram_get_latencies(ns->pi_query_hist, db);
			}
			else if (strcmp(hist_name, "si-query") == 0) {
				histogram_get_latencies(ns->si_query_hist, db);
			}
			else if (strcmp(hist_name, "re-repl") == 0) {
				histogram_get_latencies(ns->re_repl_hist, db);
			}
			else if (strcmp(hist_name, "proxy") == 0) {
				histogram_get_latencies(ns->proxy_hist, db);
			}
			else if (strcmp(hist_name, "benchmarks-read") == 0) {
				histogram_get_latencies(ns->read_start_hist, db);
				histogram_get_latencies(ns->read_restart_hist, db);
				histogram_get_latencies(ns->read_dup_res_hist, db);
				histogram_get_latencies(ns->read_repl_ping_hist, db);
				histogram_get_latencies(ns->read_local_hist, db);
				histogram_get_latencies(ns->read_response_hist, db);
			}
			else if (strcmp(hist_name, "benchmarks-write") == 0) {
				histogram_get_latencies(ns->write_start_hist, db);
				histogram_get_latencies(ns->write_restart_hist, db);
				histogram_get_latencies(ns->write_dup_res_hist, db);
				histogram_get_latencies(ns->write_master_hist, db);
				histogram_get_latencies(ns->write_repl_write_hist, db);
				histogram_get_latencies(ns->write_response_hist, db);
			}
			else if (strcmp(hist_name, "benchmarks-udf") == 0) {
				histogram_get_latencies(ns->udf_start_hist, db);
				histogram_get_latencies(ns->udf_restart_hist, db);
				histogram_get_latencies(ns->udf_dup_res_hist, db);
				histogram_get_latencies(ns->udf_master_hist, db);
				histogram_get_latencies(ns->udf_repl_write_hist, db);
				histogram_get_latencies(ns->udf_response_hist, db);
			}
			else if (strcmp(hist_name, "benchmarks-batch-sub") == 0) {
				histogram_get_latencies(ns->batch_sub_prestart_hist, db);
				histogram_get_latencies(ns->batch_sub_start_hist, db);
				histogram_get_latencies(ns->batch_sub_restart_hist, db);
				histogram_get_latencies(ns->batch_sub_dup_res_hist, db);
				histogram_get_latencies(ns->batch_sub_repl_ping_hist, db);
				histogram_get_latencies(ns->batch_sub_read_local_hist, db);
				histogram_get_latencies(ns->batch_sub_write_master_hist, db);
				histogram_get_latencies(ns->batch_sub_udf_master_hist, db);
				histogram_get_latencies(ns->batch_sub_repl_write_hist, db);
				histogram_get_latencies(ns->batch_sub_response_hist, db);
			}
			else if (strcmp(hist_name, "benchmarks-udf-sub") == 0) {
				histogram_get_latencies(ns->udf_sub_start_hist, db);
				histogram_get_latencies(ns->udf_sub_restart_hist, db);
				histogram_get_latencies(ns->udf_sub_dup_res_hist, db);
				histogram_get_latencies(ns->udf_sub_master_hist, db);
				histogram_get_latencies(ns->udf_sub_repl_write_hist, db);
				histogram_get_latencies(ns->udf_sub_response_hist, db);
			}
			else if (strcmp(hist_name, "benchmarks-ops-sub") == 0) {
				histogram_get_latencies(ns->ops_sub_start_hist, db);
				histogram_get_latencies(ns->ops_sub_restart_hist, db);
				histogram_get_latencies(ns->ops_sub_dup_res_hist, db);
				histogram_get_latencies(ns->ops_sub_master_hist, db);
				histogram_get_latencies(ns->ops_sub_repl_write_hist, db);
				histogram_get_latencies(ns->ops_sub_response_hist, db);
			}
			else {
				cf_info(AS_INFO, "%s command: unrecognized histogram: %s", name, value_str);
				cf_dyn_buf_append_string(db, "error-bad-hist-name");
				return 0;
			}
		}
		else {
			cf_info(AS_INFO, "%s command: unrecognized histogram: %s", name, value_str);
			cf_dyn_buf_append_string(db, "error-bad-hist-name");
			return 0;
		}
	}

	cf_dyn_buf_chomp(db);

	return 0;
}


// TODO - separate all these CP-related info commands.

// Format is:
//
//	revive:{namespace=<ns-name>}
//
int
info_command_revive(char *name, char *params, cf_dyn_buf *db)
{
	if (as_info_error_enterprise_only()) {
		cf_dyn_buf_append_string(db, "ERROR::enterprise-only");
		return 0;
	}

	char ns_name[AS_ID_NAMESPACE_SZ] = { 0 };
	int ns_name_len = (int)sizeof(ns_name);
	int rv = as_info_parameter_get(params, "namespace", ns_name, &ns_name_len);

	if (rv == -2) {
		cf_warning(AS_INFO, "revive: namespace parameter value too long");
		cf_dyn_buf_append_string(db, "ERROR::bad-namespace");
		return 0;
	}

	if (rv == 0) {
		as_namespace *ns = as_namespace_get_byname(ns_name);

		if (! ns) {
			cf_warning(AS_INFO, "revive: unknown namespace %s", ns_name);
			cf_dyn_buf_append_string(db, "ERROR::unknown-namespace");
			return 0;
		}

		if (! as_partition_balance_revive(ns)) {
			cf_warning(AS_INFO, "revive: failed - recluster in progress");
			cf_dyn_buf_append_string(db, "ERROR::failed-revive");
			return 0;
		}

		cf_info(AS_INFO, "revive: complete - issue 'recluster:' command");
		cf_dyn_buf_append_string(db, "ok");
		return 0;
	}

	for (uint32_t ns_ix = 0; ns_ix < g_config.n_namespaces; ns_ix++) {
		as_namespace *ns = g_config.namespaces[ns_ix];

		if (! as_partition_balance_revive(ns)) {
			cf_warning(AS_INFO, "revive: failed - recluster in progress");
			cf_dyn_buf_append_string(db, "ERROR::failed-revive");
			return 0;
		}
	}

	cf_info(AS_INFO, "revive: complete - issue 'recluster:' command");
	cf_dyn_buf_append_string(db, "ok");
	return 0;
}

void
namespace_roster_info(as_namespace *ns, cf_dyn_buf *db)
{
	as_exchange_info_lock();

	cf_dyn_buf_append_string(db, "roster=");

	if (ns->roster_count == 0) {
		cf_dyn_buf_append_string(db, "null");
	}
	else {
		for (uint32_t n = 0; n < ns->roster_count; n++) {
			cf_dyn_buf_append_uint64_x(db, ns->roster[n]);

			if (ns->roster_rack_ids[n] != 0) {
				cf_dyn_buf_append_char(db, ROSTER_ID_PAIR_SEPARATOR);
				cf_dyn_buf_append_uint32(db, ns->roster_rack_ids[n]);
			}

			cf_dyn_buf_append_char(db, ',');
		}

		cf_dyn_buf_chomp(db);
	}

	cf_dyn_buf_append_char(db, ':');

	cf_dyn_buf_append_string(db, "pending_roster=");

	if (ns->smd_roster_count == 0) {
		cf_dyn_buf_append_string(db, "null");
	}
	else {
		for (uint32_t n = 0; n < ns->smd_roster_count; n++) {
			cf_dyn_buf_append_uint64_x(db, ns->smd_roster[n]);

			if (ns->smd_roster_rack_ids[n] != 0) {
				cf_dyn_buf_append_char(db, ROSTER_ID_PAIR_SEPARATOR);
				cf_dyn_buf_append_uint32(db, ns->smd_roster_rack_ids[n]);
			}

			cf_dyn_buf_append_char(db, ',');
		}

		cf_dyn_buf_chomp(db);
	}

	cf_dyn_buf_append_char(db, ':');

	cf_dyn_buf_append_string(db, "observed_nodes=");

	if (ns->observed_cluster_size == 0) {
		cf_dyn_buf_append_string(db, "null");
	}
	else {
		for (uint32_t n = 0; n < ns->observed_cluster_size; n++) {
			cf_dyn_buf_append_uint64_x(db, ns->observed_succession[n]);

			if (ns->rack_ids[n] != 0) {
				cf_dyn_buf_append_char(db, ROSTER_ID_PAIR_SEPARATOR);
				cf_dyn_buf_append_uint32(db, ns->rack_ids[n]);
			}

			cf_dyn_buf_append_char(db, ',');
		}

		cf_dyn_buf_chomp(db);
	}

	as_exchange_info_unlock();
}

// Format is:
//
//	roster:{namespace=<ns-name>}
//
int
info_command_roster(char *name, char *params, cf_dyn_buf *db)
{
	if (as_info_error_enterprise_only()) {
		cf_dyn_buf_append_string(db, "ERROR::enterprise-only");
		return 0;
	}

	char ns_name[AS_ID_NAMESPACE_SZ] = { 0 };
	int ns_name_len = (int)sizeof(ns_name);
	int rv = as_info_parameter_get(params, "namespace", ns_name, &ns_name_len);

	if (rv == -2) {
		cf_warning(AS_INFO, "namespace parameter value too long");
		cf_dyn_buf_append_string(db, "ERROR::bad-namespace");
		return 0;
	}

	if (rv == 0) {
		as_namespace *ns = as_namespace_get_byname(ns_name);

		if (! ns) {
			cf_warning(AS_INFO, "unknown namespace %s", ns_name);
			cf_dyn_buf_append_string(db, "ERROR::unknown-namespace");
			return 0;
		}

		namespace_roster_info(ns, db);

		return 0;
	}

	for (uint32_t ns_ix = 0; ns_ix < g_config.n_namespaces; ns_ix++) {
		as_namespace *ns = g_config.namespaces[ns_ix];

		cf_dyn_buf_append_string(db, "ns=");
		cf_dyn_buf_append_string(db, ns->name);
		cf_dyn_buf_append_char(db, ':');

		namespace_roster_info(ns, db);

		cf_dyn_buf_append_char(db, ';');
	}

	cf_dyn_buf_chomp(db);

	return 0;
}

// Format is:
//
//	roster-set:namespace=<ns-name>;nodes=<nodes-string>
//
// where <nodes-string> is comma-separated list of node-id:rack-id pairs, and
// the :rack-id may be absent, indicating a rack-id of 0.
//
int
info_command_roster_set(char *name, char *params, cf_dyn_buf *db)
{
	if (as_info_error_enterprise_only()) {
		cf_dyn_buf_append_string(db, "ERROR::enterprise-only");
		return 0;
	}

	// Get the namespace name.

	char ns_name[AS_ID_NAMESPACE_SZ];
	int ns_name_len = (int)sizeof(ns_name);
	int ns_rv = as_info_parameter_get(params, "namespace", ns_name, &ns_name_len);

	if (ns_rv != 0 || ns_name_len == 0) {
		cf_warning(AS_INFO, "roster-set command: missing or invalid namespace name in command");
		cf_dyn_buf_append_string(db, "ERROR::namespace-name");
		return 0;
	}

	// Get the nodes list.

	char nodes[AS_CLUSTER_SZ * ROSTER_STRING_ELE_LEN];
	int nodes_len = (int)sizeof(nodes);
	int nodes_rv = as_info_parameter_get(params, "nodes", nodes, &nodes_len);

	if (nodes_rv == -2 || (nodes_rv == 0 && nodes_len == 0)) {
		cf_warning(AS_INFO, "roster-set command: invalid nodes in command");
		cf_dyn_buf_append_string(db, "ERROR::nodes");
		return 0;
	}

	// Issue the roster-set command.

	as_roster_set_nodes_cmd(ns_name, nodes, db);

	return 0;
}

// Format is:
//
//	truncate-namespace:namespace=<ns-name>[;lut=<UTC-nanosec-string>]
//
//	... where no lut value means use this server's current time.
//
int
info_command_truncate_namespace(char *name, char *params, cf_dyn_buf *db)
{
	// Get the namespace name.

	char ns_name[AS_ID_NAMESPACE_SZ];
	int ns_name_len = (int)sizeof(ns_name);
	int ns_rv = as_info_parameter_get(params, "namespace", ns_name, &ns_name_len);

	if (ns_rv != 0 || ns_name_len == 0) {
		cf_warning(AS_INFO, "truncate-namespace command: missing or invalid namespace name in command");
		cf_dyn_buf_append_string(db, "ERROR::namespace-name");
		return 0;
	}

	// Check for a set-name, for safety. (Did user intend 'truncate'?)

	char set_name[1]; // just checking for existence
	int set_name_len = (int)sizeof(set_name);
	int set_rv = as_info_parameter_get(params, "set", set_name, &set_name_len);

	if (set_rv != -1) {
		cf_warning(AS_INFO, "truncate-namespace command: unexpected set name in command");
		cf_dyn_buf_append_string(db, "ERROR::unexpected-set-name");
		return 0;
	}

	// Get the threshold last-update-time, if there is one.

	char lut_str[24]; // allow decimal, hex or octal in C constant format
	int lut_str_len = (int)sizeof(lut_str);
	int lut_rv = as_info_parameter_get(params, "lut", lut_str, &lut_str_len);

	if (lut_rv == -2 || (lut_rv == 0 && lut_str_len == 0)) {
		cf_warning(AS_INFO, "truncate-namespace command: invalid last-update-time in command");
		cf_dyn_buf_append_string(db, "ERROR::last-update-time");
		return 0;
	}

	// Issue the truncate command.

	as_truncate_cmd(ns_name, NULL, lut_rv == 0 ? lut_str : NULL, db);
	return 0;
}

// Format is:
//
//	truncate-namespace-undo:namespace=<ns-name>
//
int
info_command_truncate_namespace_undo(char *name, char *params, cf_dyn_buf *db)
{
	// Get the namespace name.

	char ns_name[AS_ID_NAMESPACE_SZ];
	int ns_name_len = (int)sizeof(ns_name);
	int ns_rv = as_info_parameter_get(params, "namespace", ns_name, &ns_name_len);

	if (ns_rv != 0 || ns_name_len == 0) {
		cf_warning(AS_INFO, "truncate-namespace-undo command: missing or invalid namespace name in command");
		cf_dyn_buf_append_string(db, "ERROR::namespace-name");
		return 0;
	}

	// Check for a set-name, for safety. (Did user intend 'truncate-undo'?)

	char set_name[1]; // just checking for existence
	int set_name_len = (int)sizeof(set_name);
	int set_rv = as_info_parameter_get(params, "set", set_name, &set_name_len);

	if (set_rv != -1) {
		cf_warning(AS_INFO, "truncate-namespace-undo command: unexpected set name in command");
		cf_dyn_buf_append_string(db, "ERROR::unexpected-set-name");
		return 0;
	}

	// Issue the truncate-undo command.

	as_truncate_undo_cmd(ns_name, NULL, db);

	return 0;
}

// Format is:
//
//	truncate:namespace=<ns-name>;set=<set-name>[;lut=<UTC-nanosec-string>]
//
//	... where no lut value means use this server's current time.
//
int
info_command_truncate(char *name, char *params, cf_dyn_buf *db)
{
	// Get the namespace name.

	char ns_name[AS_ID_NAMESPACE_SZ];
	int ns_name_len = (int)sizeof(ns_name);
	int ns_rv = as_info_parameter_get(params, "namespace", ns_name, &ns_name_len);

	if (ns_rv != 0 || ns_name_len == 0) {
		cf_warning(AS_INFO, "truncate command: missing or invalid namespace name in command");
		cf_dyn_buf_append_string(db, "ERROR::namespace-name");
		return 0;
	}

	// Get the set-name.

	char set_name[AS_SET_NAME_MAX_SIZE];
	int set_name_len = (int)sizeof(set_name);
	int set_rv = as_info_parameter_get(params, "set", set_name, &set_name_len);

	if (set_rv != 0 || set_name_len == 0) {
		cf_warning(AS_INFO, "truncate command: missing or invalid set name in command");
		cf_dyn_buf_append_string(db, "ERROR::set-name");
		return 0;
	}

	// Get the threshold last-update-time, if there is one.

	char lut_str[24]; // allow decimal, hex or octal in C constant format
	int lut_str_len = (int)sizeof(lut_str);
	int lut_rv = as_info_parameter_get(params, "lut", lut_str, &lut_str_len);

	if (lut_rv == -2 || (lut_rv == 0 && lut_str_len == 0)) {
		cf_warning(AS_INFO, "truncate command: invalid last-update-time in command");
		cf_dyn_buf_append_string(db, "ERROR::last-update-time");
		return 0;
	}

	// Issue the truncate command.

	as_truncate_cmd(ns_name, set_name, lut_rv == 0 ? lut_str : NULL, db);
	return 0;
}

// Format is:
//
//	truncate-undo:namespace=<ns-name>;set=<set-name>
//
int
info_command_truncate_undo(char *name, char *params, cf_dyn_buf *db)
{
	// Get the namespace name.

	char ns_name[AS_ID_NAMESPACE_SZ];
	int ns_name_len = (int)sizeof(ns_name);
	int ns_rv = as_info_parameter_get(params, "namespace", ns_name, &ns_name_len);

	if (ns_rv != 0 || ns_name_len == 0) {
		cf_warning(AS_INFO, "truncate-undo command: missing or invalid namespace name in command");
		cf_dyn_buf_append_string(db, "ERROR::namespace-name");
		return 0;
	}

	// Get the set-name.

	char set_name[AS_SET_NAME_MAX_SIZE];
	int set_name_len = (int)sizeof(set_name);
	int set_rv = as_info_parameter_get(params, "set", set_name, &set_name_len);

	if (set_rv != 0 || set_name_len == 0) {
		cf_warning(AS_INFO, "truncate-undo command: missing or invalid set name in command");
		cf_dyn_buf_append_string(db, "ERROR::set-name");
		return 0;
	}

	// Issue the truncate-undo command.

	as_truncate_undo_cmd(ns_name, set_name, db);

	return 0;
}

// Format is:
//
//	eviction-reset:namespace=<ns-name>[;ttl=<seconds-from-now>]
//
//	... where no ttl means delete the SMD evict-void-time.
//
int
info_command_eviction_reset(char *name, char *params, cf_dyn_buf *db)
{
	// Get the namespace name.

	char ns_name[AS_ID_NAMESPACE_SZ];
	int ns_name_len = (int)sizeof(ns_name);
	int ns_rv = as_info_parameter_get(params, "namespace", ns_name, &ns_name_len);

	if (ns_rv != 0 || ns_name_len == 0) {
		cf_warning(AS_INFO, "eviction-reset command: missing or invalid namespace name in command");
		cf_dyn_buf_append_string(db, "ERROR::namespace-name");
		return 0;
	}

	// Get the TTL if there is one.

	char ttl_str[12]; // allow decimal, hex or octal in C constant format
	int ttl_str_len = (int)sizeof(ttl_str);
	int ttl_rv = as_info_parameter_get(params, "ttl", ttl_str, &ttl_str_len);

	if (ttl_rv == -2 || (ttl_rv == 0 && ttl_str_len == 0)) {
		cf_warning(AS_INFO, "eviction-reset command: invalid ttl in command");
		cf_dyn_buf_append_string(db, "ERROR::ttl");
		return 0;
	}

	// Issue the eviction-reset command.

	as_nsup_eviction_reset_cmd(ns_name, ttl_rv == 0 ? ttl_str : NULL, db);

	return 0;
}

//
// Log a message to the server.
// Limited to 2048 characters.
//
// Format:
//	log-message:message=<MESSAGE>[;who=<WHO>]
//
// Example:
// 	log-message:message=Example Log Message;who=Aerospike User
//
int
info_command_log_message(char *name, char *params, cf_dyn_buf *db)
{
	char who[128];
	int who_len = sizeof(who);
	if (0 != as_info_parameter_get(params, "who", who, &who_len)) {
		strcpy(who, "unknown");
	}

	char message[2048];
	int message_len = sizeof(message);
	if (0 == as_info_parameter_get(params, "message", message, &message_len)) {
		cf_info(AS_INFO, "%s: %s", who, message);
	}

	return 0;
}

// Generic info system functions
// These functions act when an INFO message comes in over the PROTO pipe
// collects the static and dynamic portions, puts it in a 'dyn buf',
// and sends a reply
//

// Error strings for security check results.
static void
append_sec_err_str(cf_dyn_buf *db, uint32_t result, as_sec_perm cmd_perm) {
	switch (result) {
	case AS_SEC_ERR_NOT_AUTHENTICATED:
		cf_dyn_buf_append_string(db, "ERROR:");
		cf_dyn_buf_append_uint32(db, result);
		cf_dyn_buf_append_string(db, ":not authenticated");
		return;
	case AS_SEC_ERR_ROLE_VIOLATION:
		switch (cmd_perm) {
		case PERM_SINDEX_ADMIN:
			INFO_FAIL_RESPONSE(db, result, "role violation");
			return;
		case PERM_UDF_ADMIN:
			cf_dyn_buf_append_string(db, "error=role_violation");
			return;
		default:
			break;
		}
		cf_dyn_buf_append_string(db, "ERROR:");
		cf_dyn_buf_append_uint32(db, result);
		cf_dyn_buf_append_string(db, ":role violation");
		return;
	default:
		cf_dyn_buf_append_string(db, "ERROR:");
		cf_dyn_buf_append_uint32(db, result);
		cf_dyn_buf_append_string(db, ":unexpected security error");
		return;
	}
}

static cf_mutex g_info_lock = CF_MUTEX_INIT;
info_static		*static_head = 0;
info_dynamic	*dynamic_head = 0;
info_tree		*tree_head = 0;
info_command	*command_head = 0;
//
// Pull up all elements in both list into the buffers
// (efficient enough if you're looking for lots of things)
// But only gets 'default' values
//

int
info_all(const as_file_handle* fd_h, cf_dyn_buf *db)
{
	uint8_t auth_result = as_security_check_auth(fd_h);

	if (auth_result != AS_OK) {
		as_security_log(fd_h, auth_result, PERM_NONE, "info-all request", NULL);
		append_sec_err_str(db, auth_result, PERM_NONE);
		cf_dyn_buf_append_char(db, EOL);
		return 0;
	}

	info_static *s = static_head;
	while (s) {
		if (s->def == true) {
			cf_dyn_buf_append_string( db, s->name);
			cf_dyn_buf_append_char( db, SEP );
			cf_dyn_buf_append_buf( db, (uint8_t *) s->value, s->value_sz);
			cf_dyn_buf_append_char( db, EOL );
		}
		s = s->next;
	}

	info_dynamic *d = dynamic_head;
	while (d) {
		if (d->def == true) {
			cf_dyn_buf_append_string( db, d->name);
			cf_dyn_buf_append_char(db, SEP );
			d->value_fn(d->name, db);
			cf_dyn_buf_append_char(db, EOL);
		}
		d = d->next;
	}

	return(0);
}

//
// Parse the input buffer. It contains a list of keys that should be spit back.
// Do the parse, call the necessary function collecting the information in question
// Filling the dynbuf

int
info_some(char *buf, char *buf_lim, const as_file_handle* fd_h, cf_dyn_buf *db)
{
	uint8_t auth_result = as_security_check_auth(fd_h);

	if (auth_result != AS_OK) {
		// TODO - log null-terminated buf as detail?
		as_security_log(fd_h, auth_result, PERM_NONE, "info request", NULL);
		append_sec_err_str(db, auth_result, PERM_NONE);
		cf_dyn_buf_append_char(db, EOL);
		return 0;
	}

	// For each incoming name
	char	*c = buf;
	char	*tok = c;

	while (c < buf_lim) {

		if ( *c == EOL ) {
			*c = 0;
			char *name = tok;
			bool handled = false;

			// search the static queue first always
			info_static *s = static_head;
			while (s) {
				if (strcmp(s->name, name) == 0) {
					// return exact command string received from client
					cf_dyn_buf_append_string( db, name);
					cf_dyn_buf_append_char( db, SEP );
					cf_dyn_buf_append_buf( db, (uint8_t *) s->value, s->value_sz);
					cf_dyn_buf_append_char( db, EOL );
					handled = true;
					break;
				}
				s = s->next;
			}

			// didn't find in static, try dynamic
			if (!handled) {
				info_dynamic *d = dynamic_head;
				while (d) {
					if (strcmp(d->name, name) == 0) {
						// return exact command string received from client
						cf_dyn_buf_append_string( db, d->name);
						cf_dyn_buf_append_char(db, SEP );
						d->value_fn(d->name, db);
						cf_dyn_buf_append_char(db, EOL);
						handled = true;
						break;
					}
					d = d->next;
				}
			}

			// search the tree
			if (!handled) {

				// see if there's a '/',
				char *branch = strchr( name, TREE_SEP);
				if (branch) {
					*branch = 0;
					branch++;

					info_tree *t = tree_head;
					while (t) {
						if (strcmp(t->name, name) == 0) {
							// return exact command string received from client
							cf_dyn_buf_append_string( db, t->name);
							cf_dyn_buf_append_char( db, TREE_SEP);
							cf_dyn_buf_append_string( db, branch);
							cf_dyn_buf_append_char(db, SEP );
							t->tree_fn(t->name, branch, db);
							cf_dyn_buf_append_char(db, EOL);
							break;
						}
						t = t->next;
					}
				}
			}

			tok = c + 1;
		}
		// commands have parameters
		else if ( *c == ':' ) {
			*c = 0;
			char *name = tok;

			// parse parameters
			tok = c + 1;
			// make sure c doesn't go beyond buf_lim
			while (*c != EOL && c < buf_lim-1) c++;
			if (*c != EOL) {
				cf_warning(AS_INFO, "Info '%s' parameter not terminated with '\\n'.", name);
				break;
			}
			*c = 0;
			char *param = tok;

			// search the command list
			info_command *cmd = command_head;
			while (cmd) {
				if (strcmp(cmd->name, name) == 0) {
					// return exact command string received from client
					cf_dyn_buf_append_string( db, name);
					cf_dyn_buf_append_char( db, ':');
					cf_dyn_buf_append_string( db, param);
					cf_dyn_buf_append_char( db, SEP );

					uint8_t result = as_security_check_info_cmd(fd_h, name, param, cmd->required_perm);

					as_security_log(fd_h, result, cmd->required_perm, name, param);

					if (result == AS_OK) {
						cmd->command_fn(cmd->name, param, db);
					}
					else {
						append_sec_err_str(db, result, cmd->required_perm);
					}

					cf_dyn_buf_append_char( db, EOL );
					break;
				}
				cmd = cmd->next;
			}

			if (!cmd) {
				cf_info(AS_INFO, "received command %s, not registered", name);
			}

			tok = c + 1;
		}

		c++;

	}
	return(0);
}

int
as_info_buffer(uint8_t *req_buf, size_t req_buf_len, cf_dyn_buf *rsp)
{
	// Either we'e doing all, or doing some
	if (req_buf_len == 0) {
		info_all(NULL, rsp);
	}
	else {
		info_some((char *)req_buf, (char *)(req_buf + req_buf_len), NULL, rsp);
	}

	return(0);
}

//
// Worker threads!
// these actually do the work. There is a lot of network activity,
// writes and such, don't want to clog up the main queue
//

void *
thr_info_fn(void *unused)
{
	for ( ; ; ) {

		as_info_transaction it;

		if (0 != cf_queue_pop(g_info_work_q, &it, CF_QUEUE_FOREVER)) {
			cf_crash(AS_TSVC, "unable to pop from info work queue");
		}

		if (it.fd_h == NULL) {
			break; // termination signal
		}

		as_file_handle *fd_h = it.fd_h;
		as_proto *pr = it.proto;

		// Allocate an output buffer sufficiently large to avoid ever resizing
		cf_dyn_buf_define_size(db, 128 * 1024);
		// write space for the header
		uint64_t	h = 0;
		cf_dyn_buf_append_buf(&db, (uint8_t *) &h, sizeof(h));

		// Either we'e doing all, or doing some
		if (pr->sz == 0) {
			info_all(fd_h, &db);
		}
		else {
			info_some((char *)pr->body, (char *)pr->body + pr->sz, fd_h, &db);
		}

		// write the proto header in the space we pre-wrote
		db.buf[0] = 2;
		db.buf[1] = 1;
		uint64_t	sz = db.used_sz - 8;
		db.buf[4] = (sz >> 24) & 0xff;
		db.buf[5] = (sz >> 16) & 0xff;
		db.buf[6] = (sz >> 8) & 0xff;
		db.buf[7] = sz & 0xff;

		// write the data buffer
		if (cf_socket_send_all(&fd_h->sock, db.buf, db.used_sz,
				MSG_NOSIGNAL, CF_SOCKET_TIMEOUT) < 0) {
			cf_info(AS_INFO, "error sending to %s - fd %d sz %zu %s",
					fd_h->client, CSFD(&fd_h->sock), db.used_sz,
					cf_strerror(errno));
			as_end_of_transaction_force_close(fd_h);
			fd_h = NULL;
		}

		cf_dyn_buf_free(&db);

		cf_free(pr);

		if (fd_h) {
			as_end_of_transaction_ok(fd_h);
			fd_h = NULL;
		}

		G_HIST_INSERT_DATA_POINT(info_hist, it.start_time);
		cf_atomic64_incr(&g_stats.info_complete);
	}

	return NULL;
}

//
// received an info request from a file descriptor
// Called by the thr_tsvc when an info message is seen
// calls functions info_all or info_some to collect the response
// calls write to send the response back
//
// Proto will be freed by the caller
//

void
as_info(as_info_transaction *it)
{
	cf_queue_push(g_info_work_q, it);
}

// Called via info command. Caller has sanity-checked n_threads.
void
info_set_num_info_threads(uint32_t n_threads)
{
	if (g_config.n_info_threads > n_threads) {
		// Decrease the number of info threads to n_threads.
		while (g_config.n_info_threads > n_threads) {
			as_info_transaction death_msg = { 0 };

			// Send terminator (NULL message).
			as_info(&death_msg);
			g_config.n_info_threads--;
		}
	}
	else {
		// Increase the number of info threads to n_threads.
		while (g_config.n_info_threads < n_threads) {
			cf_thread_create_transient(thr_info_fn, NULL);
			g_config.n_info_threads++;
		}
	}
}

// Return the number of pending Info requests in the queue.
uint32_t
as_info_queue_get_size()
{
	return cf_queue_sz(g_info_work_q);
}

// Registers a dynamic name-value calculator.
// the get_value_fn will be called if a request comes in for this name.
// only does the registration!
// def means it's part of the default results - will get invoked for a blank info command (asinfo -v "")


int
as_info_set_dynamic(const char *name, as_info_get_value_fn gv_fn, bool def)
{
	int rv = -1;
	cf_mutex_lock(&g_info_lock);

	info_dynamic *e = dynamic_head;
	while (e) {
		if (strcmp(name, e->name) == 0) {
			e->value_fn = gv_fn;
			break;
		}

		e = e->next;
	}

	if (!e) {
		e = cf_malloc(sizeof(info_dynamic));
		e->def = def;
		e->name = cf_strdup(name);
		e->value_fn = gv_fn;
		e->next = dynamic_head;
		dynamic_head = e;
	}
	rv = 0;

	cf_mutex_unlock(&g_info_lock);
	return(rv);
}


// Registers a tree-based name-value calculator.
// the get_value_fn will be called if a request comes in for this name.
// only does the registration!


int
as_info_set_tree(char *name, as_info_get_tree_fn gv_fn)
{
	int rv = -1;
	cf_mutex_lock(&g_info_lock);

	info_tree *e = tree_head;
	while (e) {
		if (strcmp(name, e->name) == 0) {
			e->tree_fn = gv_fn;
			break;
		}

		e = e->next;
	}

	if (!e) {
		e = cf_malloc(sizeof(info_tree));
		e->name = cf_strdup(name);
		e->tree_fn = gv_fn;
		e->next = tree_head;
		tree_head = e;
	}
	rv = 0;

	cf_mutex_unlock(&g_info_lock);
	return(rv);
}


// Registers a command handler
// the get_value_fn will be called if a request comes in for this name, and
// parameters will be passed in
// This function only does the registration!

int
as_info_set_command(const char *name, as_info_command_fn command_fn, as_sec_perm required_perm)
{
	int rv = -1;
	cf_mutex_lock(&g_info_lock);

	info_command *e = command_head;
	while (e) {
		if (strcmp(name, e->name) == 0) {
			e->command_fn = command_fn;
			break;
		}

		e = e->next;
	}

	if (!e) {
		e = cf_malloc(sizeof(info_command));
		e->name = cf_strdup(name);
		e->command_fn = command_fn;
		e->required_perm = required_perm;
		e->next = command_head;
		command_head = e;
	}
	rv = 0;

	cf_mutex_unlock(&g_info_lock);
	return(rv);
}



//
// Sets a static name-value pair
// def means it's part of the default set - will get returned if nothing is passed

int
as_info_set_buf(const char *name, const uint8_t *value, size_t value_sz, bool def)
{
	cf_mutex_lock(&g_info_lock);

	// Delete case
	if (value_sz == 0 || value == 0) {

		info_static *p = 0;
		info_static *e = static_head;

		while (e) {
			if (strcmp(name, e->name) == 0) {
				if (p) {
					p->next = e->next;
					cf_free(e->name);
					cf_free(e->value);
					cf_free(e);
				}
				else {
					info_static *_t = static_head->next;
					cf_free(e->name);
					cf_free(e->value);
					cf_free(static_head);
					static_head = _t;
				}
				break;
			}
			p = e;
			e = e->next;
		}
	}
	// insert case
	else {

		info_static *e = static_head;

		// search for old value and overwrite
		while(e) {
			if (strcmp(name, e->name) == 0) {
				cf_free(e->value);
				e->value = cf_malloc(value_sz);
				memcpy(e->value, value, value_sz);
				e->value_sz = value_sz;
				break;
			}
			e = e->next;
		}

		// not found, insert fresh
		if (e == 0) {
			info_static *_t = cf_malloc(sizeof(info_static));
			_t->next = static_head;
			_t->def = def;
			_t->name = cf_strdup(name);
			_t->value = cf_malloc(value_sz);
			memcpy(_t->value, value, value_sz);
			_t->value_sz = value_sz;
			static_head = _t;
		}
	}

	cf_mutex_unlock(&g_info_lock);
	return(0);

}

//
// A helper function. Commands have the form:
// cmd:param=value;param=value
//
// The main parser gives us the entire parameter string
// so use this function to scan through and get the particular parameter value
// you're looking for
//
// The 'param_string' is the param passed by the command parser into a command
//
// @return  0 : success
//         -1 : parameter not found
//         -2 : parameter found but value is too long
//

int
as_info_parameter_get(const char *param_str, const char *param, char *value,
		int *value_len)
{
	cf_detail(AS_INFO, "parameter get: paramstr %s seeking param %s", param_str, param);

	const char *c = param_str;
	const char *tok = param_str;
	int param_len = strlen(param);

	while (*c) {
		if (*c == '=') {
			if ( ( param_len == c - tok) && (0 == memcmp(tok, param, param_len) ) ) {
				c++;
				tok = c;
				while ( *c != 0 && *c != ';') c++;
				if (*value_len <= c - tok)	{
					// The found value is too long.
					return(-2);
				}
				*value_len = c - tok;
				memcpy(value, tok, *value_len);
				value[*value_len] = 0;
				return(0);
			}
			c++;
		}
		else if (*c == ';') {
			c++;
			tok = c;
		}
		else c++;

	}

	return(-1);
}

int
as_info_set(const char *name, const char *value, bool def)
{
	return(as_info_set_buf(name, (const uint8_t *) value, strlen(value), def ) );
}

//
// Iterate through the current namespace list and cons up a string
//

int
info_get_namespaces(char *name, cf_dyn_buf *db)
{
	for (uint32_t i = 0; i < g_config.n_namespaces; i++) {
		cf_dyn_buf_append_string(db, g_config.namespaces[i]->name);
		cf_dyn_buf_append_char(db, ';');
	}

	if (g_config.n_namespaces > 0) {
		cf_dyn_buf_chomp(db);
	}

	return(0);
}

int
info_get_health_outliers(char *name, cf_dyn_buf *db)
{
	as_health_get_outliers(db);
	return(0);
}

int
info_get_health_stats(char *name, cf_dyn_buf *db)
{
	as_health_get_stats(db);
	return(0);
}

int
info_get_index_pressure(char *name, cf_dyn_buf *db)
{
	for (uint32_t i = 0; i < g_config.n_namespaces; i++) {
		as_namespace *ns = g_config.namespaces[i];
		cf_page_cache_stats stats;

		if (!cf_page_cache_get_stats(ns->arena, &stats)) {
			continue;
		}

		cf_dyn_buf_append_string(db, ns->name);
		cf_dyn_buf_append_char(db, ':');
		cf_dyn_buf_append_uint64(db, stats.resident);
		cf_dyn_buf_append_char(db, ':');
		cf_dyn_buf_append_uint64(db, stats.dirty);
		cf_dyn_buf_append_char(db, ';');
	}

	cf_dyn_buf_chomp(db);
	return 0;
}

int
info_get_logs(char *name, cf_dyn_buf *db)
{
	cf_log_get_sinks(db);
	return(0);
}

int
info_get_objects(char *name, cf_dyn_buf *db)
{
	uint64_t	objects = 0;

	for (uint32_t i = 0; i < g_config.n_namespaces; i++) {
		objects += g_config.namespaces[i]->n_objects;
	}

	cf_dyn_buf_append_uint64(db, objects);
	return(0);
}

int
info_get_sets(char *name, cf_dyn_buf *db)
{
	return info_get_tree_sets(name, "", db);
}

int
info_get_smd_info(char *name, cf_dyn_buf *db)
{
	as_smd_get_info(db);

	return (0);
}

int
info_get_bins(char *name, cf_dyn_buf *db)
{
	return info_get_tree_bins(name, "", db);
}

int
info_get_config( char* name, cf_dyn_buf *db)
{
	return info_command_config_get(name, NULL, db);
}

int
info_get_sindexes(char *name, cf_dyn_buf *db)
{
	return info_get_tree_sindexes(name, "", db);
}

static int32_t
oldest_nvme_age(const char *path)
{
	cf_storage_device_info *info = cf_storage_get_device_info(path);

	if (info == NULL) {
		return -1;
	}

	int32_t oldest = -1;

	for (int32_t i = 0; i < info->n_phys; ++i) {
		if (info->phys[i].nvme_age > oldest) {
			oldest = info->phys[i].nvme_age;
		}
	}

	return oldest;
}

static void
add_index_device_stats(as_namespace *ns, cf_dyn_buf *db)
{
	for (uint32_t i = 0; i < ns->n_xmem_mounts; i++) {
		info_append_indexed_int(db, "index-type.mount", i, "age",
				oldest_nvme_age(ns->xmem_mounts[i]));
	}
}

static void
add_data_device_stats(as_namespace *ns, cf_dyn_buf *db)
{
	uint32_t n = as_namespace_device_count(ns);
	const char* tag = ns->n_storage_devices != 0 ?
			"storage-engine.device" : "storage-engine.file";

	for (uint32_t i = 0; i < n; i++) {
		storage_device_stats stats;
		as_storage_device_stats(ns, i, &stats);

		info_append_indexed_uint64(db, tag, i, "used_bytes", stats.used_sz);
		info_append_indexed_uint32(db, tag, i, "free_wblocks", stats.n_free_wblocks);

		info_append_indexed_uint32(db, tag, i, "write_q", stats.write_q_sz);
		info_append_indexed_uint64(db, tag, i, "writes", stats.n_writes);

		info_append_indexed_uint32(db, tag, i, "defrag_q", stats.defrag_q_sz);
		info_append_indexed_uint64(db, tag, i, "defrag_reads", stats.n_defrag_reads);
		info_append_indexed_uint64(db, tag, i, "defrag_writes", stats.n_defrag_writes);

		info_append_indexed_uint32(db, tag, i, "shadow_write_q", stats.shadow_write_q_sz);

		info_append_indexed_int(db, tag, i, "age",
				oldest_nvme_age(ns->storage_devices[i]));
	}
}

void
info_get_namespace_info(as_namespace *ns, cf_dyn_buf *db)
{
	// Cluster size.

	// Using ns_ prefix to avoid confusion with global cluster_size.
	info_append_uint32(db, "ns_cluster_size", ns->cluster_size);

	info_append_uint32(db, "effective_replication_factor", ns->replication_factor);

	// Object counts.

	info_append_uint64(db, "objects", ns->n_objects);
	info_append_uint64(db, "tombstones", ns->n_tombstones);
	info_append_uint64(db, "xdr_tombstones", ns->n_xdr_tombstones);
	info_append_uint64(db, "xdr_bin_cemeteries", ns->n_xdr_bin_cemeteries);

	repl_stats mp;
	as_partition_get_replica_stats(ns, &mp);

	info_append_uint64(db, "master_objects", mp.n_master_objects);
	info_append_uint64(db, "master_tombstones", mp.n_master_tombstones);
	info_append_uint64(db, "prole_objects", mp.n_prole_objects);
	info_append_uint64(db, "prole_tombstones", mp.n_prole_tombstones);
	info_append_uint64(db, "non_replica_objects", mp.n_non_replica_objects);
	info_append_uint64(db, "non_replica_tombstones", mp.n_non_replica_tombstones);

	// Consistency info.

	info_append_uint64(db, "unreplicated_records", ns->n_unreplicated_records);
	info_append_uint32(db, "dead_partitions", ns->n_dead_partitions);
	info_append_uint32(db, "unavailable_partitions", ns->n_unavailable_partitions);
	info_append_bool(db, "clock_skew_stop_writes", ns->clock_skew_stop_writes);

	// Expiration & eviction (nsup) stats.

	info_append_bool(db, "stop_writes", ns->stop_writes);
	info_append_bool(db, "hwm_breached", ns->hwm_breached);

	info_append_uint64(db, "current_time", as_record_void_time_get());
	info_append_uint64(db, "non_expirable_objects", ns->non_expirable_objects);
	info_append_uint64(db, "expired_objects", ns->n_expired_objects);
	info_append_uint64(db, "evicted_objects", ns->n_evicted_objects);
	info_append_int(db, "evict_ttl", ns->evict_ttl);
	info_append_uint32(db, "evict_void_time", ns->evict_void_time);
	info_append_uint32(db, "smd_evict_void_time", ns->smd_evict_void_time);
	info_append_uint32(db, "nsup_cycle_duration", ns->nsup_cycle_duration);

	// Truncate stats.

	info_append_uint64(db, "truncate_lut", ns->truncate.lut);
	info_append_uint64(db, "truncated_records", ns->truncate.n_records);

	// Sindex GC stats.

	info_append_uint64(db, "sindex_gc_cleaned", ns->n_sindex_gc_cleaned);

	// Memory usage stats.

	uint64_t index_used = (ns->n_tombstones + ns->n_objects) * sizeof(as_index);

	uint64_t data_memory = ns->n_bytes_memory;
	uint64_t index_memory = as_namespace_index_persisted(ns) ? 0 : index_used;
	uint64_t set_index_memory = as_set_index_used_bytes(ns);
	uint64_t sindex_memory = as_sindex_used_bytes(ns);
	uint64_t used_memory = data_memory + index_memory + set_index_memory + sindex_memory;

	info_append_uint64(db, "memory_used_bytes", used_memory);
	info_append_uint64(db, "memory_used_data_bytes", data_memory);
	info_append_uint64(db, "memory_used_index_bytes", index_memory);
	info_append_uint64(db, "memory_used_set_index_bytes", set_index_memory);
	info_append_uint64(db, "memory_used_sindex_bytes", sindex_memory);

	uint64_t free_pct = ns->memory_size > used_memory ?
			((ns->memory_size - used_memory) * 100L) / ns->memory_size : 0;

	info_append_uint64(db, "memory_free_pct", free_pct);

	// Persistent memory block keys' namespace ID (enterprise only).
	info_append_uint32(db, "xmem_id", ns->xmem_id);

	// Remaining bin-name slots.
	if (! ns->single_bin) {
		info_append_uint32(db, "available_bin_names", MAX_BIN_NAMES - cf_vmapx_count(ns->p_bin_name_vmap));
	}

	// Persistent index stats.

	if (ns->xmem_type == CF_XMEM_TYPE_PMEM) {
		// If numa-pinned, not all configured mounts are used.
		if (as_config_is_numa_pinned()) {
			for (uint32_t i = 0; i < ns->n_xmem_mounts; i++) {
				if (cf_mount_is_local(ns->xmem_mounts[i])) {
					info_append_indexed_string(db, "local_mount", i, NULL,
							ns->xmem_mounts[i]);
				}
			}
		}

		uint64_t used_pct = index_used * 100 / ns->mounts_size_limit;

		info_append_uint64(db, "index_pmem_used_bytes", index_used);
		info_append_uint64(db, "index_pmem_used_pct", used_pct);
	}
	else if (ns->xmem_type == CF_XMEM_TYPE_FLASH) {
		uint64_t used_pct = index_used * 100 / ns->mounts_size_limit;

		info_append_uint64(db, "index_flash_used_bytes", index_used);
		info_append_uint64(db, "index_flash_used_pct", used_pct);

		uint64_t alloc_sz = as_load_uint64(&ns->arena->alloc_sz);

		info_append_uint64(db, "index_flash_alloc_bytes", alloc_sz);
		info_append_uint64(db, "index_flash_alloc_pct",
				alloc_sz * 100 / ns->mounts_size_limit);

		add_index_device_stats(ns, db);
	}

	// Persistent storage stats.

	if (ns->storage_type == AS_STORAGE_ENGINE_PMEM) {
		int available_pct = 0;
		uint64_t used_bytes = 0;
		as_storage_stats(ns, &available_pct, &used_bytes);

		info_append_uint64(db, "pmem_total_bytes", ns->drive_size);
		info_append_uint64(db, "pmem_used_bytes", used_bytes);

		free_pct = (ns->drive_size != 0 && (ns->drive_size > used_bytes)) ?
				((ns->drive_size - used_bytes) * 100L) / ns->drive_size : 0;

		info_append_uint64(db, "pmem_free_pct", free_pct);
		info_append_int(db, "pmem_available_pct", available_pct);

		if (ns->storage_compression != AS_COMPRESSION_NONE) {
			double orig_sz = as_load_double(&ns->comp_avg_orig_sz);
			double ratio = orig_sz > 0.0 ? ns->comp_avg_comp_sz / orig_sz : 1.0;

			info_append_format(db, "pmem_compression_ratio", "%.3f", ratio);
		}

		add_data_device_stats(ns, db);
	}
	else if (ns->storage_type == AS_STORAGE_ENGINE_SSD) {
		int available_pct = 0;
		uint64_t used_bytes = 0;
		as_storage_stats(ns, &available_pct, &used_bytes);

		info_append_uint64(db, "device_total_bytes", ns->drive_size);
		info_append_uint64(db, "device_used_bytes", used_bytes);

		free_pct = (ns->drive_size != 0 && (ns->drive_size > used_bytes)) ?
				((ns->drive_size - used_bytes) * 100L) / ns->drive_size : 0;

		info_append_uint64(db, "device_free_pct", free_pct);
		info_append_int(db, "device_available_pct", available_pct);

		if (ns->storage_compression != AS_COMPRESSION_NONE) {
			double orig_sz = as_load_double(&ns->comp_avg_orig_sz);
			double ratio = orig_sz > 0.0 ? ns->comp_avg_comp_sz / orig_sz : 1.0;

			info_append_format(db, "device_compression_ratio", "%.3f", ratio);
		}

		if (! ns->storage_data_in_memory) {
			info_append_int(db, "cache_read_pct", (int)(ns->cache_read_pct + 0.5));
		}

		add_data_device_stats(ns, db);
	}

	// Proto compression stats.

	double record_orig_sz = as_load_double(&ns->record_comp_stat.avg_orig_sz);
	double record_ratio = record_orig_sz > 0.0 ? ns->record_comp_stat.avg_comp_sz / record_orig_sz : 1.0;

	info_append_format(db, "record_proto_uncompressed_pct", "%.3f", ns->record_comp_stat.uncomp_pct);
	info_append_format(db, "record_proto_compression_ratio", "%.3f", record_ratio);

	double query_orig_sz = as_load_double(&ns->query_comp_stat.avg_orig_sz);
	double query_ratio = query_orig_sz > 0.0 ? ns->query_comp_stat.avg_comp_sz / query_orig_sz : 1.0;

	info_append_format(db, "query_proto_uncompressed_pct", "%.3f", ns->query_comp_stat.uncomp_pct);
	info_append_format(db, "query_proto_compression_ratio", "%.3f", query_ratio);

	// Partition balance state.

	info_append_bool(db, "pending_quiesce", ns->pending_quiesce);
	info_append_bool(db, "effective_is_quiesced", ns->is_quiesced);
	info_append_uint64(db, "nodes_quiesced", ns->cluster_size - ns->active_size);

	info_append_bool(db, "effective_prefer_uniform_balance", ns->prefer_uniform_balance);

	// Migration stats.

	info_append_uint64(db, "migrate_tx_partitions_imbalance", ns->migrate_tx_partitions_imbalance);

	info_append_uint64(db, "migrate_tx_instances", ns->migrate_tx_instance_count);
	info_append_uint64(db, "migrate_rx_instances", ns->migrate_rx_instance_count);

	info_append_uint64(db, "migrate_tx_partitions_active", ns->migrate_tx_partitions_active);
	info_append_uint64(db, "migrate_rx_partitions_active", ns->migrate_rx_partitions_active);

	info_append_uint64(db, "migrate_tx_partitions_initial", ns->migrate_tx_partitions_initial);
	info_append_uint64(db, "migrate_tx_partitions_remaining", ns->migrate_tx_partitions_remaining);
	info_append_uint64(db, "migrate_tx_partitions_lead_remaining", ns->migrate_tx_partitions_lead_remaining);

	info_append_uint64(db, "migrate_rx_partitions_initial", ns->migrate_rx_partitions_initial);
	info_append_uint64(db, "migrate_rx_partitions_remaining", ns->migrate_rx_partitions_remaining);

	info_append_uint64(db, "migrate_records_skipped", ns->migrate_records_skipped);
	info_append_uint64(db, "migrate_records_transmitted", ns->migrate_records_transmitted);
	info_append_uint64(db, "migrate_record_retransmits", ns->migrate_record_retransmits);
	info_append_uint64(db, "migrate_record_receives", ns->migrate_record_receives);

	info_append_uint64(db, "migrate_signals_active", ns->migrate_signals_active);
	info_append_uint64(db, "migrate_signals_remaining", ns->migrate_signals_remaining);

	info_append_uint64(db, "appeals_tx_active", ns->appeals_tx_active);
	info_append_uint64(db, "appeals_rx_active", ns->appeals_rx_active);

	info_append_uint64(db, "appeals_tx_remaining", ns->appeals_tx_remaining);

	info_append_uint64(db, "appeals_records_exonerated", ns->appeals_records_exonerated);

	// From-client transaction stats.

	info_append_uint64(db, "client_tsvc_error", ns->n_client_tsvc_error);
	info_append_uint64(db, "client_tsvc_timeout", ns->n_client_tsvc_timeout);

	info_append_uint64(db, "client_proxy_complete", ns->n_client_proxy_complete);
	info_append_uint64(db, "client_proxy_error", ns->n_client_proxy_error);
	info_append_uint64(db, "client_proxy_timeout", ns->n_client_proxy_timeout);

	info_append_uint64(db, "client_read_success", ns->n_client_read_success);
	info_append_uint64(db, "client_read_error", ns->n_client_read_error);
	info_append_uint64(db, "client_read_timeout", ns->n_client_read_timeout);
	info_append_uint64(db, "client_read_not_found", ns->n_client_read_not_found);
	info_append_uint64(db, "client_read_filtered_out", ns->n_client_read_filtered_out);

	info_append_uint64(db, "client_write_success", ns->n_client_write_success);
	info_append_uint64(db, "client_write_error", ns->n_client_write_error);
	info_append_uint64(db, "client_write_timeout", ns->n_client_write_timeout);
	info_append_uint64(db, "client_write_filtered_out", ns->n_client_write_filtered_out);

	// Subset of n_client_write_... above, respectively.
	info_append_uint64(db, "xdr_client_write_success", ns->n_xdr_client_write_success);
	info_append_uint64(db, "xdr_client_write_error", ns->n_xdr_client_write_error);
	info_append_uint64(db, "xdr_client_write_timeout", ns->n_xdr_client_write_timeout);

	info_append_uint64(db, "client_delete_success", ns->n_client_delete_success);
	info_append_uint64(db, "client_delete_error", ns->n_client_delete_error);
	info_append_uint64(db, "client_delete_timeout", ns->n_client_delete_timeout);
	info_append_uint64(db, "client_delete_not_found", ns->n_client_delete_not_found);
	info_append_uint64(db, "client_delete_filtered_out", ns->n_client_delete_filtered_out);

	// Subset of n_client_delete_... above, respectively.
	info_append_uint64(db, "xdr_client_delete_success", ns->n_xdr_client_delete_success);
	info_append_uint64(db, "xdr_client_delete_error", ns->n_xdr_client_delete_error);
	info_append_uint64(db, "xdr_client_delete_timeout", ns->n_xdr_client_delete_timeout);
	info_append_uint64(db, "xdr_client_delete_not_found", ns->n_xdr_client_delete_not_found);

	info_append_uint64(db, "client_udf_complete", ns->n_client_udf_complete);
	info_append_uint64(db, "client_udf_error", ns->n_client_udf_error);
	info_append_uint64(db, "client_udf_timeout", ns->n_client_udf_timeout);
	info_append_uint64(db, "client_udf_filtered_out", ns->n_client_udf_filtered_out);

	info_append_uint64(db, "client_lang_read_success", ns->n_client_lang_read_success);
	info_append_uint64(db, "client_lang_write_success", ns->n_client_lang_write_success);
	info_append_uint64(db, "client_lang_delete_success", ns->n_client_lang_delete_success);
	info_append_uint64(db, "client_lang_error", ns->n_client_lang_error);

	// From-proxy transaction stats.

	info_append_uint64(db, "from_proxy_tsvc_error", ns->n_from_proxy_tsvc_error);
	info_append_uint64(db, "from_proxy_tsvc_timeout", ns->n_from_proxy_tsvc_timeout);

	info_append_uint64(db, "from_proxy_read_success", ns->n_from_proxy_read_success);
	info_append_uint64(db, "from_proxy_read_error", ns->n_from_proxy_read_error);
	info_append_uint64(db, "from_proxy_read_timeout", ns->n_from_proxy_read_timeout);
	info_append_uint64(db, "from_proxy_read_not_found", ns->n_from_proxy_read_not_found);
	info_append_uint64(db, "from_proxy_read_filtered_out", ns->n_from_proxy_read_filtered_out);

	info_append_uint64(db, "from_proxy_write_success", ns->n_from_proxy_write_success);
	info_append_uint64(db, "from_proxy_write_error", ns->n_from_proxy_write_error);
	info_append_uint64(db, "from_proxy_write_timeout", ns->n_from_proxy_write_timeout);
	info_append_uint64(db, "from_proxy_write_filtered_out", ns->n_from_proxy_write_filtered_out);

	// Subset of n_from_proxy_write_... above, respectively.
	info_append_uint64(db, "xdr_from_proxy_write_success", ns->n_xdr_from_proxy_write_success);
	info_append_uint64(db, "xdr_from_proxy_write_error", ns->n_xdr_from_proxy_write_error);
	info_append_uint64(db, "xdr_from_proxy_write_timeout", ns->n_xdr_from_proxy_write_timeout);

	info_append_uint64(db, "from_proxy_delete_success", ns->n_from_proxy_delete_success);
	info_append_uint64(db, "from_proxy_delete_error", ns->n_from_proxy_delete_error);
	info_append_uint64(db, "from_proxy_delete_timeout", ns->n_from_proxy_delete_timeout);
	info_append_uint64(db, "from_proxy_delete_not_found", ns->n_from_proxy_delete_not_found);
	info_append_uint64(db, "from_proxy_delete_filtered_out", ns->n_from_proxy_delete_filtered_out);

	// Subset of n_from_proxy_delete_... above, respectively.
	info_append_uint64(db, "xdr_from_proxy_delete_success", ns->n_xdr_from_proxy_delete_success);
	info_append_uint64(db, "xdr_from_proxy_delete_error", ns->n_xdr_from_proxy_delete_error);
	info_append_uint64(db, "xdr_from_proxy_delete_timeout", ns->n_xdr_from_proxy_delete_timeout);
	info_append_uint64(db, "xdr_from_proxy_delete_not_found", ns->n_xdr_from_proxy_delete_not_found);

	info_append_uint64(db, "from_proxy_udf_complete", ns->n_from_proxy_udf_complete);
	info_append_uint64(db, "from_proxy_udf_error", ns->n_from_proxy_udf_error);
	info_append_uint64(db, "from_proxy_udf_timeout", ns->n_from_proxy_udf_timeout);
	info_append_uint64(db, "from_proxy_udf_filtered_out", ns->n_from_proxy_udf_filtered_out);

	info_append_uint64(db, "from_proxy_lang_read_success", ns->n_from_proxy_lang_read_success);
	info_append_uint64(db, "from_proxy_lang_write_success", ns->n_from_proxy_lang_write_success);
	info_append_uint64(db, "from_proxy_lang_delete_success", ns->n_from_proxy_lang_delete_success);
	info_append_uint64(db, "from_proxy_lang_error", ns->n_from_proxy_lang_error);

	// Batch sub-transaction stats.

	info_append_uint64(db, "batch_sub_tsvc_error", ns->n_batch_sub_tsvc_error);
	info_append_uint64(db, "batch_sub_tsvc_timeout", ns->n_batch_sub_tsvc_timeout);

	info_append_uint64(db, "batch_sub_proxy_complete", ns->n_batch_sub_proxy_complete);
	info_append_uint64(db, "batch_sub_proxy_error", ns->n_batch_sub_proxy_error);
	info_append_uint64(db, "batch_sub_proxy_timeout", ns->n_batch_sub_proxy_timeout);

	info_append_uint64(db, "batch_sub_read_success", ns->n_batch_sub_read_success);
	info_append_uint64(db, "batch_sub_read_error", ns->n_batch_sub_read_error);
	info_append_uint64(db, "batch_sub_read_timeout", ns->n_batch_sub_read_timeout);
	info_append_uint64(db, "batch_sub_read_not_found", ns->n_batch_sub_read_not_found);
	info_append_uint64(db, "batch_sub_read_filtered_out", ns->n_batch_sub_read_filtered_out);

	info_append_uint64(db, "batch_sub_write_success", ns->n_batch_sub_write_success);
	info_append_uint64(db, "batch_sub_write_error", ns->n_batch_sub_write_error);
	info_append_uint64(db, "batch_sub_write_timeout", ns->n_batch_sub_write_timeout);
	info_append_uint64(db, "batch_sub_write_filtered_out", ns->n_batch_sub_write_filtered_out);

	info_append_uint64(db, "batch_sub_delete_success", ns->n_batch_sub_delete_success);
	info_append_uint64(db, "batch_sub_delete_error", ns->n_batch_sub_delete_error);
	info_append_uint64(db, "batch_sub_delete_timeout", ns->n_batch_sub_delete_timeout);
	info_append_uint64(db, "batch_sub_delete_not_found", ns->n_batch_sub_delete_not_found);
	info_append_uint64(db, "batch_sub_delete_filtered_out", ns->n_batch_sub_delete_filtered_out);

	info_append_uint64(db, "batch_sub_udf_complete", ns->n_batch_sub_udf_complete);
	info_append_uint64(db, "batch_sub_udf_error", ns->n_batch_sub_udf_error);
	info_append_uint64(db, "batch_sub_udf_timeout", ns->n_batch_sub_udf_timeout);
	info_append_uint64(db, "batch_sub_udf_filtered_out", ns->n_batch_sub_udf_filtered_out);

	info_append_uint64(db, "batch_sub_lang_read_success", ns->n_batch_sub_lang_read_success);
	info_append_uint64(db, "batch_sub_lang_write_success", ns->n_batch_sub_lang_write_success);
	info_append_uint64(db, "batch_sub_lang_delete_success", ns->n_batch_sub_lang_delete_success);
	info_append_uint64(db, "batch_sub_lang_error", ns->n_batch_sub_lang_error);

	// From-proxy batch sub-transaction stats.

	info_append_uint64(db, "from_proxy_batch_sub_tsvc_error", ns->n_from_proxy_batch_sub_tsvc_error);
	info_append_uint64(db, "from_proxy_batch_sub_tsvc_timeout", ns->n_from_proxy_batch_sub_tsvc_timeout);

	info_append_uint64(db, "from_proxy_batch_sub_read_success", ns->n_from_proxy_batch_sub_read_success);
	info_append_uint64(db, "from_proxy_batch_sub_read_error", ns->n_from_proxy_batch_sub_read_error);
	info_append_uint64(db, "from_proxy_batch_sub_read_timeout", ns->n_from_proxy_batch_sub_read_timeout);
	info_append_uint64(db, "from_proxy_batch_sub_read_not_found", ns->n_from_proxy_batch_sub_read_not_found);
	info_append_uint64(db, "from_proxy_batch_sub_read_filtered_out", ns->n_from_proxy_batch_sub_read_filtered_out);

	info_append_uint64(db, "from_proxy_batch_sub_write_success", ns->n_from_proxy_batch_sub_write_success);
	info_append_uint64(db, "from_proxy_batch_sub_write_error", ns->n_from_proxy_batch_sub_write_error);
	info_append_uint64(db, "from_proxy_batch_sub_write_timeout", ns->n_from_proxy_batch_sub_write_timeout);
	info_append_uint64(db, "from_proxy_batch_sub_write_filtered_out", ns->n_from_proxy_batch_sub_write_filtered_out);

	info_append_uint64(db, "from_proxy_batch_sub_delete_success", ns->n_from_proxy_batch_sub_delete_success);
	info_append_uint64(db, "from_proxy_batch_sub_delete_error", ns->n_from_proxy_batch_sub_delete_error);
	info_append_uint64(db, "from_proxy_batch_sub_delete_timeout", ns->n_from_proxy_batch_sub_delete_timeout);
	info_append_uint64(db, "from_proxy_batch_sub_delete_not_found", ns->n_from_proxy_batch_sub_delete_not_found);
	info_append_uint64(db, "from_proxy_batch_sub_delete_filtered_out", ns->n_from_proxy_batch_sub_delete_filtered_out);

	info_append_uint64(db, "from_proxy_batch_sub_udf_complete", ns->n_from_proxy_batch_sub_udf_complete);
	info_append_uint64(db, "from_proxy_batch_sub_udf_error", ns->n_from_proxy_batch_sub_udf_error);
	info_append_uint64(db, "from_proxy_batch_sub_udf_timeout", ns->n_from_proxy_batch_sub_udf_timeout);
	info_append_uint64(db, "from_proxy_batch_sub_udf_filtered_out", ns->n_from_proxy_batch_sub_udf_filtered_out);

	info_append_uint64(db, "from_proxy_batch_sub_lang_read_success", ns->n_from_proxy_batch_sub_lang_read_success);
	info_append_uint64(db, "from_proxy_batch_sub_lang_write_success", ns->n_from_proxy_batch_sub_lang_write_success);
	info_append_uint64(db, "from_proxy_batch_sub_lang_delete_success", ns->n_from_proxy_batch_sub_lang_delete_success);
	info_append_uint64(db, "from_proxy_batch_sub_lang_error", ns->n_from_proxy_batch_sub_lang_error);

	// Internal-UDF sub-transaction stats.

	info_append_uint64(db, "udf_sub_tsvc_error", ns->n_udf_sub_tsvc_error);
	info_append_uint64(db, "udf_sub_tsvc_timeout", ns->n_udf_sub_tsvc_timeout);

	info_append_uint64(db, "udf_sub_udf_complete", ns->n_udf_sub_udf_complete);
	info_append_uint64(db, "udf_sub_udf_error", ns->n_udf_sub_udf_error);
	info_append_uint64(db, "udf_sub_udf_timeout", ns->n_udf_sub_udf_timeout);
	info_append_uint64(db, "udf_sub_udf_filtered_out", ns->n_udf_sub_udf_filtered_out);

	info_append_uint64(db, "udf_sub_lang_read_success", ns->n_udf_sub_lang_read_success);
	info_append_uint64(db, "udf_sub_lang_write_success", ns->n_udf_sub_lang_write_success);
	info_append_uint64(db, "udf_sub_lang_delete_success", ns->n_udf_sub_lang_delete_success);
	info_append_uint64(db, "udf_sub_lang_error", ns->n_udf_sub_lang_error);

	// Internal-ops sub-transaction stats.

	info_append_uint64(db, "ops_sub_tsvc_error", ns->n_ops_sub_tsvc_error);
	info_append_uint64(db, "ops_sub_tsvc_timeout", ns->n_ops_sub_tsvc_timeout);

	info_append_uint64(db, "ops_sub_write_success", ns->n_ops_sub_write_success);
	info_append_uint64(db, "ops_sub_write_error", ns->n_ops_sub_write_error);
	info_append_uint64(db, "ops_sub_write_timeout", ns->n_ops_sub_write_timeout);
	info_append_uint64(db, "ops_sub_write_filtered_out", ns->n_ops_sub_write_filtered_out);

	// Duplicate resolution stats.

	info_append_uint64(db, "dup_res_ask", ns->n_dup_res_ask);

	info_append_uint64(db, "dup_res_respond_read", ns->n_dup_res_respond_read);
	info_append_uint64(db, "dup_res_respond_no_read", ns->n_dup_res_respond_no_read);

	// Transaction retransmit stats - 'all' means both client & proxy origins.

	info_append_uint64(db, "retransmit_all_read_dup_res", ns->n_retransmit_all_read_dup_res);

	info_append_uint64(db, "retransmit_all_write_dup_res", ns->n_retransmit_all_write_dup_res);
	info_append_uint64(db, "retransmit_all_write_repl_write", ns->n_retransmit_all_write_repl_write);

	info_append_uint64(db, "retransmit_all_delete_dup_res", ns->n_retransmit_all_delete_dup_res);
	info_append_uint64(db, "retransmit_all_delete_repl_write", ns->n_retransmit_all_delete_repl_write);

	info_append_uint64(db, "retransmit_all_udf_dup_res", ns->n_retransmit_all_udf_dup_res);
	info_append_uint64(db, "retransmit_all_udf_repl_write", ns->n_retransmit_all_udf_repl_write);

	info_append_uint64(db, "retransmit_all_batch_sub_dup_res", ns->n_retransmit_all_batch_sub_dup_res);

	info_append_uint64(db, "retransmit_udf_sub_dup_res", ns->n_retransmit_udf_sub_dup_res);
	info_append_uint64(db, "retransmit_udf_sub_repl_write", ns->n_retransmit_udf_sub_repl_write);

	info_append_uint64(db, "retransmit_ops_sub_dup_res", ns->n_retransmit_ops_sub_dup_res);
	info_append_uint64(db, "retransmit_ops_sub_repl_write", ns->n_retransmit_ops_sub_repl_write);

	// Primary index query (formerly scan) stats.

	info_append_uint64(db, "pi_query_short_basic_complete", ns->n_pi_query_short_basic_complete);
	info_append_uint64(db, "pi_query_short_basic_error", ns->n_pi_query_short_basic_error);
	info_append_uint64(db, "pi_query_short_basic_timeout", ns->n_pi_query_short_basic_timeout);

	info_append_uint64(db, "pi_query_long_basic_complete", ns->n_pi_query_long_basic_complete);
	info_append_uint64(db, "pi_query_long_basic_error", ns->n_pi_query_long_basic_error);
	info_append_uint64(db, "pi_query_long_basic_abort", ns->n_pi_query_long_basic_abort);

	info_append_uint64(db, "pi_query_aggr_complete", ns->n_pi_query_aggr_complete);
	info_append_uint64(db, "pi_query_aggr_error", ns->n_pi_query_aggr_error);
	info_append_uint64(db, "pi_query_aggr_abort", ns->n_pi_query_aggr_abort);

	info_append_uint64(db, "pi_query_udf_bg_complete", ns->n_pi_query_udf_bg_complete);
	info_append_uint64(db, "pi_query_udf_bg_error", ns->n_pi_query_udf_bg_error);
	info_append_uint64(db, "pi_query_udf_bg_abort", ns->n_pi_query_udf_bg_abort);

	info_append_uint64(db, "pi_query_ops_bg_complete", ns->n_pi_query_ops_bg_complete);
	info_append_uint64(db, "pi_query_ops_bg_error", ns->n_pi_query_ops_bg_error);
	info_append_uint64(db, "pi_query_ops_bg_abort", ns->n_pi_query_ops_bg_abort);

	// Secondary index query stats.

	info_append_uint64(db, "si_query_short_basic_complete", ns->n_si_query_short_basic_complete);
	info_append_uint64(db, "si_query_short_basic_error", ns->n_si_query_short_basic_error);
	info_append_uint64(db, "si_query_short_basic_timeout", ns->n_si_query_short_basic_timeout);

	info_append_uint64(db, "si_query_long_basic_complete", ns->n_si_query_long_basic_complete);
	info_append_uint64(db, "si_query_long_basic_error", ns->n_si_query_long_basic_error);
	info_append_uint64(db, "si_query_long_basic_abort", ns->n_si_query_long_basic_abort);

	info_append_uint64(db, "si_query_aggr_complete", ns->n_si_query_aggr_complete);
	info_append_uint64(db, "si_query_aggr_error", ns->n_si_query_aggr_error);
	info_append_uint64(db, "si_query_aggr_abort", ns->n_si_query_aggr_abort);

	info_append_uint64(db, "si_query_udf_bg_complete", ns->n_si_query_udf_bg_complete);
	info_append_uint64(db, "si_query_udf_bg_error", ns->n_si_query_udf_bg_error);
	info_append_uint64(db, "si_query_udf_bg_abort", ns->n_si_query_udf_bg_abort);

	info_append_uint64(db, "si_query_ops_bg_complete", ns->n_si_query_ops_bg_complete);
	info_append_uint64(db, "si_query_ops_bg_error", ns->n_si_query_ops_bg_error);
	info_append_uint64(db, "si_query_ops_bg_abort", ns->n_si_query_ops_bg_abort);

	// Geospatial query stats:
	info_append_uint64(db, "geo_region_query_reqs", ns->geo_region_query_count);
	info_append_uint64(db, "geo_region_query_cells", ns->geo_region_query_cells);
	info_append_uint64(db, "geo_region_query_points", ns->geo_region_query_points);
	info_append_uint64(db, "geo_region_query_falsepos", ns->geo_region_query_falsepos);

	// Re-replication stats - relevant only for enterprise edition.

	info_append_uint64(db, "re_repl_success", ns->n_re_repl_success);
	info_append_uint64(db, "re_repl_error", ns->n_re_repl_error);
	info_append_uint64(db, "re_repl_timeout", ns->n_re_repl_timeout);

	// Special errors that deserve their own counters:

	info_append_uint64(db, "fail_xdr_forbidden", ns->n_fail_xdr_forbidden);
	info_append_uint64(db, "fail_key_busy", ns->n_fail_key_busy);
	info_append_uint64(db, "fail_generation", ns->n_fail_generation);
	info_append_uint64(db, "fail_record_too_big", ns->n_fail_record_too_big);
	info_append_uint64(db, "fail_client_lost_conflict", ns->n_fail_client_lost_conflict);
	info_append_uint64(db, "fail_xdr_lost_conflict", ns->n_fail_xdr_lost_conflict);

	// Special non-error counters:

	info_append_uint64(db, "deleted_last_bin", ns->n_deleted_last_bin);
}

//
// Iterate through the current namespace list and cons up a string
//

int
info_get_tree_namespace(char *name, char *subtree, cf_dyn_buf *db)
{
	as_namespace *ns = as_namespace_get_byname(subtree);

	if (! ns)   {
		cf_dyn_buf_append_string(db, "type=unknown"); // TODO - better message?
		return 0;
	}

	info_get_namespace_info(ns, db);
	info_namespace_config_get(ns->name, db);

	cf_dyn_buf_chomp(db);

	return 0;
}

int
info_get_tree_sets(char *name, char *subtree, cf_dyn_buf *db)
{
	char *set_name    = NULL;
	as_namespace *ns  = NULL;

	// if there is a subtree, get the namespace
	if (subtree && strlen(subtree) > 0) {
		// see if subtree has a sep as well
		set_name = strchr(subtree, TREE_SEP);

		// pull out namespace, and namespace name...
		if (set_name) {
			int ns_name_len = (set_name - subtree);
			char ns_name[ns_name_len + 1];
			memcpy(ns_name, subtree, ns_name_len);
			ns_name[ns_name_len] = '\0';
			ns = as_namespace_get_byname(ns_name);
			set_name++; // currently points to the TREE_SEP, which is not what we want.
		}
		else {
			ns = as_namespace_get_byname(subtree);
		}

		if (!ns) {
			cf_dyn_buf_append_string(db, "ns_type=unknown");
			return(0);
		}
	}

	// format w/o namespace is ns1:set1:prop1=val1:prop2=val2:..propn=valn;ns1:set2...;ns2:set1...;
	if (!ns) {
		for (uint32_t i = 0; i < g_config.n_namespaces; i++) {
			as_namespace_get_set_info(g_config.namespaces[i], set_name, db);
		}
	}
	// format w namespace w/o set name is ns:set1:prop1=val1:prop2=val2...propn=valn;ns:set2...;
	// format w namespace & set name is prop1=val1:prop2=val2...propn=valn;
	else {
		as_namespace_get_set_info(ns, set_name, db);
	}
	return(0);
}

int
info_get_tree_bins(char *name, char *subtree, cf_dyn_buf *db)
{
	as_namespace *ns  = NULL;

	// if there is a subtree, get the namespace
	if (subtree && strlen(subtree) > 0) {
		ns = as_namespace_get_byname(subtree);

		if (!ns) {
			cf_dyn_buf_append_string(db, "ns_type=unknown");
			return 0;
		}
	}

	// format w/o namespace is
	// ns:num-bin-names=val1,bin-names-quota=val2,name1,name2,...;ns:...
	if (!ns) {
		for (uint32_t i = 0; i < g_config.n_namespaces; i++) {
			as_namespace_get_bins_info(g_config.namespaces[i], db, true);
		}
	}
	// format w/namespace is
	// num-bin-names=val1,bin-names-quota=val2,name1,name2,...
	else {
		as_namespace_get_bins_info(ns, db, false);
	}

	return 0;
}

int
info_command_histogram(char *name, char *params, cf_dyn_buf *db)
{
	char value_str[128];
	int  value_str_len = sizeof(value_str);

	if (0 != as_info_parameter_get(params, "namespace", value_str, &value_str_len)) {
		cf_info(AS_INFO, "histogram %s command: no namespace specified", name);
		cf_dyn_buf_append_string(db, "error-no-namespace");
		return 0;
	}

	as_namespace *ns = as_namespace_get_byname(value_str);

	if (!ns) {
		cf_info(AS_INFO, "histogram %s command: unknown namespace: %s", name, value_str);
		cf_dyn_buf_append_string(db, "error-unknown-namespace");
		return 0;
	}

	value_str_len = sizeof(value_str);

	if (0 != as_info_parameter_get(params, "type", value_str, &value_str_len)) {
		cf_info(AS_INFO, "histogram %s command:", name);
		cf_dyn_buf_append_string(db, "error-no-histogram-specified");

		return 0;
	}

	// get optional set field
	char set_name_str[AS_SET_NAME_MAX_SIZE];
	int set_name_str_len = sizeof(set_name_str);
	set_name_str[0] = 0;

	if (as_info_parameter_get(params, "set", set_name_str, &set_name_str_len) == -2) {
		cf_warning(AS_INFO, "set name too long");
		cf_dyn_buf_append_string(db, "ERROR::bad-set-name");
		return 0;
	}

	as_namespace_get_hist_info(ns, set_name_str, value_str, db);

	return 0;
}


int
info_get_tree_log(char *name, char *subtree, cf_dyn_buf *db)
{
	// see if subtree has a sep as well
	int sink_id;
	char *context = strchr(subtree, TREE_SEP);
	if (context) { // this means: log/id/context ,
		*context = 0;
		context++;

		if (0 != cf_str_atoi(subtree, &sink_id)) return(-1);

		cf_log_get_level(sink_id, context, db);
	}
	else { // this means just: log/id , so get all contexts
		if (0 != cf_str_atoi(subtree, &sink_id)) return(-1);

		cf_log_get_all_levels(sink_id, db);
	}

	return(0);
}


static void
smd_show_cb(const cf_vector* items, void* udata)
{
	cf_dyn_buf* db = (cf_dyn_buf*)udata;
	uint32_t n_items = 0;

	for (uint32_t i = 0; i < cf_vector_size(items); i++) {
		as_smd_item* item = cf_vector_get_ptr(items, i);

		if (item->value == NULL) {
			continue; // ignore tombstones
		}

		n_items++;

		cf_dyn_buf_append_string(db, item->key);
		cf_dyn_buf_append_char(db, '='); // for now, not escaping
		cf_dyn_buf_append_string(db, item->value);
		cf_dyn_buf_append_char(db, ';');
	}

	if (n_items != 0) {
		cf_dyn_buf_chomp_char(db, ';');
	}
	else {
		cf_dyn_buf_append_string(db, "<empty>");
	}
}

int
info_command_smd_show(char* name, char* params, cf_dyn_buf* db)
{
	// Command format:
	// smd-show:module=sindex

	char module_str[8 + 1];
	int module_len = sizeof(module_str);
	int rv = as_info_parameter_get(params, "module", module_str, &module_len);

	if (rv == -1 || (rv == 0 && module_len == 0)) {
		cf_warning(AS_INFO, "smd-show: missing 'module'");
		INFO_ERROR_RESPONSE(db, AS_ERR_PARAMETER, "missing 'module'");
		return 0;
	}

	if (rv == -2) {
		cf_warning(AS_INFO, "smd-show: 'module' too long");
		INFO_ERROR_RESPONSE(db, AS_ERR_PARAMETER, "'module' too long");
		return 0;
	}

	if (strcasecmp(module_str, "evict") == 0) {
		as_smd_get_all(AS_SMD_MODULE_EVICT, smd_show_cb, db);
	}
	else if(strcasecmp(module_str, "roster") == 0) {
		if (as_info_error_enterprise_only()) {
			INFO_ERROR_RESPONSE(db, AS_ERR_ENTERPRISE_ONLY, "enterprise-only");
			return 0;
		}

		as_smd_get_all(AS_SMD_MODULE_ROSTER, smd_show_cb, db);
	}
	else if(strcasecmp(module_str, "security") == 0) {
		if (as_info_error_enterprise_only()) {
			INFO_ERROR_RESPONSE(db, AS_ERR_ENTERPRISE_ONLY, "enterprise-only");
			return 0;
		}

		cf_warning(AS_INFO, "smd-show: security module forbidden");
		INFO_ERROR_RESPONSE(db, AS_ERR_FORBIDDEN, "security module forbidden");
	}
	else if(strcasecmp(module_str, "sindex") == 0) {
		as_smd_get_all(AS_SMD_MODULE_SINDEX, smd_show_cb, db);
	}
	else if(strcasecmp(module_str, "truncate") == 0) {
		as_smd_get_all(AS_SMD_MODULE_TRUNCATE, smd_show_cb, db);
	}
	else if(strcasecmp(module_str, "UDF") == 0) {
		as_smd_get_all(AS_SMD_MODULE_UDF, smd_show_cb, db);
	}
	else if(strcasecmp(module_str, "XDR") == 0) {
		if (as_info_error_enterprise_only()) {
			INFO_ERROR_RESPONSE(db, AS_ERR_ENTERPRISE_ONLY, "enterprise-only");
			return 0;
		}

		as_smd_get_all(AS_SMD_MODULE_XDR, smd_show_cb, db);
	}
	else {
		cf_warning(AS_INFO, "smd-show: unknown 'module' %s", module_str);
		INFO_ERROR_RESPONSE(db, AS_ERR_PARAMETER, "unknown 'module'");
	}

	return 0;
}


int
info_get_tree_sindexes(char *name, char *subtree, cf_dyn_buf *db)
{
	char *index_name = NULL;
	as_namespace *ns = NULL;

	// if there is a subtree, get the namespace
	if (subtree && strlen(subtree) > 0) {
		// see if subtree has a sep as well
		index_name = strchr(subtree, TREE_SEP);

		// pull out namespace, and namespace name...
		if (index_name != NULL) {
			int ns_name_len = (index_name - subtree);
			char ns_name[ns_name_len + 1];
			memcpy(ns_name, subtree, ns_name_len);
			ns_name[ns_name_len] = '\0';
			ns = as_namespace_get_byname(ns_name);
			index_name++; // currently points to the TREE_SEP, which is not what we want.
		}
		else {
			ns = as_namespace_get_byname(subtree);
		}

		if (ns == NULL) {
			cf_dyn_buf_append_string(db, "ns_type=unknown");
			return 0;
		}
	}

	// format w/o namespace is:
	//    ns=ns1:set=set1:indexname=index1:prop1=val1:...:propn=valn;ns=ns1:set=set2:indexname=index2:...;ns=ns2:set=set1:...;
	if (ns == NULL) {
		for (uint32_t i = 0; i < g_config.n_namespaces; i++) {
			as_sindex_list_str(g_config.namespaces[i], false, db);
		}

		cf_dyn_buf_chomp_char(db, ';');
	}
	// format w namespace w/o index name is:
	//    ns=ns1:set=set1:indexname=index1:prop1=val1:...:propn=valn;ns=ns1:set=set2:indexname=indexname2:...;
	else if (index_name == NULL) {
		as_sindex_list_str(ns, false, db);
		cf_dyn_buf_chomp_char(db, ';');
	}
	else {
		// format w namespace & index name is:
		//    prop1=val1;prop2=val2;...;propn=valn
		if (! as_sindex_stats_str(ns, index_name, db)) {
			cf_warning(AS_INFO, "failed to get statistics for index %s: not found",
					index_name);
			INFO_FAIL_RESPONSE(db, AS_ERR_SINDEX_NOT_FOUND, "no-index");
		}
	}
	return 0;
}

typedef struct find_sindex_key_udata_s {
	const char* ns_name;
	const char* index_name;
	const char* smd_key;
	char* found_key; // only when unique
	uint32_t n_name_matches;
	uint32_t n_indexes;
	bool has_smd_key;
} find_sindex_key_udata;

static void
find_sindex_key(const cf_vector* items, void* udata)
{
	find_sindex_key_udata* fsk = (find_sindex_key_udata*)udata;
	uint32_t ns_name_len = strlen(fsk->ns_name);

	fsk->found_key = NULL;
	fsk->n_name_matches = 0;
	fsk->n_indexes = 0;
	fsk->has_smd_key = false;

	for (uint32_t i = 0; i < cf_vector_size(items); i++) {
		as_smd_item* item = cf_vector_get_ptr(items, i);

		if (item->value == NULL) {
			continue; // ignore tombstones
		}

		const char* smd_ns_name_end = strchr(item->key, '|');

		if (smd_ns_name_end == NULL) {
			cf_warning(AS_INFO, "unexpected sindex key format '%s'", item->key);
			continue;
		}

		uint32_t smd_ns_name_len = smd_ns_name_end - item->key;

		if (smd_ns_name_len != ns_name_len ||
				memcmp(item->key, fsk->ns_name, ns_name_len) != 0) {
			continue;
		}

		fsk->n_indexes++;

		if (fsk->smd_key != NULL && strcmp(fsk->smd_key, item->key) == 0) {
			fsk->has_smd_key = true;
			fsk->smd_key = NULL; // can only be one
		}

		if (strcmp(fsk->index_name, item->value) != 0) {
			continue;
		}

		fsk->n_name_matches++;

		if (fsk->n_name_matches == 1) {
			fsk->found_key = strdup(item->key);
		}
		else {
			cf_free(fsk->found_key); // only return when unique
			fsk->found_key = NULL;
		}
	}
}

int
info_command_sindex_create(char *name, char *params, cf_dyn_buf *db)
{
	// Command format:
	// sindex-create:ns=usermap;set=demo;indexname=um_age;indextype=list;indexdata=age,numeric
	// sindex-create:ns=usermap;set=demo;indexname=um_state;indexdata=state,string
	// sindex-create:ns=usermap;set=demo;indexname=um_highscore;context=<base64-cdt-ctx>;indexdata=scores,numeric

	char index_name_str[INAME_MAX_SZ];
	int index_name_len = sizeof(index_name_str);
	int rv = as_info_parameter_get(params, "indexname", index_name_str,
			&index_name_len);

	if (rv == -1 || (rv == 0 && index_name_len == 0)) {
		cf_warning(AS_INFO, "sindex-create: missing 'indexname'");
		INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "missing 'indexname'");
		return 0;
	}

	if (rv == -2) {
		cf_warning(AS_INFO, "sindex-create: 'indexname' too long");
		INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "'indexname' too long");
		return 0;
	}

	char ns_str[AS_ID_NAMESPACE_SZ];
	int ns_len = sizeof(ns_str);

	rv = as_info_parameter_get(params, "ns", ns_str, &ns_len);

	if (rv == -1 || (rv == 0 && ns_len == 0)) {
		cf_warning(AS_INFO, "sindex-create %s: missing 'ns'", index_name_str);
		INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "missing 'ns'");
		return 0;
	}

	if (rv == -2) {
		cf_warning(AS_INFO, "sindex-create %s: 'ns' too long", index_name_str);
		INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "'ns' too long");
		return 0;
	}

	char set_str[AS_SET_NAME_MAX_SIZE];
	char* p_set_str = NULL;
	int set_len = sizeof(set_str);

	rv = as_info_parameter_get(params, "set", set_str, &set_len);

	if (rv == 0) {
		if (set_len == 0) {
			cf_warning(AS_INFO, "sindex-create %s: zero-length 'set'",
					index_name_str);
			INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "zero-length 'set'");
			return 0;
		}

		p_set_str = set_str;
	}
	else if (rv == -2) {
		cf_warning(AS_INFO, "sindex-create %s: 'set' too long", index_name_str);
		INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "'set' too long");
		return 0;
	}

	char ctx_b64[CTX_B64_MAX_SZ];
	int ctx_b64_len = sizeof(ctx_b64);
	const char* p_cdt_ctx = NULL;

	rv = as_info_parameter_get(params, "context", ctx_b64, &ctx_b64_len);

	if (rv == 0) {
		uint8_t* buf;
		int32_t buf_sz = as_sindex_cdt_ctx_b64_decode(ctx_b64, ctx_b64_len,
				&buf);

		if (buf_sz > 0) {
			cf_free(buf);
		}
		else if (buf_sz < 0) {
			switch (buf_sz) {
			case -1:
				cf_warning(AS_INFO, "sindex-create %s: 'context' invalid base64",
						index_name_str);
				INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "'context' invalid base64");
				return 0;
			case -2:
				cf_warning(AS_INFO, "sindex-create %s: 'context' invalid cdt context",
						index_name_str);
				INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "'context' invalid cdt context");
				return 0;
			case -3:
				cf_warning(AS_INFO, "sindex-create %s: 'context' not normalized msgpack",
						index_name_str);
				INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "'context' not normalized msgpack");
				return 0;
			default:
				cf_crash(AS_INFO, "unreachable");
			}
		}

		p_cdt_ctx = ctx_b64;
	}
	else if (rv == -2) {
		cf_warning(AS_INFO, "sindex-create %s: 'context' too long",
				index_name_str);
		INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "'context' too long");
		return 0;
	}

	char indextype_str[INDEXTYPE_MAX_SZ];
	int indtype_len = sizeof(indextype_str);
	as_sindex_type itype;

	rv = as_info_parameter_get(params, "indextype", indextype_str,
			&indtype_len);

	if (rv == -1) {
		// If not specified, the index type is DEFAULT.
		itype = AS_SINDEX_ITYPE_DEFAULT;
	}
	else if (rv == -2) {
		cf_warning(AS_INFO, "sindex-create %s: 'indextype' too long",
				index_name_str);
		INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "'indextype' too long");
		return 0;
	}
	else {
		itype = as_sindex_itype_from_string(indextype_str);

		if (itype == AS_SINDEX_N_ITYPES) {
			cf_warning(AS_INFO, "sindex-create %s: bad 'indextype' '%s'",
					index_name_str, indextype_str);
			INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "bad 'indextype' - must be one of 'default', 'list', 'mapkeys', 'mapvalues'");
			return 0;
		}
	}

	// indexdata=bin-name,keytype
	char indexdata_str[INDEXDATA_MAX_SZ];
	int indexdata_len = sizeof(indexdata_str);

	rv = as_info_parameter_get(params, "indexdata", indexdata_str,
			&indexdata_len);

	if (rv == -1 || (rv == 0 && indexdata_len == 0)) {
		cf_warning(AS_INFO, "sindex-create %s: missing 'indexdata'",
				index_name_str);
		INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "missing 'indexdata'");
		return 0;
	}

	if (rv == -2) {
		cf_warning(AS_INFO, "sindex-create %s: 'indexdata' too long",
				index_name_str);
		INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "'indexdata' too long");
		return 0;
	}

	char* bin_name = indexdata_str;
	char* type_str = strchr(indexdata_str, ',');

	if (type_str == NULL) {
		cf_warning(AS_INFO, "sindex-create %s: 'indexdata' missing bin type",
				index_name_str);
		INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "'indexdata' missing bin type");
		return 0;
	}

	*type_str++ = '\0';

	if (bin_name[0] == '\0') {
		cf_warning(AS_INFO, "sindex-create %s: 'indexdata' missing bin name",
				index_name_str);
		INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "'indexdata' missing bin name");
		return 0;
	}

	if (strlen(bin_name) >= AS_BIN_NAME_MAX_SZ) {
		cf_warning(AS_INFO, "sindex-create %s: 'indexdata' bin name too long",
				index_name_str);
		INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "'indexdata' bin name too long");
		return 0;
	}

	as_particle_type ktype = as_sindex_ktype_from_string(type_str);

	if (ktype == AS_PARTICLE_TYPE_BAD) {
		cf_warning(AS_INFO, "sindex-create %s: bad 'indexdata' bin type '%s'",
				index_name_str, type_str);
		INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "bad 'indexdata' bin type - must be one of 'numeric', 'string', 'geo2dsphere'");
		return 0;
	}

	cf_info(AS_INFO, "sindex-create: request received for %s:%s via info",
			ns_str, index_name_str);

	char smd_key[SINDEX_SMD_KEY_MAX_SZ];

	as_sindex_build_smd_key(ns_str, p_set_str, bin_name, p_cdt_ctx, itype,
			ktype, smd_key);

	find_sindex_key_udata fsk = {
			.ns_name = ns_str,
			.index_name = index_name_str,
			.smd_key = smd_key
	};

	as_smd_get_all(AS_SMD_MODULE_SINDEX, find_sindex_key, &fsk);

	if (fsk.found_key != NULL) {
		if (strcmp(fsk.found_key, smd_key) != 0) {
			cf_free(fsk.found_key);
			cf_warning(AS_INFO, "sindex-create %s:%s: 'indexname' already exists with different definition",
					ns_str, index_name_str);
			INFO_FAIL_RESPONSE(db, AS_ERR_SINDEX_FOUND, "'indexname' already exists with different definition");
			return 0;
		}

		cf_free(fsk.found_key);
		cf_info(AS_INFO, "sindex-create %s:%s: 'indexname' and defintion already exists",
				ns_str, index_name_str);
		cf_dyn_buf_append_string(db, "OK");
		return 0;
	}

	if (fsk.n_name_matches > 1) {
		cf_warning(AS_INFO, "sindex-create %s:%s: 'indexname' already exists with %u definitions - rename(s) required",
				ns_str, index_name_str, fsk.n_name_matches);
		INFO_FAIL_RESPONSE(db, AS_ERR_SINDEX_FOUND, "'indexname' already exists with multiple definitions");
		return 0;
	}

	if (! fsk.has_smd_key && fsk.n_indexes >= MAX_N_SINDEXES) {
		cf_warning(AS_INFO, "sindex-create %s:%s: already at sindex definition limit",
				ns_str, index_name_str);
		INFO_FAIL_RESPONSE(db, AS_ERR_SINDEX_MAX_COUNT, "already at sindex definition limit");
		return 0;
	}

	if (! as_smd_set_blocking(AS_SMD_MODULE_SINDEX, smd_key, index_name_str,
			0)) {
		cf_warning(AS_INFO, "sindex-create: timeout while creating %s:%s in SMD",
				ns_str, index_name_str);
		INFO_FAIL_RESPONSE(db, AS_ERR_TIMEOUT, "timeout");
		return 0;
	}

	cf_dyn_buf_append_string(db, "OK");

	return 0;
}

int
info_command_sindex_delete(char *name, char *params, cf_dyn_buf *db)
{
	// Command format:
	// sindex-delete:ns=usermap;set=demo;indexname=um_state

	char index_name_str[INAME_MAX_SZ];
	int index_name_len = sizeof(index_name_str);
	int ret = as_info_parameter_get(params, "indexname", index_name_str,
			&index_name_len);

	if (ret == -1 || (ret == 0 && index_name_len == 0)) {
		cf_warning(AS_INFO, "sindex-delete: missing 'indexname'");
		INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "missing 'indexname'");
		return 0;
	}

	if (ret == -2) {
		cf_warning(AS_INFO, "sindex-delete: 'indexname' too long");
		INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "'indexname' too long");
		return 0;
	}

	char ns_str[AS_ID_NAMESPACE_SZ];
	int ns_len = sizeof(ns_str);

	ret = as_info_parameter_get(params, "ns", ns_str, &ns_len);

	if (ret == -1 || (ret == 0 && ns_len == 0)) {
		cf_warning(AS_INFO, "sindex-delete %s: missing 'ns'",
				index_name_str);
		INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "missing 'ns'");
		return 0;
	}

	if (ret == -2) {
		cf_warning(AS_INFO, "sindex-delete %s: 'ns' too long", index_name_str);
		INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "'ns' too long");
		return 0;
	}

	cf_info(AS_INFO, "sindex-delete: request received for %s:%s via info",
			ns_str, index_name_str);

	find_sindex_key_udata fsk = {
			.ns_name = ns_str,
			.index_name = index_name_str
	};

	as_smd_get_all(AS_SMD_MODULE_SINDEX, find_sindex_key, &fsk);

	if (fsk.found_key == NULL) {
		if (fsk.n_name_matches == 0) {
			cf_info(AS_INFO, "sindex-delete: 'indexname' %s not found",
					fsk.index_name);
			cf_dyn_buf_append_string(db, "OK");
			return 0;
		}

		cf_warning(AS_INFO, "sindex-delete: 'indexname' %s not unique - found %u matches - rename(s) required",
				fsk.index_name, fsk.n_name_matches);
		INFO_FAIL_RESPONSE(db, AS_ERR_SINDEX_FOUND, "'indexname' is not unique");
		return 0;
	}

	if (! as_smd_delete_blocking(AS_SMD_MODULE_SINDEX, fsk.found_key, 0)) {
		cf_free(fsk.found_key);
		cf_warning(AS_INFO, "sindex-delete: timeout while dropping %s:%s in SMD",
				ns_str, index_name_str);
		INFO_FAIL_RESPONSE(db, AS_ERR_TIMEOUT, "timeout");
		return 0;
	}

	cf_free(fsk.found_key);

	cf_dyn_buf_append_string(db, "OK");

	return 0;
}

int
info_command_sindex_exists(char *name, char *params, cf_dyn_buf *db)
{
	// Command format:
	// sindex-exists:ns=usermap;indexname=um_state

	char index_name_str[INAME_MAX_SZ];
	int index_name_len = sizeof(index_name_str);
	int ret = as_info_parameter_get(params, "indexname", index_name_str,
			&index_name_len);

	if (ret == -1 || (ret == 0 && index_name_len == 0)) {
		cf_warning(AS_INFO, "sindex-exists: missing 'indexname'");
		INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "missing 'indexname'");
		return 0;
	}

	if (ret == -2) {
		cf_warning(AS_INFO, "sindex-exists: 'indexname' too long");
		INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "'indexname' too long");
		return 0;
	}

	char ns_str[AS_ID_NAMESPACE_SZ];
	int ns_len = sizeof(ns_str);

	ret = as_info_parameter_get(params, "ns", ns_str, &ns_len);

	if (ret == -1 || (ret == 0 && ns_len == 0)) {
		cf_warning(AS_INFO, "sindex-exists %s: missing 'ns'", index_name_str);
		INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "missing 'ns'");
		return 0;
	}

	if (ret == -2) {
		cf_warning(AS_INFO, "sindex-exists %s: 'ns' too long", index_name_str);
		INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "'ns' too long");
		return 0;
	}

	const as_namespace* ns = as_namespace_get_byname(ns_str);

	cf_dyn_buf_append_string(db, as_sindex_exists(ns, index_name_str) ?
			"true" : "false");

	return 0;
}

int
as_info_parse_ns_iname(char* params, as_namespace** ns, char** iname,
		cf_dyn_buf* db, char* sindex_cmd)
{
	char ns_str[AS_ID_NAMESPACE_SZ];
	int ns_len = sizeof(ns_str);
	int ret = 0;

	ret = as_info_parameter_get(params, "ns", ns_str, &ns_len);

	if (ret) {
		if (ret == -2) {
			cf_warning(AS_INFO, "%s : namespace name exceeds max length %d",
				sindex_cmd, AS_ID_NAMESPACE_SZ);
			INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER,
				"Namespace name exceeds max length");
		}
		else {
			cf_warning(AS_INFO, "%s : invalid namespace %s", sindex_cmd, ns_str);
			INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER,
				"Namespace Not Specified");
		}
		return -1;
	}

	*ns = as_namespace_get_byname(ns_str);
	if (!*ns) {
		cf_warning(AS_INFO, "%s : namespace %s not found", sindex_cmd, ns_str);
		INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "Namespace Not Found");
		return -1;
	}

	// get indexname
	char index_name_str[INAME_MAX_SZ];
	int  index_len = sizeof(index_name_str);
	ret = as_info_parameter_get(params, "indexname", index_name_str, &index_len);
	if (ret) {
		if (ret == -2) {
			cf_warning(AS_INFO, "%s : indexname exceeds max length %d", sindex_cmd, INAME_MAX_SZ);
			INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER,
				"Index Name exceeds max length");
		}
		else {
			cf_warning(AS_INFO, "%s : invalid indexname %s", sindex_cmd, index_name_str);
			INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER,
				"Index Name Not Specified");
		}
		return -1;
	}

	cf_info(AS_SINDEX, "%s : received request on index %s - namespace %s",
			sindex_cmd, index_name_str, ns_str);

	*iname = cf_strdup(index_name_str);

	return 0;
}

// Note - a bit different to 'query-list' which collects less info.
// TODO - remove 'query-list'?
int
info_query_show(char *name, cf_dyn_buf *db)
{
	(void)name;

	as_mon_info_cmd(AS_MON_MODULES[QUERY_MOD], NULL, 0, 0, db);

	return 0;
}

int
info_command_query_show(char *name, char *params, cf_dyn_buf *db)
{
	(void)name;

	char trid_str[1 + 24 + 1] = { 0 }; // allow octal, decimal, hex
	int trid_str_len = (int)sizeof(trid_str);
	int rv = as_info_parameter_get(params, "trid", trid_str, &trid_str_len);

	if (rv == -2) {
		cf_warning(AS_INFO, "trid too long");
		cf_dyn_buf_append_string(db, "ERROR::bad-trid");
		return 0;
	}

	if (rv == -1) { // no trid specified - show all
		as_mon_info_cmd(AS_MON_MODULES[QUERY_MOD], NULL, 0, 0, db);
		return 0;
	}

	uint64_t trid = 0;

	if (cf_strtoul_u64_raw(trid_str, &trid) != 0 || trid == 0) {
		cf_warning(AS_INFO, "bad trid %s", trid_str);
		cf_dyn_buf_append_string(db, "ERROR::bad-trid");
		return 0;
	}

	as_mon_info_cmd(AS_MON_MODULES[QUERY_MOD], "get-job", trid, 0, db);

	return 0;
}

static int
info_command_abort_query(char *name, char *params, cf_dyn_buf *db)
{
	(void)name;

	char trid_str[1 + 24 + 1] = { 0 }; // allow octal, decimal, hex
	int trid_str_len = (int)sizeof(trid_str);
	int rv = as_info_parameter_get(params, "trid", trid_str, &trid_str_len);

	if (rv == -2) {
		cf_warning(AS_INFO, "trid too long");
		cf_dyn_buf_append_string(db, "ERROR::bad-trid");
		return 0;
	}

	// Allow 'id' for backward compatibility of scan-abort. Remove in 6 months.
	if (rv == -1) {
		rv = as_info_parameter_get(params, "id", trid_str, &trid_str_len);

		if (rv == -2) {
			cf_warning(AS_INFO, "id too long");
			cf_dyn_buf_append_string(db, "ERROR::bad-trid");
			return 0;
		}
	}

	if (rv == -1) {
		cf_warning(AS_INFO, "trid missing");
		cf_dyn_buf_append_string(db, "ERROR::trid-missing");
		return 0;
	}

	uint64_t trid = 0;

	if (cf_strtoul_u64_raw(trid_str, &trid) != 0 || trid == 0) {
		cf_warning(AS_INFO, "bad trid %s", trid_str);
		cf_dyn_buf_append_string(db, "ERROR::bad-trid");
		return 0;
	}

	if (as_query_abort(trid)) {
		cf_dyn_buf_append_string(db, "OK");
		return 0;
	}

	cf_dyn_buf_append_string(db, "ERROR:");
	cf_dyn_buf_append_int(db, AS_ERR_NOT_FOUND);
	cf_dyn_buf_append_string(db, ":trid-not-active");

	return 0;
}

int info_command_abort_all_queries(char *name, char *params, cf_dyn_buf *db) {

	uint32_t n_queries_killed = as_query_abort_all();

	cf_dyn_buf_append_string(db, "OK - number of queries killed: ");
	cf_dyn_buf_append_uint32(db, n_queries_killed);

	return 0;
}

int info_command_sindex_stat(char *name, char *params, cf_dyn_buf *db) {
	as_namespace  *ns = NULL;
	char * iname = NULL;

	if (as_info_parse_ns_iname(params, &ns, &iname, db, "SINDEX STAT")) {
		return 0;
	}

	if (! as_sindex_stats_str(ns, iname, db))  {
		cf_warning(AS_INFO, "SINDEX STAT : index %s not found for ns %s",
			iname, ns->name);
		INFO_FAIL_RESPONSE(db, AS_ERR_SINDEX_NOT_FOUND, "NO INDEX");
	}

	if (iname) {
		cf_free(iname);
	}
	return(0);
}

int
info_command_sindex_list(char *name, char *params, cf_dyn_buf *db)
{
	bool all_ns = true;
	char ns_str[128];
	int ns_len = sizeof(ns_str);

	if (as_info_parameter_get(params, "ns", ns_str, &ns_len) == 0) {
		all_ns = false;
	}

	char b64_str[6];
	int b64_len = sizeof(b64_str);
	int rv = as_info_parameter_get(params, "b64", b64_str, &b64_len);
	bool b64 = false;

	if (rv == -2) {
		cf_warning(AS_INFO, "b64 parameter value too long");
		INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "bad-b64");
		return 0;
	}

	if (rv == 0) {
		if (strcmp(b64_str, "true") == 0) {
			b64 = true;
		}
		else if (strcmp(b64_str, "false") == 0) {
			b64 = false;
		}
		else {
			cf_warning(AS_INFO, "b64 value invalid");
			INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "bad-b64");
			return 0;
		}
	}

	if (all_ns) {
		for (uint32_t ns_ix = 0; ns_ix < g_config.n_namespaces; ns_ix++) {
			as_sindex_list_str(g_config.namespaces[ns_ix], b64, db);
		}

		cf_dyn_buf_chomp_char(db, ';');
	}
	else {
		as_namespace *ns = as_namespace_get_byname(ns_str);

		if (ns == NULL) {
			cf_warning(AS_INFO, "SINDEX LIST : ns %s not found", ns_str);
			INFO_FAIL_RESPONSE(db, AS_ERR_PARAMETER, "namespace not found");
		}
		else {
			as_sindex_list_str(ns, b64, db);
			cf_dyn_buf_chomp_char(db, ';');
		}
	}

	return 0;
}

// Defined in "make_in/version.c" (auto-generated by the build system.)
extern const char aerospike_build_id[];
extern const char aerospike_build_time[];
extern const char aerospike_build_type[];
extern const char aerospike_build_os[];
extern const char aerospike_build_features[];

void
as_info_init()
{
	// create worker threads
	g_info_work_q = cf_queue_create(sizeof(as_info_transaction), true);

	char vstr[64];
	sprintf(vstr, "%s build %s", aerospike_build_type, aerospike_build_id);

	char compatibility_id[20];
	cf_str_itoa(AS_EXCHANGE_COMPATIBILITY_ID, compatibility_id, 10);

	// Set some basic values
	as_info_set("version", vstr, true);                  // Returns the edition and build number.
	as_info_set("build", aerospike_build_id, true);      // Returns the build number for this server.
	as_info_set("build_os", aerospike_build_os, true);   // Return the OS used to create this build.
	as_info_set("build_time", aerospike_build_time, true); // Return the creation time of this build.
	as_info_set("edition", aerospike_build_type, true);  // Return the edition of this build.
	as_info_set("compatibility-id", compatibility_id, true); // Used for compatibility purposes.
	as_info_set("digests", "RIPEMD160", false);          // Returns the hashing algorithm used by the server for key hashing.
	as_info_set("status", "ok", false);                  // Always returns ok, used to verify service port is open.
	as_info_set("STATUS", "OK", false);                  // Always returns OK, used to verify service port is open.

	char istr[1024];
	cf_str_itoa(AS_PARTITIONS, istr, 10);
	as_info_set("partitions", istr, false);              // Returns the number of partitions used to hash keys across.

	cf_str_itoa_u64(g_config.self_node, istr, 16);
	as_info_set("node", istr, true);                     // Node ID. Unique 15 character hex string for each node based on the mac address and port.
	as_info_set("name", istr, false);                    // Alias to 'node'.

	// Returns list of features supported by this server
	static char features[1024];
	strcat(features,
			"batch-any;batch-index;blob-bits;"
			"cdt-list;cdt-map;cluster-stable;"
			"float;"
			"geo;"
			"sindex-exists;"
			"peers;pipelining;pquery;pscans;"
			"query-show;"
			"relaxed-sc;replicas;replicas-all;replicas-master;replicas-max;"
			"truncate-namespace;"
			"udf");
	strcat(features, aerospike_build_features);
	as_info_set("features", features, true);

	as_hb_mode hb_mode;
	as_hb_info_listen_addr_get(&hb_mode, istr, sizeof(istr));
	as_info_set(hb_mode == AS_HB_MODE_MESH ? "mesh" :  "mcast", istr, false);

	// Commands expected via asinfo/telnet. If it's not in this list, it's a
	// "client-only" command, e.g. for cluster management.
	as_info_set("help",
			"best-practices;bins;build;build_os;build_time;"
			"cluster-name;config-get;config-set;"
			"digests;dump-cluster;dump-fabric;dump-hb;dump-hlc;dump-migrates;"
			"dump-msgs;dump-rw;dump-si;dump-skew;dump-wb-summary;"
			"eviction-reset;"
			"feature-key;"
			"get-config;get-sl;"
			"health-outliers;health-stats;histogram;"
			"jem-stats;jobs;"
			"latencies;log;log-set;log-message;logs;"
			"mcast;mesh;"
			"name;namespace;namespaces;node;"
			"physical-devices;"
			"query-abort;query-abort-all;query-show;quiesce;quiesce-undo;"
			"racks;recluster;revive;roster;roster-set;"
			"scan-abort;scan-abort-all;scan-show;service;services;"
			"services-alumni;services-alumni-reset;set-config;set-log;sets;"
			"show-devices;sindex;sindex-create;sindex-delete;smd-show;"
			"statistics;status;"
			"tip;tip-clear;truncate;truncate-namespace;truncate-namespace-undo;"
			"truncate-undo;"
			"version;",
			false);

	// Set up some dynamic functions
	as_info_set_dynamic("alumni-clear-std", as_service_list_dynamic, false);          // Supersedes "services-alumni" for non-TLS service.
	as_info_set_dynamic("alumni-tls-std", as_service_list_dynamic, false);            // Supersedes "services-alumni" for TLS service.
	as_info_set_dynamic("best-practices", info_get_best_practices, false);            // Returns best-practices information.
	as_info_set_dynamic("bins", info_get_bins, false);                                // Returns bin usage information and used bin names.
	as_info_set_dynamic("cluster-name", info_get_cluster_name, false);                // Returns cluster name.
	as_info_set_dynamic("endpoints", info_get_endpoints, false);                      // Returns the expanded bind / access address configuration.
	as_info_set_dynamic("feature-key", info_get_features, false);                     // Returns the contents of the feature key (except signature).
	as_info_set_dynamic("get-config", info_get_config, false);                        // Returns running config for specified context.
	as_info_set_dynamic("health-outliers", info_get_health_outliers, false);          // Returns a list of outliers.
	as_info_set_dynamic("health-stats", info_get_health_stats, false);                // Returns health stats.
	as_info_set_dynamic("index-pressure", info_get_index_pressure, false);            // Number of resident and dirty AF index pages.
	as_info_set_dynamic("logs", info_get_logs, false);                                // Returns a list of log file locations in use by this server.
	as_info_set_dynamic("namespaces", info_get_namespaces, false);                    // Returns a list of namespace defined on this server.
	as_info_set_dynamic("objects", info_get_objects, false);                          // Returns the number of objects stored on this server.
	as_info_set_dynamic("partition-generation", info_get_partition_generation, true); // Returns the current partition generation.
	as_info_set_dynamic("partition-info", info_get_partition_info, false);            // Returns partition ownership information.
	as_info_set_dynamic("peers-clear-alt", as_service_list_dynamic, false);           // Supersedes "services-alternate" for non-TLS, alternate addresses.
	as_info_set_dynamic("peers-clear-std", as_service_list_dynamic, false);           // Supersedes "services" for non-TLS, standard addresses.
	as_info_set_dynamic("peers-generation", as_service_list_dynamic, false);          // Returns the generation of the peers-*-* services lists.
	as_info_set_dynamic("peers-tls-alt", as_service_list_dynamic, false);             // Supersedes "services-alternate" for TLS, alternate addresses.
	as_info_set_dynamic("peers-tls-std", as_service_list_dynamic, false);             // Supersedes "services" for TLS, standard addresses.
	as_info_set_dynamic("rack-ids", info_get_rack_ids, false);                        // Effective rack-ids for all namespaces on this node.
	as_info_set_dynamic("rebalance-generation", info_get_rebalance_generation, false); // How many rebalances we've done.
	as_info_set_dynamic("replicas", info_get_replicas, false);                        // Same as replicas-all, but includes regime.
	as_info_set_dynamic("replicas-all", info_get_replicas_all, false);                // Base 64 encoded binary representation of partitions this node is replica for.
	as_info_set_dynamic("replicas-master", info_get_replicas_master, false);          // Base 64 encoded binary representation of partitions this node is master (replica) for.
	as_info_set_dynamic("service", as_service_list_dynamic, false);                   // IP address and server port for this node, expected to be a single.
	                                                                                  // address/port per node, may be multiple address if this node is configured.
	                                                                                  // to listen on multiple interfaces (typically not advised).
	as_info_set_dynamic("service-clear-alt", as_service_list_dynamic, false);         // Supersedes "service". The alternate address and port for this node's non-TLS
	                                                                                  // client service.
	as_info_set_dynamic("service-clear-std", as_service_list_dynamic, false);         // Supersedes "service". The address and port for this node's non-TLS client service.
	as_info_set_dynamic("service-tls-alt", as_service_list_dynamic, false);           // Supersedes "service". The alternate address and port for this node's TLS
	                                                                                  // client service.
	as_info_set_dynamic("service-tls-std", as_service_list_dynamic, false);           // Supersedes "service". The address and port for this node's TLS client service.
	as_info_set_dynamic("services", as_service_list_dynamic, true);                   // List of addresses of neighbor cluster nodes to advertise for Application to connect.
	as_info_set_dynamic("services-alternate", as_service_list_dynamic, false);        // IP address mapping from internal to public ones
	as_info_set_dynamic("services-alumni", as_service_list_dynamic, true);            // All neighbor addresses (services) this server has ever know about.
	as_info_set_dynamic("services-alumni-reset", as_service_list_dynamic, false);     // Reset the services alumni to equal services.
	as_info_set_dynamic("sets", info_get_sets, false);                                // Returns set statistics for all or a particular set.
	as_info_set_dynamic("smd-info", info_get_smd_info, false);                        // Returns SMD state information.
	as_info_set_dynamic("statistics", info_get_stats, true);                          // Returns system health and usage stats for this server.
	as_info_set_dynamic("thread-traces", cf_thread_traces, false);                    // Returns backtraces for all threads.

	// Tree-based names
	as_info_set_tree("bins", info_get_tree_bins);           // Returns bin usage information and used bin names for all or a particular namespace.
	as_info_set_tree("log", info_get_tree_log);             //
	as_info_set_tree("namespace", info_get_tree_namespace); // Returns health and usage stats for a particular namespace.
	as_info_set_tree("sets", info_get_tree_sets);           // Returns set statistics for all or a particular set.

	// Define commands
	as_info_set_command("cluster-stable", info_command_cluster_stable, PERM_NONE);            // Returns cluster key if cluster is stable.
	as_info_set_command("config-get", info_command_config_get, PERM_NONE);                    // Returns running config for specified context.
	as_info_set_command("config-set", info_command_config_set, PERM_SET_CONFIG);              // Set a configuration parameter at run time, configuration parameter must be dynamic.
	as_info_set_command("dump-cluster", info_command_dump_cluster, PERM_LOGGING_CTRL);        // Print debug information about clustering and exchange to the log file.
	as_info_set_command("dump-fabric", info_command_dump_fabric, PERM_LOGGING_CTRL);          // Print debug information about fabric to the log file.
	as_info_set_command("dump-hb", info_command_dump_hb, PERM_LOGGING_CTRL);                  // Print debug information about heartbeat state to the log file.
	as_info_set_command("dump-hlc", info_command_dump_hlc, PERM_LOGGING_CTRL);                // Print debug information about Hybrid Logical Clock to the log file.
	as_info_set_command("dump-migrates", info_command_dump_migrates, PERM_LOGGING_CTRL);      // Print debug information about migration.
	as_info_set_command("dump-rw", info_command_dump_rw_request_hash, PERM_LOGGING_CTRL);     // Print debug information about transaction hash table to the log file.
	as_info_set_command("dump-skew", info_command_dump_skew, PERM_LOGGING_CTRL);              // Print information about clock skew
	as_info_set_command("dump-wb-summary", info_command_dump_wb_summary, PERM_LOGGING_CTRL);  // Print summary information about all Write Blocks (WB) on a device to the log file.
	as_info_set_command("eviction-reset", info_command_eviction_reset, PERM_EVICT_ADMIN);     // Delete or manually set SMD evict-void-time.
	as_info_set_command("get-config", info_command_config_get, PERM_NONE);                    // Returns running config for all or a particular context.
	as_info_set_command("get-sl", info_command_get_sl, PERM_NONE);                            // Get the Paxos succession list.
	as_info_set_command("get-stats", info_command_get_stats, PERM_NONE);                      // Returns statistics for a particular context.
	as_info_set_command("histogram", info_command_histogram, PERM_NONE);                      // Returns a histogram snapshot for a particular histogram.
	as_info_set_command("jem-stats", info_command_jem_stats, PERM_LOGGING_CTRL);              // Print JEMalloc statistics to the log file.
	as_info_set_command("latencies", info_command_latencies, PERM_NONE);                      // Returns latency and throughput information.
	as_info_set_command("log-message", info_command_log_message, PERM_LOGGING_CTRL);          // Log a message.
	as_info_set_command("log-set", info_command_log_set, PERM_LOGGING_CTRL);                  // Set values in the log system.
	as_info_set_command("peers-clear-alt", as_service_list_command, PERM_NONE);               // The delta update version of "peers-clear-alt".
	as_info_set_command("peers-clear-std", as_service_list_command, PERM_NONE);               // The delta update version of "peers-clear-std".
	as_info_set_command("peers-tls-alt", as_service_list_command, PERM_NONE);                 // The delta update version of "peers-tls-alt".
	as_info_set_command("peers-tls-std", as_service_list_command, PERM_NONE);                 // The delta update version of "peers-tls-std".
	as_info_set_command("physical-devices", info_command_physical_devices, PERM_NONE);        // Physical device information.
	as_info_set_command("quiesce", info_command_quiesce, PERM_SERVICE_CTRL);                  // Quiesce this node.
	as_info_set_command("quiesce-undo", info_command_quiesce_undo, PERM_SERVICE_CTRL);        // Un-quiesce this node.
	as_info_set_command("racks", info_command_racks, PERM_NONE);                              // Rack-aware information.
	as_info_set_command("recluster", info_command_recluster, PERM_SERVICE_CTRL);              // Force cluster to re-form.
	as_info_set_command("replicas", info_command_replicas, PERM_NONE);                        // Same as 'dynamic' replicas, but with 'max' param.
	as_info_set_command("revive", info_command_revive, PERM_SERVICE_CTRL);                    // Mark "untrusted" partitions as "revived".
	as_info_set_command("roster", info_command_roster, PERM_NONE);                            // Roster information.
	as_info_set_command("roster-set", info_command_roster_set, PERM_SERVICE_CTRL);            // Set the entire roster.
	as_info_set_command("set-config", info_command_config_set, PERM_SET_CONFIG);              // Set config values.
	as_info_set_command("set-log", info_command_log_set, PERM_LOGGING_CTRL);                  // Set values in the log system.
	as_info_set_command("smd-show", info_command_smd_show, PERM_NONE);                        // Debug command to show raw SMD info for any module except security.
	as_info_set_command("tip", info_command_tip, PERM_SERVICE_CTRL);                          // Add external IP to mesh-mode heartbeats.
	as_info_set_command("tip-clear", info_command_tip_clear, PERM_SERVICE_CTRL);              // Clear tip list from mesh-mode heartbeats.
	as_info_set_command("truncate", info_command_truncate, PERM_TRUNCATE);                    // Truncate a set.
	as_info_set_command("truncate-namespace", info_command_truncate_namespace, PERM_TRUNCATE); // Truncate a namespace.
	as_info_set_command("truncate-namespace-undo", info_command_truncate_namespace_undo, PERM_TRUNCATE); // Undo a truncate-namespace command.
	as_info_set_command("truncate-undo", info_command_truncate_undo, PERM_TRUNCATE);          // Undo a truncate (set) command.

	// SINDEX
	as_info_set_dynamic("sindex", info_get_sindexes, false);
	as_info_set_tree("sindex", info_get_tree_sindexes);
	as_info_set_command("sindex-create", info_command_sindex_create, PERM_SINDEX_ADMIN); // Create a secondary index.
	as_info_set_command("sindex-delete", info_command_sindex_delete, PERM_SINDEX_ADMIN); // Delete a secondary index.
	as_info_set_command("sindex-exists", info_command_sindex_exists, PERM_SINDEX_ADMIN); // Does secondary index exist.

	// UDF
	as_info_set_dynamic("udf-list", udf_cask_info_list, false);
	as_info_set_command("udf-put", udf_cask_info_put, PERM_UDF_ADMIN);
	as_info_set_command("udf-get", udf_cask_info_get, PERM_NONE);
	as_info_set_command("udf-remove", udf_cask_info_remove, PERM_UDF_ADMIN);
	as_info_set_command("udf-clear-cache", udf_cask_info_clear_cache, PERM_UDF_ADMIN);

	// JOBS
	// TODO - deprecated - remove September 2022 +
	as_info_set_command("jobs", info_command_mon_cmd, PERM_QUERY_ADMIN); // Manipulate the multi-key lookup monitoring infrastructure.

	// TODO - deprecated - remove January 2023 +:
	as_info_set_dynamic("scan-show", info_query_show, false);
	as_info_set_command("scan-show", info_command_query_show, PERM_NONE);
	as_info_set_command("scan-abort", info_command_abort_query, PERM_QUERY_ADMIN);
	as_info_set_command("scan-abort-all", info_command_abort_all_queries, PERM_QUERY_ADMIN); // Abort all queries.

	as_info_set_dynamic("query-show", info_query_show, false);
	as_info_set_command("query-show", info_command_query_show, PERM_NONE);
	as_info_set_command("query-abort", info_command_abort_query, PERM_QUERY_ADMIN);
	as_info_set_command("query-abort-all", info_command_abort_all_queries, PERM_QUERY_ADMIN); // Abort all queries.

	as_info_set_command("sindex-stat", info_command_sindex_stat, PERM_NONE);
	as_info_set_command("sindex-list", info_command_sindex_list, PERM_NONE);

	// XDR
	as_info_set_command("xdr-dc-state", as_xdr_dc_state, PERM_NONE);
	as_info_set_command("xdr-get-filter", as_xdr_get_filter, PERM_NONE);
	as_info_set_command("xdr-set-filter", as_xdr_set_filter, PERM_XDR_SET_FILTER);

	as_service_list_init();

	for (uint32_t i = 0; i < g_config.n_info_threads; i++) {
		cf_thread_create_transient(thr_info_fn, NULL);
	}
}
