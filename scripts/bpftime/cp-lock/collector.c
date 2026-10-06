// Collector loaded under `bpftime load`: attaches uprobes to a
// target binary's three lock functions.
//
// Keeps cumulative per-call-site stats and writes them out
// periodically. Not part of the control plane — it changes no YANET
// code and runs only when a developer opts in.

#if !defined(__x86_64__)
#error "this collector resolves a uprobe's caller via the x86-64 stack layout; build for x86-64"
#endif

// For mkostemp(), used to create each snapshot's temp file.
#define _GNU_SOURCE

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cp_lock.h"
#include "elf_util.h"

#define CP_LOCK_DEFAULT_INTERVAL_SEC 10
#define CP_LOCK_POLL_MS 200
#define CP_LOCK_MAX_TGIDS 256
// Generous enough that an error message embedding a full PATH_MAX path
// plus surrounding text is never at risk of truncation.
#define CP_LOCK_ERRBUF_SIZE (PATH_MAX + 256)
// bpftime's own default, unrelated to anything this tool names
// itself.
//
// The name its syscall-server uses for the shared memory segment
// when BPFTIME_GLOBAL_SHM_NAME is not set.
#define CP_LOCK_DEFAULT_BPFTIME_SHM_NAME "bpftime_maps_shm"

struct cp_lock_args {
	const char *binary;
	const char *debug_file;
	const char *bpf_object;
	const char *out_prefix;
	int interval_sec;
};

// One lock function's attach target.
//
// Its link-time address and file offset in the binary (the latter is
// what a uprobe attaches at), and the two BPF programs — entry,
// return — that probe it.
struct cp_lock_target {
	const char *symbol;
	uint64_t vaddr;
	uint64_t offset;
	const char *entry_prog;
	const char *return_prog;
};

struct cp_lock_collector {
	char binary_path[PATH_MAX];
	// Where symbols actually came from: the command-line binary, or
	// the separate debug file if that binary had no .symtab.
	//
	// addr2line and nearest-symbol lookups both read from this image,
	// since a stripped command-line binary has no line info of its
	// own.
	char symbol_source_path[PATH_MAX];
	struct elf_image *binary_image;
	struct bpf_object *bpf_obj;
	int sites_fd;
	int overflow_fd;
	int bias_fd;
	int drops_fd;
	const char *out_prefix;
	int interval_sec;
	time_t start_time;

	// addr2line is best-effort and slow enough to be worth caching
	// across snapshots.
	//
	// Keyed by file-relative address, since every row symbolizes
	// against the same binary image.
	struct cp_lock_collector_line_entry {
		uint64_t file_vaddr;
		char line[256];
	} line_cache[CP_LOCK_MAX_SITES];
	size_t line_cache_count;
};

struct cp_lock_row {
	struct cp_lock_site_key key;
	struct cp_lock_site_stats stats;
};

static volatile sig_atomic_t g_snapshot_requested;
static volatile sig_atomic_t g_exit_requested;

static void
on_sigusr1(int sig) {
	(void)sig;
	g_snapshot_requested = 1;
}

static void
on_terminate(int sig) {
	(void)sig;
	g_exit_requested = 1;
}

static void
usage(const char *prog) {
	fprintf(stderr,
		"usage: %s --binary PATH --out PREFIX "
		"[--debug-file PATH] [--bpf-object PATH] "
		"[--interval SECONDS]\n",
		prog);
}

// Parses a strictly positive integer with no trailing garbage.
//
// Returns 1 and writes it on success, 0 on any parse error: empty
// string, trailing characters, a non-positive value, or overflow.
static int
parse_positive_int(const char *s, int *out) {
	if (!s || s[0] == '\0') {
		return 0;
	}
	errno = 0;
	char *end = NULL;
	long val = strtol(s, &end, 10);
	if (*end != '\0' || errno == ERANGE || val <= 0 || val > INT_MAX) {
		return 0;
	}
	*out = (int)val;
	return 1;
}

static int
parse_args(int argc, char **argv, struct cp_lock_args *args) {
	static const struct option opts[] = {
		{"binary", required_argument, NULL, 'b'},
		{"debug-file", required_argument, NULL, 'd'},
		{"bpf-object", required_argument, NULL, 'o'},
		{"out", required_argument, NULL, 'O'},
		{"interval", required_argument, NULL, 'i'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};

	args->interval_sec = CP_LOCK_DEFAULT_INTERVAL_SEC;

	int c;
	while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		switch (c) {
		case 'b':
			args->binary = optarg;
			break;
		case 'd':
			args->debug_file = optarg;
			break;
		case 'o':
			args->bpf_object = optarg;
			break;
		case 'O':
			args->out_prefix = optarg;
			break;
		case 'i':
			if (!parse_positive_int(optarg, &args->interval_sec)) {
				usage(argv[0]);
				return -1;
			}
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
		default:
			usage(argv[0]);
			return -1;
		}
	}

	if (!args->binary || !args->out_prefix) {
		usage(argv[0]);
		return -1;
	}
	return 0;
}

