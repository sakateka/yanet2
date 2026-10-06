#include "elf_util.h"

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int
elf_image_load_phdrs(struct elf_image *img, Elf *elf) {
	size_t count = 0;
	if (elf_getphdrnum(elf, &count) != 0) {
		return -1;
	}

	img->loads = calloc(count, sizeof(*img->loads));
	if (!img->loads) {
		return -1;
	}

	for (size_t i = 0; i < count; ++i) {
		GElf_Phdr phdr;
		if (!gelf_getphdr(elf, (int)i, &phdr)) {
			continue;
		}
		if (phdr.p_type != PT_LOAD) {
			continue;
		}
		img->loads[img->load_count].vaddr = phdr.p_vaddr;
		img->loads[img->load_count].offset = phdr.p_offset;
		img->loads[img->load_count].filesz = phdr.p_filesz;
		img->load_count++;
	}
	return 0;
}

// Orders by address, so a search for the symbol containing a given
// address always lands on the same alias when several share one.
//
// Preference among same-address aliases: a global symbol over a
// local or weak one, then the lexicographically smallest name.
static int
symbol_cmp(const void *a, const void *b) {
	const struct elf_symbol *sa = a;
	const struct elf_symbol *sb = b;
	if (sa->value != sb->value) {
		return sa->value < sb->value ? -1 : 1;
	}
	if (sa->is_global != sb->is_global) {
		return sa->is_global ? 1 : -1;
	}
	return strcmp(sb->name, sa->name);
}

// Loads STT_FUNC entries from the first SHT_SYMTAB section, if any.
//
// An image with none is left with an empty symbol table, not an
// error — that is how a stripped binary is told apart from a parse
// failure.
static int
elf_image_load_symtab(struct elf_image *img, Elf *elf) {
	Elf_Scn *scn = NULL;
	while ((scn = elf_nextscn(elf, scn)) != NULL) {
		GElf_Shdr shdr;
		if (!gelf_getshdr(scn, &shdr)) {
			continue;
		}
		if (shdr.sh_type != SHT_SYMTAB) {
			continue;
		}

		Elf_Data *data = elf_getdata(scn, NULL);
		if (!data || shdr.sh_entsize == 0) {
			return 0;
		}
		size_t nsyms = data->d_size / shdr.sh_entsize;

		// Sized for the worst case where every symbol is a kept
		// STT_FUNC; the real count is tracked separately.
		struct elf_symbol *symbols = calloc(nsyms, sizeof(*symbols));
		char **names = calloc(nsyms, sizeof(*names));
		if (!symbols || !names) {
			free(symbols);
			free(names);
			return -1;
		}

		size_t kept = 0;
		for (size_t i = 0; i < nsyms; ++i) {
			GElf_Sym sym;
			if (!gelf_getsym(data, (int)i, &sym)) {
				continue;
			}
			if (GELF_ST_TYPE(sym.st_info) != STT_FUNC) {
				continue;
			}
			if (sym.st_shndx == SHN_UNDEF || sym.st_value == 0) {
				continue;
			}
			const char *name =
				elf_strptr(elf, shdr.sh_link, sym.st_name);
			if (!name || name[0] == '\0') {
				continue;
			}
			names[kept] = strdup(name);
			if (!names[kept]) {
				continue;
			}
			symbols[kept].name = names[kept];
			symbols[kept].value = sym.st_value;
			symbols[kept].size = sym.st_size;
			symbols[kept].is_global =
				GELF_ST_BIND(sym.st_info) == STB_GLOBAL;
			kept++;
		}

		qsort(symbols, kept, sizeof(*symbols), symbol_cmp);

		img->symbols = symbols;
		img->symbol_count = kept;
		// The raw name-pointer table is no longer needed.
		//
		// Each string is now owned by its matching kept symbol and
		// freed individually when the image closes.
		free(names);
		return 0;
	}
	return 0;
}

struct elf_image *
elf_image_open(const char *path, char *errbuf, size_t errbuf_size) {
	if (elf_version(EV_CURRENT) == EV_NONE) {
		snprintf(errbuf, errbuf_size, "libelf version mismatch");
		return NULL;
	}

	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		snprintf(errbuf, errbuf_size, "%s: %s", path, strerror(errno));
		return NULL;
	}

	Elf *elf = elf_begin(fd, ELF_C_READ, NULL);
	if (!elf || elf_kind(elf) != ELF_K_ELF) {
		snprintf(errbuf, errbuf_size, "%s: not an ELF file", path);
		if (elf) {
			elf_end(elf);
		}
		close(fd);
		return NULL;
	}

	struct elf_image *img = calloc(1, sizeof(*img));
	if (!img) {
		snprintf(errbuf, errbuf_size, "%s: out of memory", path);
		elf_end(elf);
		close(fd);
		return NULL;
	}
	img->path = strdup(path);
	img->fd = fd;
	img->elf = elf;

	if (elf_image_load_phdrs(img, elf) != 0) {
		snprintf(errbuf, errbuf_size, "%s: no program headers", path);
		elf_image_close(img);
		return NULL;
	}
	if (elf_image_load_symtab(img, elf) != 0) {
		snprintf(errbuf, errbuf_size, "%s: malformed symtab", path);
		elf_image_close(img);
		return NULL;
	}

	return img;
}

void
elf_image_close(struct elf_image *img) {
	if (!img) {
		return;
	}
	for (size_t i = 0; i < img->symbol_count; ++i) {
		free((void *)img->symbols[i].name);
	}
	free(img->symbols);
	free(img->loads);
	free(img->path);
	if (img->elf) {
		elf_end((Elf *)img->elf);
	}
	if (img->fd >= 0) {
		close(img->fd);
	}
	free(img);
}

