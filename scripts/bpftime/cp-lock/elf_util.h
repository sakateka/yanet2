// Minimal ELF helper the collector uses both ways: symbol name to
// file offset, and return address back to symbol name.
//
// Deliberately narrow — only PT_LOAD segments, SHT_SYMTAB (never
// SHT_DYNSYM: a stripped binary is defined here as one without a
// .symtab, which is what sends the collector looking for a separate
// debug file), and the GNU build-id note.

#pragma once

#include <stddef.h>
#include <stdint.h>

// One entry of the target's .symtab, kept only for STT_FUNC symbols.
struct elf_symbol {
	const char *name;
	uint64_t value;
	uint64_t size;
	int is_global; // STB_GLOBAL, as opposed to STB_LOCAL or STB_WEAK.
};

struct elf_image {
	char *path;
	int fd;
	void *elf; // Elf *, opaque here so this header stays libelf-free.

	struct {
		uint64_t vaddr;
		uint64_t offset;
		uint64_t filesz;
	} *loads;
	size_t load_count;

	struct elf_symbol *symbols; // Sorted by value, ascending.
	size_t symbol_count;
};

// A GNU build-id as a lowercase hex string, sized for up to a 32-byte
// (e.g. SHA-256) build-id — nothing issued in practice is longer.
#define ELF_BUILD_ID_MAX_BYTES 32
#define ELF_BUILD_ID_HEX_SIZE (2 * ELF_BUILD_ID_MAX_BYTES + 1)

// Opens the ELF file at the given path and parses its program
// headers and .symtab, if present.
//
// Returns NULL and fills the error buffer on an I/O or ELF-format
// error; a missing .symtab is not an error here, it leaves the
// symbol table empty for the caller to react to.
struct elf_image *
elf_image_open(const char *path, char *errbuf, size_t errbuf_size);

void
elf_image_close(struct elf_image *img);

// True once a non-empty .symtab has been loaded, either from this image
// directly or merged in from a separate debug image.
int
elf_image_has_symbols(const struct elf_image *img);

// Replaces the target image's symbol table with the given one,
// keeping the target's own program headers.
//
// Used when the target binary is stripped but a build-id debug file
// or an explicitly supplied one carries the same link layout with
// symbols intact.
void
elf_image_adopt_symbols(struct elf_image *img, struct elf_image *sym_src);

// Resolves a symbol name to its link-time virtual address.
//
// A global binding is preferred over a local or weak one when more
// than one symbol shares the name; if more than one global candidate
// remains after that, resolution is ambiguous rather than an
// arbitrary pick. Returns 1 and writes the address on success, 0 if
// the symbol is not found or is ambiguous — the two cases are told
// apart by a nonzero *ambiguous on the latter.
int
elf_image_symbol_vaddr(
	const struct elf_image *img,
	const char *name,
	uint64_t *vaddr,
	int *ambiguous
);

// Resolves a symbol name to its file offset via the target image's
// own program headers.
//
// Returns 1 and writes the offset on success, 0 if the symbol or its
// containing segment is not found.
int
elf_image_symbol_offset(
	const struct elf_image *img, const char *name, uint64_t *offset
);

// Finds the function symbol whose range contains the given address.
//
// The address is file-relative, already adjusted for any runtime
// load bias. Returns NULL if no symbol covers it.
const struct elf_symbol *
elf_image_find_containing(const struct elf_image *img, uint64_t vaddr);

// Reads the target image's GNU build-id as a lowercase hex string.
//
// Returns 1 and writes it, into a buffer at least
// ELF_BUILD_ID_HEX_SIZE long, on success, 0 if the image carries no
// build-id note or the note is longer than this tool supports.
int
elf_image_build_id(const struct elf_image *img, char *hex_out);

// Builds the .build-id/xx/yyyy…debug path under the given root, from
// the target image's own GNU build-id.
//
// A NULL root means the real system convention, /usr/lib/debug.
// Returns 1 and writes the path out on success, 0 under the same
// conditions as elf_image_build_id, or when the path does not fit in
// out_size.
int
elf_image_build_id_debug_path(
	const struct elf_image *img,
	const char *debug_root,
	char *out,
	size_t out_size
);