// Refuses to proceed if bpftime's shared memory segment already
// exists.
//
// Also warns if BPFTIME_GLOBAL_SHM_NAME is set in this process's own
// environment. The syscall-server this collector runs under creates
// that segment lazily, on its first file-open call rather than at
// process start, so this check, run before any such call, only ever
// sees a previous session's segment: one left behind after a crash,
// or one a concurrent collector is actively using. A non-default
// name matters because `bpftime attach` reads it from the target
// process's own environment, not from this command, so a target
// started without it only ever reaches a collector using the
// default.
static int
check_bpftime_shm_not_in_use(void) {
	const char *custom_name = getenv("BPFTIME_GLOBAL_SHM_NAME");
	if (custom_name) {
		fprintf(stderr,
			"warning: BPFTIME_GLOBAL_SHM_NAME=%s is set; "
			"`bpftime attach` to a target that does not have "
			"the same value in its own environment will miss "
			"this collector and report an empty snapshot\n",
			custom_name);
	}
	const char *shm_name =
		custom_name ? custom_name : CP_LOCK_DEFAULT_BPFTIME_SHM_NAME;

	char shm_path[PATH_MAX], lock_path[PATH_MAX];
	snprintf(shm_path, sizeof(shm_path), "/dev/shm/%s", shm_name);
	snprintf(
		lock_path,
		sizeof(lock_path),
		"/tmp/bpftime-shm-%s.lock",
		shm_name
	);

	if (access(shm_path, F_OK) != 0) {
		return 0;
	}
	fprintf(stderr,
		"%s already exists: another bpftime session is using it, "
		"or a prior one left it behind after a crash. Remove it "
		"first with `bpftimetool remove`, then `rm %s`.\n",
		shm_path,
		lock_path);
	return -1;
}

// Defaults to cp_lock.bpf.o next to this executable, so the tool runs
// the same way regardless of the caller's working directory.
static int
default_bpf_object_path(char *out, size_t out_size) {
	char exe[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n <= 0) {
		return -1;
	}
	exe[n] = '\0';

	char *slash = strrchr(exe, '/');
	if (!slash) {
		return -1;
	}
	*slash = '\0';
	snprintf(out, out_size, "%s/cp_lock.bpf.o", exe);
	return 0;
}

// Resolves the three lock symbols to file offsets in the target
// binary.
//
// Falls back to a build-id debug file or an explicit --debug-file
// when the binary itself has no .symtab. The returned image keeps
// that symbol table loaded for the collector's lifetime, to
// symbolize report rows later. source_path (a caller-owned buffer of
// at least PATH_MAX) receives whichever file the symbols actually
// came from, since that is also the file addr2line needs later:
// binary_path itself never has line info once it is the stripped
// side of a debug-file split.
static struct elf_image *
resolve_targets(
	const char *binary_path,
	const char *debug_file_arg,
	struct cp_lock_target targets[3],
	char *source_path,
	char *errbuf,
	size_t errbuf_size
) {
	struct elf_image *img =
		elf_image_open(binary_path, errbuf, errbuf_size);
	if (!img) {
		return NULL;
	}
	snprintf(source_path, PATH_MAX, "%s", binary_path);

	if (!elf_image_has_symbols(img)) {
		const char *debug_path = debug_file_arg;
		char build_id_path[PATH_MAX];
		// Test-only: lets the selftest point the build-id lookup at
		// a temporary root.
		//
		// The real /usr/lib/debug needs permissions the selftest
		// does not have to write into.
		const char *debug_root = getenv("CP_LOCK_DEBUG_ROOT");
		if (!debug_path && elf_image_build_id_debug_path(
					   img,
					   debug_root,
					   build_id_path,
					   sizeof(build_id_path)
				   )) {
			debug_path = build_id_path;
		}
		if (!debug_path) {
			snprintf(
				errbuf,
				errbuf_size,
				"%s has no .symtab and no build-id debug "
				"file was found; pass --debug-file",
				binary_path
			);
			elf_image_close(img);
			return NULL;
		}

		char debug_err[CP_LOCK_ERRBUF_SIZE];
		struct elf_image *debug_img = elf_image_open(
			debug_path, debug_err, sizeof(debug_err)
		);
		if (!debug_img || !elf_image_has_symbols(debug_img)) {
			snprintf(
				errbuf,
				errbuf_size,
				"%s has no .symtab and the debug file %s "
				"did not supply one",
				binary_path,
				debug_path
			);
			if (debug_img) {
				elf_image_close(debug_img);
			}
			elf_image_close(img);
			return NULL;
		}

		// A debug file for the wrong binary would still carry a
		// .symtab — just the wrong one.
		//
		// That silently mis-symbolizes every row; the build-id,
		// when both sides have one, is what actually ties the two
		// files together.
		char bin_id[ELF_BUILD_ID_HEX_SIZE],
			dbg_id[ELF_BUILD_ID_HEX_SIZE];
		int have_bin_id = elf_image_build_id(img, bin_id);
		int have_dbg_id = elf_image_build_id(debug_img, dbg_id);
		if (have_bin_id && have_dbg_id && strcmp(bin_id, dbg_id) != 0) {
			snprintf(
				errbuf,
				errbuf_size,
				"%s build-id %s does not match %s build-id "
				"%s",
				binary_path,
				bin_id,
				debug_path,
				dbg_id
			);
			elf_image_close(debug_img);
			elf_image_close(img);
			return NULL;
		}
		if (have_bin_id != have_dbg_id) {
			snprintf(
				errbuf,
				errbuf_size,
				"%s: only one of it and the debug file %s has "
				"a "
				"build-id; cannot verify they match",
				binary_path,
				debug_path
			);
			elf_image_close(debug_img);
			elf_image_close(img);
			return NULL;
		}
		if (!have_bin_id) {
			fprintf(stderr,
				"warning: %s has no build-id note; using %s "
				"without verifying it actually matches\n",
				binary_path,
				debug_path);
		}

		elf_image_adopt_symbols(img, debug_img);
		elf_image_close(debug_img);
		snprintf(source_path, PATH_MAX, "%s", debug_path);
	}

	for (int i = 0; i < 3; ++i) {
		int ambiguous = 0;
		if (!elf_image_symbol_vaddr(
			    img,
			    targets[i].symbol,
			    &targets[i].vaddr,
			    &ambiguous
		    ) ||
		    !elf_image_symbol_offset(
			    img, targets[i].symbol, &targets[i].offset
		    )) {
			if (ambiguous) {
				snprintf(
					errbuf,
					errbuf_size,
					"%s: symbol %s has more than one "
					"global definition; cannot pick one",
					binary_path,
					targets[i].symbol
				);
			} else {
				snprintf(
					errbuf,
					errbuf_size,
					"%s: symbol %s not found",
					binary_path,
					targets[i].symbol
				);
			}
			elf_image_close(img);
			return NULL;
		}
	}
	return img;
}