int
elf_image_has_symbols(const struct elf_image *img) {
	return img && img->symbol_count > 0;
}

void
elf_image_adopt_symbols(struct elf_image *img, struct elf_image *sym_src) {
	for (size_t i = 0; i < img->symbol_count; ++i) {
		free((void *)img->symbols[i].name);
	}
	free(img->symbols);

	img->symbols = sym_src->symbols;
	img->symbol_count = sym_src->symbol_count;
	// Ownership of the symbol strings moves to the target image;
	// detach them from the source so closing it does not double-free.
	sym_src->symbols = NULL;
	sym_src->symbol_count = 0;
}

static int
vaddr_to_offset(const struct elf_image *img, uint64_t vaddr, uint64_t *out) {
	for (size_t i = 0; i < img->load_count; ++i) {
		uint64_t start = img->loads[i].vaddr;
		uint64_t end = start + img->loads[i].filesz;
		if (vaddr >= start && vaddr < end) {
			*out = vaddr - start + img->loads[i].offset;
			return 1;
		}
	}
	return 0;
}

int
elf_image_symbol_vaddr(
	const struct elf_image *img,
	const char *name,
	uint64_t *vaddr,
	int *ambiguous
) {
	if (ambiguous) {
		*ambiguous = 0;
	}

	// A global symbol is preferred over a local or weak one sharing
	// the same name.
	//
	// Among candidates of the same preferred kind, more than one is
	// an unresolvable ambiguity, not an arbitrary pick.
	const struct elf_symbol *best = NULL;
	int best_is_global = 0;
	int global_candidates = 0, local_candidates = 0;

	for (size_t i = 0; i < img->symbol_count; ++i) {
		if (strcmp(img->symbols[i].name, name) != 0) {
			continue;
		}
		if (img->symbols[i].is_global) {
			global_candidates++;
			if (!best || !best_is_global) {
				best = &img->symbols[i];
				best_is_global = 1;
			}
		} else {
			local_candidates++;
			if (!best) {
				best = &img->symbols[i];
			}
		}
	}

	if (!best) {
		return 0;
	}
	int candidates =
		global_candidates > 0 ? global_candidates : local_candidates;
	if (candidates > 1) {
		if (ambiguous) {
			*ambiguous = 1;
		}
		return 0;
	}
	*vaddr = best->value;
	return 1;
}

int
elf_image_symbol_offset(
	const struct elf_image *img, const char *name, uint64_t *offset
) {
	uint64_t vaddr;
	if (!elf_image_symbol_vaddr(img, name, &vaddr, NULL)) {
		return 0;
	}
	return vaddr_to_offset(img, vaddr, offset);
}

const struct elf_symbol *
elf_image_find_containing(const struct elf_image *img, uint64_t vaddr) {
	if (img->symbol_count == 0) {
		return NULL;
	}

	// Finds the last symbol at or before the given address, via
	// binary search over the address-ascending symbol array.
	size_t lo = 0, hi = img->symbol_count;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (img->symbols[mid].value <= vaddr) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	if (lo == 0) {
		return NULL;
	}
	const struct elf_symbol *sym = &img->symbols[lo - 1];
	if (sym->size != 0 && vaddr >= sym->value + sym->size) {
		return NULL;
	}
	return sym;
}

int
elf_image_build_id(const struct elf_image *img, char *hex_out) {
	Elf *elf = (Elf *)img->elf;
	Elf_Scn *scn = NULL;
	while ((scn = elf_nextscn(elf, scn)) != NULL) {
		GElf_Shdr shdr;
		if (!gelf_getshdr(scn, &shdr) || shdr.sh_type != SHT_NOTE) {
			continue;
		}
		Elf_Data *data = elf_getdata(scn, NULL);
		if (!data) {
			continue;
		}

		size_t off = 0;
		GElf_Nhdr nhdr;
		size_t name_off, desc_off;
		while ((off = gelf_getnote(
				data, off, &nhdr, &name_off, &desc_off
			)) != 0) {
			const char *note_name =
				(const char *)data->d_buf + name_off;
			if (nhdr.n_type != NT_GNU_BUILD_ID ||
			    strcmp(note_name, "GNU") != 0) {
				continue;
			}
			// An oversized build-id note is skipped entirely.
			//
			// A truncated copy would compare equal to an
			// unrelated id that merely shares its prefix, which
			// is worse than reporting none.
			if (nhdr.n_descsz < 2 ||
			    nhdr.n_descsz > ELF_BUILD_ID_MAX_BYTES) {
				continue;
			}

			const unsigned char *id =
				(const unsigned char *)data->d_buf + desc_off;
			for (size_t i = 0; i < nhdr.n_descsz; ++i) {
				snprintf(hex_out + 2 * i, 3, "%02x", id[i]);
			}
			return 1;
		}
	}
	return 0;
}

int
elf_image_build_id_debug_path(
	const struct elf_image *img,
	const char *debug_root,
	char *out,
	size_t out_size
) {
	if (!debug_root) {
		debug_root = "/usr/lib/debug";
	}
	char hex[ELF_BUILD_ID_HEX_SIZE];
	if (!elf_image_build_id(img, hex)) {
		return 0;
	}
	int n = snprintf(
		out,
		out_size,
		"%s/.build-id/%.2s/%s.debug",
		debug_root,
		hex,
		hex + 2
	);
	return n > 0 && (size_t)n < out_size;
}