static int
attach_one(
	struct bpf_object *obj,
	const char *prog_name,
	bool retprobe,
	const char *binary_path,
	uint64_t offset
) {
	struct bpf_program *prog =
		bpf_object__find_program_by_name(obj, prog_name);
	if (!prog) {
		fprintf(stderr, "bpf program %s not found\n", prog_name);
		return -1;
	}
	struct bpf_link *link = bpf_program__attach_uprobe(
		prog, retprobe, -1, binary_path, offset
	);
	if (!link) {
		fprintf(stderr,
			"failed to attach %s at offset 0x%lx: %s\n",
			prog_name,
			(unsigned long)offset,
			strerror(errno));
		return -1;
	}
	return 0;
}

static int
attach_targets(
	struct bpf_object *obj,
	const char *binary_path,
	const struct cp_lock_target targets[3]
) {
	for (int i = 0; i < 3; ++i) {
		if (attach_one(
			    obj,
			    targets[i].entry_prog,
			    false,
			    binary_path,
			    targets[i].offset
		    ) != 0) {
			return -1;
		}
		if (attach_one(
			    obj,
			    targets[i].return_prog,
			    true,
			    binary_path,
			    targets[i].offset
		    ) != 0) {
			return -1;
		}
	}
	return 0;
}

// Writes each target's link-time address into the BPF program's
// lookup table, at the same index its matching probe reads from.
//
// MUST run after the program is loaded, since the map has to exist,
// and before any uprobe is attached: a probe that reads a still-zero
// entry here skips recording that process's load address rather than
// recording a wrong one, losing the bias for that call instead of
// corrupting it.
static int
populate_targets(
	struct bpf_object *obj, const struct cp_lock_target targets[3]
) {
	int fd = bpf_object__find_map_fd_by_name(obj, "cp_lock_targets");
	if (fd < 0) {
		fprintf(stderr, "cp_lock_targets map not found\n");
		return -1;
	}
	for (__u32 i = 0; i < 3; ++i) {
		if (bpf_map_update_elem(fd, &i, &targets[i].vaddr, BPF_ANY) !=
		    0) {
			fprintf(stderr,
				"failed to set target %s: %s\n",
				targets[i].symbol,
				strerror(errno));
			return -1;
		}
	}
	return 0;
}

// Runs `addr2line -e elf_path file_vaddr` via fork/exec, never a
// shell.
//
// A path with shell metacharacters cannot do anything but fail to
// open. Returns 1 and writes the file:line on success, 0 if
// addr2line is missing, exits abnormally, or has no line info for
// that address.
static int
run_addr2line(
	const char *elf_path, uint64_t file_vaddr, char *out, size_t out_size
) {
	int pipefd[2];
	if (pipe(pipefd) != 0) {
		return 0;
	}

	pid_t pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return 0;
	}
	if (pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		close(pipefd[1]);
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}
		char addr_arg[32];
		snprintf(
			addr_arg,
			sizeof(addr_arg),
			"0x%lx",
			(unsigned long)file_vaddr
		);
		char *argv[] = {
			(char *)"addr2line",
			(char *)"-e",
			(char *)elf_path,
			addr_arg,
			NULL
		};
		execvp("addr2line", argv);
		_exit(127);
	}

	close(pipefd[1]);
	FILE *f = fdopen(pipefd[0], "r");
	int ok = 0;
	if (f) {
		if (fgets(out, out_size, f)) {
			size_t n = strlen(out);
			if (n && out[n - 1] == '\n') {
				out[n - 1] = '\0';
			}
			ok = out[0] != '\0' && strcmp(out, "??:0") != 0 &&
			     strcmp(out, "??:?") != 0;
		}
		fclose(f);
	} else {
		close(pipefd[0]);
	}

	int status = 0;
	waitpid(pid, &status, 0);
	return ok;
}

// Looks up, caching across calls, the file:line addr2line reports for
// a file-relative address.
//
// Returns NULL when addr2line is unavailable or has no line info, in
// which case the caller falls back to the symbol name alone.
static const char *
line_lookup(struct cp_lock_collector *col, uint64_t file_vaddr) {
	for (size_t i = 0; i < col->line_cache_count; ++i) {
		if (col->line_cache[i].file_vaddr == file_vaddr) {
			return col->line_cache[i].line[0]
				       ? col->line_cache[i].line
				       : NULL;
		}
	}

	char line[256] = {0};
	run_addr2line(col->symbol_source_path, file_vaddr, line, sizeof(line));

	if (col->line_cache_count >= CP_LOCK_MAX_SITES) {
		// Cache exhausted: degrade to no line info rather than
		// returning a pointer that would not outlive this call.
		return NULL;
	}
	struct cp_lock_collector_line_entry *entry =
		&col->line_cache[col->line_cache_count++];
	entry->file_vaddr = file_vaddr;
	snprintf(entry->line, sizeof(entry->line), "%s", line);
	return entry->line[0] ? entry->line : NULL;
}

// Describes one row's call site for the report.
//
// A symbol name whenever it can be resolved, the raw pid:address
// pair as the documented fallback otherwise.
struct cp_lock_site_label {
	char name[256];	   // "func" or "func+0x18", empty if unresolved.
	char line[256];	   // "file:line", empty if unavailable.
	char fallback[64]; // "pid:0xADDR", used when name is empty.
};

static void
symbolize(
	struct cp_lock_collector *col,
	const struct cp_lock_row *row,
	struct cp_lock_site_label *label
) {
	memset(label, 0, sizeof(*label));
	snprintf(
		label->fallback,
		sizeof(label->fallback),
		"%lu:0x%lx",
		(unsigned long)row->key.tgid,
		(unsigned long)row->key.ret
	);

	// The process's load address, recorded in the BPF program itself
	// from its first probed call.
	//
	// Unlike /proc/<tgid>/maps, this stays available for the
	// collector's whole lifetime, long past that process exiting.
	uint64_t bias;
	if (bpf_map_lookup_elem(col->bias_fd, &row->key.tgid, &bias) != 0) {
		return;
	}

	// Steps one byte back from the return address before symbolizing.
	//
	// A call that is the last instruction before falling into the
	// next function would otherwise resolve to that neighboring
	// symbol instead of the real caller.
	uint64_t file_vaddr = row->key.ret - 1 - bias;

	const struct elf_symbol *sym =
		elf_image_find_containing(col->binary_image, file_vaddr);
	if (!sym) {
		return;
	}

	uint64_t sym_offset = file_vaddr - sym->value;
	if (sym_offset == 0) {
		snprintf(label->name, sizeof(label->name), "%s", sym->name);
	} else {
		snprintf(
			label->name,
			sizeof(label->name),
			"%s+0x%lx",
			sym->name,
			(unsigned long)sym_offset
		);
	}

	const char *line = line_lookup(col, file_vaddr);
	if (line) {
		snprintf(label->line, sizeof(label->line), "%s", line);
	}
}

// Resolves every row's symbol and file:line exactly once, so the
// .txt and .json outputs always agree with each other.
//
// Resolving a row shells out to addr2line on an uncached address, so
// this also keeps that cost to one call per row.
static void
symbolize_all(
	struct cp_lock_collector *col,
	const struct cp_lock_row *rows,
	size_t row_count,
	struct cp_lock_site_label *labels
) {
	for (size_t i = 0; i < row_count; ++i) {
		symbolize(col, &rows[i], &labels[i]);
	}
}

// The site column's width for this snapshot: wide enough for every
// resolved name in it, never narrower than the table's usual width.
//
// A rare long name widens the column instead of shifting every field
// after it out of alignment.
#define CP_LOCK_SITE_COLUMN_MIN_WIDTH 40

static int
site_column_width(const struct cp_lock_site_label *labels, size_t row_count) {
	int width = CP_LOCK_SITE_COLUMN_MIN_WIDTH;
	for (size_t i = 0; i < row_count; ++i) {
		const char *name =
			labels[i].name[0] ? labels[i].name : labels[i].fallback;
		int len = (int)strlen(name);
		if (len > width) {
			width = len;
		}
	}
	return width;
}

static int
row_cmp_hold_desc(const void *a, const void *b) {
	const struct cp_lock_row *ra = a;
	const struct cp_lock_row *rb = b;
	if (ra->stats.hold_sum_ns > rb->stats.hold_sum_ns) {
		return -1;
	}
	if (ra->stats.hold_sum_ns < rb->stats.hold_sum_ns) {
		return 1;
	}
	return 0;
}

static size_t
collect_rows(
	struct cp_lock_collector *col, struct cp_lock_row *rows, size_t max_rows
) {
	size_t count = 0;
	struct cp_lock_site_key key = {0}, next_key;
	int have_key = 0;

	while (count < max_rows) {
		int rc = bpf_map_get_next_key(
			col->sites_fd, have_key ? &key : NULL, &next_key
		);
		if (rc != 0) {
			break;
		}
		key = next_key;
		have_key = 1;

		struct cp_lock_site_stats stats;
		if (bpf_map_lookup_elem(col->sites_fd, &key, &stats) != 0) {
			continue;
		}
		rows[count].key = key;
		rows[count].stats = stats;
		count++;
	}

	qsort(rows, count, sizeof(*rows), row_cmp_hold_desc);
	return count;
}

static uint64_t
read_overflow_events(struct cp_lock_collector *col) {
	__u32 zero_key = 0;
	uint64_t value = 0;
	bpf_map_lookup_elem(col->overflow_fd, &zero_key, &value);
	return value;
}

static void
read_drops(struct cp_lock_collector *col, struct cp_lock_drop_counters *out) {
	__u32 zero_key = 0;
	memset(out, 0, sizeof(*out));
	bpf_map_lookup_elem(col->drops_fd, &zero_key, out);
}

// Collects the distinct tgids among rows into pids.
//
// Sets *truncated when more distinct tgids exist than max_pids
// holds, so a report never claims a short pids list is complete.
static size_t
collect_pids(
	const struct cp_lock_row *rows,
	size_t row_count,
	uint64_t *pids,
	size_t max_pids,
	int *truncated
) {
	size_t n = 0;
	*truncated = 0;
	for (size_t i = 0; i < row_count; ++i) {
		uint64_t tgid = rows[i].key.tgid;
		int seen = 0;
		for (size_t j = 0; j < n; ++j) {
			if (pids[j] == tgid) {
				seen = 1;
				break;
			}
		}
		if (seen) {
			continue;
		}
		if (n < max_pids) {
			pids[n++] = tgid;
		} else {
			*truncated = 1;
		}
	}
	return n;
}

// Writes to a fresh dir/name.XXXXXX, then renames it over dir/name,
// so a reader polling the output path never observes a partial file.
//
// The random suffix, not a predictable name, is what keeps another
// user from preplanting anything at this path in a shared output
// directory ahead of time.
static FILE *
open_atomic(const char *final_path, char *tmp_path, size_t tmp_path_size) {
	snprintf(tmp_path, tmp_path_size, "%s.XXXXXX", final_path);

	int fd = mkostemp(tmp_path, O_CLOEXEC);
	if (fd < 0) {
		return NULL;
	}
	if (fchmod(fd, 0644) != 0) {
		close(fd);
		unlink(tmp_path);
		return NULL;
	}
	FILE *f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		unlink(tmp_path);
	}
	return f;
}

// Finishes an atomic write: on any failure, the temp file is removed
// and the previous snapshot is left exactly as it was.
//
// Failure covers a write that already failed (ferror), fflush,
// fsync, fclose, or the final rename — final_path is never replaced
// by a partial file.
static int
close_atomic(FILE *f, const char *tmp_path, const char *final_path) {
	int failed = ferror(f);
	if (fflush(f) != 0) {
		failed = 1;
	}
	int fd = fileno(f);
	if (fd >= 0 && fsync(fd) != 0) {
		failed = 1;
	}
	if (fclose(f) != 0) {
		failed = 1;
	}
	if (failed) {
		unlink(tmp_path);
		return -1;
	}
	if (rename(tmp_path, final_path) != 0) {
		unlink(tmp_path);
		return -1;
	}
	return 0;
}

static uint64_t
hist_total(const struct cp_lock_site_stats *stats) {
	uint64_t total = 0;
	for (int i = 0; i < CP_LOCK_HIST_BUCKETS; ++i) {
		total += stats->hist[i];
	}
	return total;
}

// Formats a UTC wall-clock time as RFC3339 (e.g. 2026-10-05T12:34:56Z).
static void
format_rfc3339(time_t t, char *out, size_t out_size) {
	struct tm tm;
	gmtime_r(&t, &tm);
	strftime(out, out_size, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static void
write_snapshot_txt(
	struct cp_lock_collector *col,
	struct cp_lock_row *rows,
	const struct cp_lock_site_label *labels,
	size_t row_count,
	uint64_t overflow_events,
	const struct cp_lock_drop_counters *drops,
	const uint64_t *pids,
	size_t pid_count,
	int pids_truncated
) {
	char final_path[PATH_MAX + 16], tmp_path[PATH_MAX + 32];
	snprintf(final_path, sizeof(final_path), "%s.txt", col->out_prefix);
	FILE *f = open_atomic(final_path, tmp_path, sizeof(tmp_path));
	if (!f) {
		fprintf(stderr, "snapshot: %s: %s\n", tmp_path, strerror(errno)
		);
		return;
	}

	char timestamp[32], collector_start[32];
	format_rfc3339(time(NULL), timestamp, sizeof(timestamp));
	format_rfc3339(
		col->start_time, collector_start, sizeof(collector_start)
	);

	fprintf(f, "cp_config_lock instrumentation snapshot\n");
	fprintf(f, "binary: %s\n", col->binary_path);
	fprintf(f, "timestamp: %s\n", timestamp);
	fprintf(f, "collector_start: %s\n", collector_start);
	// Any call — an acquisition or a failed try_lock — not recorded
	// because the site table was full.
	fprintf(f, "overflow_events: %lu\n", (unsigned long)overflow_events);
	fprintf(f,
		"drops: stale_state=%lu state_insert_fail=%lu "
		"unlock_no_state=%lu bias_insert_fail=%lu "
		"ret_read_fail=%lu bad_timestamp=%lu "
		"site_insert_fail=%lu\n",
		(unsigned long)drops->stale_state,
		(unsigned long)drops->state_insert_fail,
		(unsigned long)drops->unlock_no_state,
		(unsigned long)drops->bias_insert_fail,
		(unsigned long)drops->ret_read_fail,
		(unsigned long)drops->bad_timestamp,
		(unsigned long)drops->site_insert_fail);
	fprintf(f, "pids:");
	for (size_t i = 0; i < pid_count; ++i) {
		fprintf(f, " %lu", (unsigned long)pids[i]);
	}
	fprintf(f, "\n");
	if (pids_truncated) {
		fprintf(f, "pids_truncated: true\n");
	}
	fprintf(f, "\n");

	int name_width = site_column_width(labels, row_count);

	fprintf(f,
		"%-*s %8s %10s %12s %12s %12s %12s %10s %10s\n",
		name_width,
		"site",
		"pid",
		"count",
		"wait_sum_ns",
		"wait_max_ns",
		"hold_sum_ns",
		"hold_max_ns",
		"fail_count",
		"hist_total");

	for (size_t i = 0; i < row_count; ++i) {
		const char *name =
			labels[i].name[0] ? labels[i].name : labels[i].fallback;

		fprintf(f,
			"%-*s %8lu %10lu %12lu %12lu %12lu %12lu %10lu "
			"%10lu\n",
			name_width,
			name,
			(unsigned long)rows[i].key.tgid,
			(unsigned long)rows[i].stats.count,
			(unsigned long)rows[i].stats.wait_sum_ns,
			(unsigned long)rows[i].stats.wait_max_ns,
			(unsigned long)rows[i].stats.hold_sum_ns,
			(unsigned long)rows[i].stats.hold_max_ns,
			(unsigned long)rows[i].stats.fail_count,
			(unsigned long)hist_total(&rows[i].stats));
		if (labels[i].line[0]) {
			fprintf(f, "    %s\n", labels[i].line);
		}
	}

	if (close_atomic(f, tmp_path, final_path) != 0) {
		fprintf(stderr,
			"snapshot: failed to write %s; keeping previous "
			"snapshot\n",
			final_path);
	}
}

// Length in bytes of the well-formed UTF-8 sequence starting at s[0],
// or 0 if s does not begin one.
//
// Rejects an overlong encoding, a UTF-16 surrogate, and a code point
// above U+10FFFF, in addition to a malformed leading or continuation
// byte — a path or symbol from the target process is untrusted input,
// and only a sequence that passes all of these actually round-trips
// through a JSON string reader as the code point it claims to be.
static int
utf8_seq_len(const unsigned char *s, size_t avail) {
	unsigned char c0 = s[0];
	int len;
	unsigned int min_cp, cp;
	if ((c0 & 0xE0) == 0xC0) {
		len = 2;
		min_cp = 0x80;
		cp = c0 & 0x1F;
	} else if ((c0 & 0xF0) == 0xE0) {
		len = 3;
		min_cp = 0x800;
		cp = c0 & 0x0F;
	} else if ((c0 & 0xF8) == 0xF0) {
		len = 4;
		min_cp = 0x10000;
		cp = c0 & 0x07;
	} else {
		return 0;
	}
	if (avail < (size_t)len) {
		return 0;
	}
	for (int i = 1; i < len; ++i) {
		unsigned char c = s[i];
		if ((c & 0xC0) != 0x80) {
			return 0;
		}
		cp = (cp << 6) | (c & 0x3F);
	}
	if (cp < min_cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
		return 0;
	}
	return len;
}

// Escapes a string for a JSON string literal, passing well-formed
// UTF-8 through unchanged.
//
// A byte that cannot start or continue a valid sequence is escaped on
// its own as \u00XX rather than copied raw, so one invalid byte in an
// untrusted path or symbol never turns the surrounding JSON malformed.
static void
json_escape(const char *in, char *out, size_t out_size) {
	size_t o = 0;
	size_t len = strlen(in);
	size_t i = 0;
	while (i < len) {
		unsigned char c = (unsigned char)in[i];
		if (c == '"' || c == '\\') {
			if (o + 2 >= out_size) {
				break;
			}
			out[o++] = '\\';
			out[o++] = (char)c;
			i += 1;
		} else if (c < 0x20) {
			if (o + 6 >= out_size) {
				break;
			}
			snprintf(out + o, 7, "\\u%04x", c);
			o += 6;
			i += 1;
		} else if (c < 0x80) {
			if (o + 1 >= out_size) {
				break;
			}
			out[o++] = (char)c;
			i += 1;
		} else {
			int seq_len = utf8_seq_len(
				(const unsigned char *)in + i, len - i
			);
			if (seq_len > 0) {
				if (o + (size_t)seq_len >= out_size) {
					break;
				}
				memcpy(out + o, in + i, (size_t)seq_len);
				o += (size_t)seq_len;
				i += (size_t)seq_len;
			} else {
				if (o + 6 >= out_size) {
					break;
				}
				snprintf(out + o, 7, "\\u%04x", c);
				o += 6;
				i += 1;
			}
		}
	}
	out[o] = '\0';
}

static void
write_snapshot_json(
	struct cp_lock_collector *col,
	struct cp_lock_row *rows,
	const struct cp_lock_site_label *labels,
	size_t row_count,
	uint64_t overflow_events,
	const struct cp_lock_drop_counters *drops,
	const uint64_t *pids,
	size_t pid_count,
	int pids_truncated
) {
	char final_path[PATH_MAX + 16], tmp_path[PATH_MAX + 32];
	snprintf(final_path, sizeof(final_path), "%s.json", col->out_prefix);
	FILE *f = open_atomic(final_path, tmp_path, sizeof(tmp_path));
	if (!f) {
		fprintf(stderr, "snapshot: %s: %s\n", tmp_path, strerror(errno)
		);
		return;
	}

	char binary_escaped[PATH_MAX * 2];
	json_escape(col->binary_path, binary_escaped, sizeof(binary_escaped));
	char timestamp[32], collector_start[32];
	format_rfc3339(time(NULL), timestamp, sizeof(timestamp));
	format_rfc3339(
		col->start_time, collector_start, sizeof(collector_start)
	);

	fprintf(f, "{\n");
	fprintf(f, "  \"binary\": \"%s\",\n", binary_escaped);
	fprintf(f, "  \"timestamp\": \"%s\",\n", timestamp);
	fprintf(f, "  \"collector_start\": \"%s\",\n", collector_start);
	// Any call — an acquisition or a failed try_lock — not recorded
	// because the site table was full.
	fprintf(f,
		"  \"overflow_events\": %lu,\n",
		(unsigned long)overflow_events);
	fprintf(f,
		"  \"drops\": {\"stale_state\": %lu, "
		"\"state_insert_fail\": %lu, \"unlock_no_state\": %lu, "
		"\"bias_insert_fail\": %lu, \"ret_read_fail\": %lu, "
		"\"bad_timestamp\": %lu, \"site_insert_fail\": %lu},\n",
		(unsigned long)drops->stale_state,
		(unsigned long)drops->state_insert_fail,
		(unsigned long)drops->unlock_no_state,
		(unsigned long)drops->bias_insert_fail,
		(unsigned long)drops->ret_read_fail,
		(unsigned long)drops->bad_timestamp,
		(unsigned long)drops->site_insert_fail);
	fprintf(f,
		"  \"pids_truncated\": %s,\n",
		pids_truncated ? "true" : "false");
	fprintf(f, "  \"pids\": [");
	for (size_t i = 0; i < pid_count; ++i) {
		fprintf(f, "%s%lu", i ? ", " : "", (unsigned long)pids[i]);
	}
	fprintf(f, "],\n");
	fprintf(f, "  \"sites\": [\n");

	for (size_t i = 0; i < row_count; ++i) {
		const char *name =
			labels[i].name[0] ? labels[i].name : labels[i].fallback;
		char name_escaped[512];
		json_escape(name, name_escaped, sizeof(name_escaped));
		char line_escaped[512];
		json_escape(labels[i].line, line_escaped, sizeof(line_escaped));

		fprintf(f,
			"    {\"tgid\": %lu, \"ret\": \"0x%lx\", \"site\": "
			"\"%s\", "
			"\"file_line\": \"%s\", \"count\": %lu, "
			"\"wait_sum_ns\": %lu, \"wait_max_ns\": %lu, "
			"\"hold_sum_ns\": %lu, \"hold_max_ns\": %lu, "
			"\"fail_count\": %lu, \"hist\": [",
			(unsigned long)rows[i].key.tgid,
			(unsigned long)rows[i].key.ret,
			name_escaped,
			line_escaped,
			(unsigned long)rows[i].stats.count,
			(unsigned long)rows[i].stats.wait_sum_ns,
			(unsigned long)rows[i].stats.wait_max_ns,
			(unsigned long)rows[i].stats.hold_sum_ns,
			(unsigned long)rows[i].stats.hold_max_ns,
			(unsigned long)rows[i].stats.fail_count);
		for (int b = 0; b < CP_LOCK_HIST_BUCKETS; ++b) {
			fprintf(f,
				"%s%lu",
				b ? ", " : "",
				(unsigned long)rows[i].stats.hist[b]);
		}
		fprintf(f, "]}%s\n", (i + 1 < row_count) ? "," : "");
	}

	fprintf(f, "  ]\n}\n");

	if (close_atomic(f, tmp_path, final_path) != 0) {
		fprintf(stderr,
			"snapshot: failed to write %s; keeping previous "
			"snapshot\n",
			final_path);
	}
}

static void
write_snapshot(struct cp_lock_collector *col) {
	static struct cp_lock_row rows[CP_LOCK_MAX_SITES];
	size_t row_count = collect_rows(col, rows, CP_LOCK_MAX_SITES);
	uint64_t overflow_events = read_overflow_events(col);
	struct cp_lock_drop_counters drops;
	read_drops(col, &drops);

	uint64_t pids[CP_LOCK_MAX_TGIDS];
	int pids_truncated = 0;
	size_t pid_count = collect_pids(
		rows, row_count, pids, CP_LOCK_MAX_TGIDS, &pids_truncated
	);

	static struct cp_lock_site_label labels[CP_LOCK_MAX_SITES];
	symbolize_all(col, rows, row_count, labels);

	write_snapshot_txt(
		col,
		rows,
		labels,
		row_count,
		overflow_events,
		&drops,
		pids,
		pid_count,
		pids_truncated
	);
	write_snapshot_json(
		col,
		rows,
		labels,
		row_count,
		overflow_events,
		&drops,
		pids,
		pid_count,
		pids_truncated
	);
}

static void
run(struct cp_lock_collector *col) {
	time_t next_interval =
		col->interval_sec > 0 ? time(NULL) + col->interval_sec : 0;

	for (;;) {
		if (g_exit_requested) {
			break;
		}
		if (g_snapshot_requested) {
			g_snapshot_requested = 0;
			write_snapshot(col);
		}
		if (next_interval != 0 && time(NULL) >= next_interval) {
			write_snapshot(col);
			next_interval = time(NULL) + col->interval_sec;
		}

		struct timespec ts = {
			.tv_sec = 0, .tv_nsec = CP_LOCK_POLL_MS * 1000000L
		};
		nanosleep(&ts, NULL);
	}

	write_snapshot(col);
}

int
main(int argc, char **argv) {
	struct cp_lock_args args = {0};
	if (parse_args(argc, argv, &args) != 0) {
		return 1;
	}

	// A longer prefix would leave no room for the suffixes appended to
	// build each output path within a bounded buffer.
	//
	// Refusing here means a later output path is always built in full,
	// never silently truncated.
	if (strlen(args.out_prefix) > (size_t)(PATH_MAX - 32)) {
		fprintf(stderr,
			"--out prefix is too long (max %d characters)\n",
			PATH_MAX - 32);
		return 1;
	}

	if (check_bpftime_shm_not_in_use() != 0) {
		return 1;
	}

	struct cp_lock_collector col = {0};
	col.out_prefix = args.out_prefix;
	col.interval_sec = args.interval_sec;
	col.start_time = time(NULL);

	if (!realpath(args.binary, col.binary_path)) {
		fprintf(stderr, "%s: %s\n", args.binary, strerror(errno));
		return 1;
	}

	struct cp_lock_target targets[3] = {
		{.symbol = "cp_config_lock",
		 .entry_prog = "cp_lock_on_lock_entry",
		 .return_prog = "cp_lock_on_lock_return"},
		{.symbol = "cp_config_try_lock",
		 .entry_prog = "cp_lock_on_try_lock_entry",
		 .return_prog = "cp_lock_on_try_lock_return"},
		{.symbol = "cp_config_unlock",
		 .entry_prog = "cp_lock_on_unlock_entry",
		 .return_prog = "cp_lock_on_unlock_return"},
	};

	char errbuf[CP_LOCK_ERRBUF_SIZE];
	col.binary_image = resolve_targets(
		col.binary_path,
		args.debug_file,
		targets,
		col.symbol_source_path,
		errbuf,
		sizeof(errbuf)
	);
	if (!col.binary_image) {
		fprintf(stderr, "%s\n", errbuf);
		return 1;
	}

	char bpf_object_path[PATH_MAX + 32];
	const char *bpf_object = args.bpf_object;
	if (!bpf_object) {
		if (default_bpf_object_path(
			    bpf_object_path, sizeof(bpf_object_path)
		    ) != 0) {
			fprintf(stderr,
				"could not locate cp_lock.bpf.o; pass "
				"--bpf-object\n");
			return 1;
		}
		bpf_object = bpf_object_path;
	}

	col.bpf_obj = bpf_object__open_file(bpf_object, NULL);
	if (!col.bpf_obj || bpf_object__load(col.bpf_obj) != 0) {
		fprintf(stderr, "failed to load %s\n", bpf_object);
		return 1;
	}

	// Populating the target table and finding the maps both happen
	// before any uprobe is attached.
	//
	// A probe must never see an unpopulated target table or a
	// missing map.
	if (populate_targets(col.bpf_obj, targets) != 0) {
		return 1;
	}

	col.sites_fd =
		bpf_object__find_map_fd_by_name(col.bpf_obj, "cp_lock_sites");
	col.overflow_fd = bpf_object__find_map_fd_by_name(
		col.bpf_obj, "cp_lock_overflow_events"
	);
	col.bias_fd =
		bpf_object__find_map_fd_by_name(col.bpf_obj, "cp_lock_bias");
	col.drops_fd =
		bpf_object__find_map_fd_by_name(col.bpf_obj, "cp_lock_drops");
	if (col.sites_fd < 0 || col.overflow_fd < 0 || col.bias_fd < 0 ||
	    col.drops_fd < 0) {
		fprintf(stderr, "failed to find collector maps\n");
		return 1;
	}

	// Installed before any uprobe is attached.
	//
	// A SIGINT, SIGTERM, or SIGUSR1 arriving during attach would
	// otherwise hit the default disposition instead of this
	// collector's own clean-exit handling.
	struct sigaction sa_usr1 = {0}, sa_term = {0};
	sa_usr1.sa_handler = on_sigusr1;
	sa_term.sa_handler = on_terminate;
	if (sigaction(SIGUSR1, &sa_usr1, NULL) != 0 ||
	    sigaction(SIGINT, &sa_term, NULL) != 0 ||
	    sigaction(SIGTERM, &sa_term, NULL) != 0) {
		fprintf(stderr,
			"failed to install signal handlers: %s\n",
			strerror(errno));
		return 1;
	}

	if (attach_targets(col.bpf_obj, col.binary_path, targets) != 0) {
		return 1;
	}

	fprintf(stderr,
		"cp-lock collector ready: pid=%d binary=%s out=%s.{txt,json} "
		"interval=%ds\n",
		(int)getpid(),
		col.binary_path,
		col.out_prefix,
		col.interval_sec);

	run(&col);

	elf_image_close(col.binary_image);
	return 0;
}
