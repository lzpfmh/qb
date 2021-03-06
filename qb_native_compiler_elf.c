/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2012 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Chung Leong <cleong@cal.berkeley.edu>                        |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#include <elf.h>

#ifdef __LP64__
#	define LP64_USE_PIC		1
#endif

static void qb_create_cache_folder(qb_native_compiler_context *cxt) {
	uint32_t len = (uint32_t) strlen(cxt->cache_folder_path);
	if(len == 0) {
		// use the system temp folder when no cache folder is specified
		char *temp_folder = getenv("TMPDIR");
		if(!temp_folder) {
			temp_folder = "/tmp";
		}
		len = strlen(temp_folder);
		if(temp_folder[len - 1] == '/') {
			len--;
		}
		cxt->cache_folder_path = estrndup(temp_folder, len);
	}
	mkdir(cxt->cache_folder_path, 0777);
}

static int32_t qb_launch_compiler(qb_native_compiler_context *cxt) {
	USE_TSRM

	int gcc_pipe_write[2];
	int gcc_pipe_read[2];
	int gcc_pipe_error[2];
	if(pipe(gcc_pipe_write) != 0 || pipe(gcc_pipe_read) != 0 || pipe(gcc_pipe_error) != 0) {
		return FALSE;
	}

	const char *compiler_path = QB_G(compiler_path);
	const char *compiler_env_path = QB_G(compiler_env_path);

	pid_t pid = fork();
	if(pid == 0) {
		// set up stdin, stdout, and stderr
		dup2(gcc_pipe_write[0], STDIN_FILENO);
		dup2(gcc_pipe_read[1], STDOUT_FILENO);
		dup2(gcc_pipe_error[1], STDERR_FILENO);
		close(gcc_pipe_write[0]);
		close(gcc_pipe_write[1]);
		close(gcc_pipe_read[0]);
		close(gcc_pipe_read[1]);
		close(gcc_pipe_error[0]);
		close(gcc_pipe_error[1]);

		// start gcc
		const char *args[32];
		int argc = 0;
		if(strlen(compiler_path) > 0) {
			args[argc++] = compiler_path;
		} else {
			args[argc++] = "c99";
		}
		args[argc++] = "-c";
		args[argc++] = "-O2";										// optimization level

#ifdef HAVE_GCC_MARCH_NATIVE
		args[argc++] = "-march=native";								// optimize for current CPU
#elif defined(__SSE4__)
		args[argc++] = "-msse4";
#elif defined(__SSE3__)
		args[argc++] = "-msse3";
#elif defined(__SSE2__)
		args[argc++] = "-msse2";
#elif defined(__SSE__)
		args[argc++] = "-msse";
#endif
#if defined(__ARM_ARCH_7A__)
		args[argc++] = "-mlong-calls";
#endif
		args[argc++] = "-pipe";										// use pipes for internal communication
#if !ZEND_DEBUG
		args[argc++] = "-Wp,-w";									// disable preprocessor warning
#endif
		args[argc++] = "-Wno-pointer-sign";
		args[argc++] = "-Werror=implicit-function-declaration";		// elevate implicit function declaration to an error
		args[argc++] = "-fno-stack-protector"; 						// disable stack protector
#if defined(__LP64__)
#	if LP64_USE_PIC
		args[argc++] = "-fpic";										// in 64-bit function call needs PIC since external function could be anywhere
#	else
		args[argc++] = "-mcmodel=large";							// use large memory model instead of PIC,
#	endif
#endif
		args[argc++] = "-o";
		args[argc++] = cxt->obj_file_path;
		args[argc++] = "-xc";										// indicate the source is C
		args[argc++] = "-";											// input from stdin
		args[argc++] = NULL;

		if(strlen(compiler_env_path) > 0) {
			setenv("PATH", compiler_env_path, TRUE);
		}
		execvp(args[0], (char **) args);
		_exit(255);
	}

	close(gcc_pipe_write[0]);
	close(gcc_pipe_read[1]);
	close(gcc_pipe_error[1]);

	cxt->write_stream = fdopen(gcc_pipe_write[1], "w");
	cxt->read_stream = fdopen(gcc_pipe_read[0], "r");
	cxt->error_stream = fdopen(gcc_pipe_error[0], "r");

	return (pid > 0 && cxt->write_stream && cxt->read_stream && cxt->error_stream);
}

static int32_t qb_wait_for_compiler_response(qb_native_compiler_context *cxt) {
	// close the write stream
	fclose(cxt->write_stream);
	cxt->write_stream = NULL;

	// read output from stderr
	char buffer[256];
	int count;
	while((count = fread(buffer, 1, sizeof(buffer), cxt->error_stream))) {
		if(cxt->print_errors) {
			USE_TSRM
			php_write(buffer, count TSRMLS_CC);
		}
	}

	// wait for the gcc to finish
	int status;
	wait(&status);

	if(status == -1) {
		return FALSE;
	}
	return TRUE;
}

static int32_t qb_check_symbol_strip_trailing_tag(qb_native_compiler_context *cxt, const char *name) {
	// icc creates extra symbols ending in ..0, ..1, etc.
	// don't know what they are
	uint32_t name_len = 0;
	while(name[name_len] != '\0' && name[name_len] != '.') {
		name_len++;
	}
	char *new_name = alloca(name_len + 1);
	memcpy(new_name, name, name_len + 1);
	new_name[name_len] = '\0';
	return qb_check_symbol(cxt, new_name);
}

#ifdef LP64_USE_PIC

#pragma pack(push,1)
typedef struct qb_elf_entry {
	int16_t opcode;
	int32_t rip_address;
	int16_t padding;
	void *function_address;
} qb_elf_entry;
#pragma pack(pop)

static void * qb_find_symbol_plt_entry(qb_native_compiler_context *cxt, const char *name) {
	static qb_elf_entry *procedure_linkage_table = NULL;
	uint32_t i, name_len = (uint32_t) strlen(name);
	long hash_value = zend_get_hash_value(name, name_len + 1);
	if(!procedure_linkage_table) {
		procedure_linkage_table = mmap(NULL, sizeof(qb_elf_entry) * global_native_symbol_count, PROT_EXEC | PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	}
	for(i = 0; i < global_native_symbol_count; i++) {
		qb_native_symbol *symbol = &global_native_symbols[i];
		if(symbol->hash_value == hash_value) {
			if(strcmp(symbol->name, name) == 0) {
				qb_elf_entry *plt_entry = &procedure_linkage_table[i];
				if(!plt_entry->opcode) {
					plt_entry->opcode = 0x25FF;
					plt_entry->rip_address = 2;
					plt_entry->function_address = qb_get_symbol_address(cxt, symbol);
				}
				if(plt_entry->function_address) {
					return plt_entry;
				}
			}
		}
	}
	return NULL;
}
#endif

#ifdef ELF_R_TYPE
#	undef ELF_R_TYPE
#endif
#ifdef ELF_R_SYM
#	undef ELF_R_SYM
#endif
#ifdef ELF_ST_BIND
#	undef ELF_ST_BIND
#endif
#ifdef ELF_ST_TYPE
#	undef ELF_ST_TYPE
#endif

#ifdef __LP64__
#define ELF_R_TYPE(r)			ELF64_R_TYPE(r)
#define ELF_R_SYM(r)			ELF64_R_SYM(r)
#define ELF_ST_BIND(r)			ELF64_ST_BIND(r)
#define ELF_ST_TYPE(s)			ELF64_ST_TYPE(s)
typedef Elf64_Ehdr				Elf_Ehdr;
typedef Elf64_Shdr				Elf_Shdr;
typedef Elf64_Rel				Elf_Rel;
typedef Elf64_Rela				Elf_Rela;
typedef Elf64_Sym				Elf_Sym;
#else
#define ELF_R_TYPE(r)			ELF32_R_TYPE(r)
#define ELF_R_SYM(r)			ELF32_R_SYM(r)
#define ELF_ST_BIND(r)			ELF32_ST_BIND(r)
#define ELF_ST_TYPE(s)			ELF32_ST_TYPE(s)
typedef Elf32_Ehdr				Elf_Ehdr;
typedef Elf32_Shdr				Elf_Shdr;
typedef Elf32_Rel				Elf_Rel;
typedef Elf32_Rela				Elf_Rela;
typedef Elf32_Sym				Elf_Sym;
#endif

#if defined(__x86_64__)
#	define EM_EXPECTED			EM_X86_64
#elif defined(__i386__)
#	define EM_EXPECTED			EM_386
#elif defined(__ARM_ARCH_7A__)
#	define EM_EXPECTED			EM_ARM
#endif

#ifdef QB_LITTLE_ENDIAN
#	define ELF_ENDIANNESS		1
#else
#	define ELF_ENDIANNESS		2
#endif

static int32_t qb_parse_object_file(qb_native_compiler_context *cxt, int fd) {
	struct stat stat_buf;
	size_t file_size = 0;
	uint32_t missing_symbol_count = 0;

	if(fstat(fd, &stat_buf) != -1) {
		file_size = stat_buf.st_size;
	}
	if(file_size < sizeof(Elf_Ehdr)) {
		return FALSE;
	}

	Elf_Ehdr _header, *header = &_header;
	if(read(fd, header, sizeof(Elf_Ehdr)) == -1) {
		return FALSE;
	}
	if(header->e_machine != EM_EXPECTED) {
		return FALSE;
	}	if(header->e_ident[EI_DATA] != 1) {
		return FALSE;
	}


	// check the size
	int section_header_end = header->e_shoff + header->e_shentsize * header->e_shnum;
	if(section_header_end > file_size) {
		return FALSE;
	}

	// see if it's has the ELF signature
	if(memcmp(header->e_ident, "\x7f""ELF", 4) != 0) {
		return FALSE;
	}
	if(header->e_ident[EI_DATA] != ELF_ENDIANNESS || header->e_ident[EI_VERSION] != 1) {
		return FALSE;
	}

	int section_count = header->e_shnum;
	Elf_Shdr *section_headers = alloca(sizeof(Elf_Shdr) * section_count);
	if(lseek(fd, header->e_shoff, SEEK_SET) == -1 || read(fd, section_headers, sizeof(Elf_Shdr) * header->e_shnum) == -1) {
		return FALSE;
	}

	Elf_Shdr *section_string_section_header = &section_headers[header->e_shstrndx];
	char *section_string_section = alloca(section_string_section_header->sh_size);
	if(lseek(fd, section_string_section_header->sh_offset, SEEK_SET) == -1 || read(fd, section_string_section, section_string_section_header->sh_size) == -1) {
		return FALSE;
	}

	int i, j;
	uintptr_t address = sizeof(Elf_Ehdr);
	for(i = 0; i < section_count; i++) {
		Elf_Shdr *section_header = &section_headers[i];
		section_header->sh_addr = (section_header->sh_addralign > 1) ? ALIGN_TO(address, section_header->sh_addralign) : address;
		address = section_header->sh_addr + section_header->sh_size;
	}

	// allocate memory
	cxt->binary_size = address;
	cxt->binary = mmap(NULL, cxt->binary_size, PROT_EXEC | PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if(cxt->binary == MAP_FAILED) {
		return FALSE;
	}

	// load the sections
	for(i = 0; i < section_count; i++) {
		Elf_Shdr *section_header = &section_headers[i];
		if(section_header->sh_size > 0) {
			if(lseek(fd, section_header->sh_offset, SEEK_SET) == -1 || read(fd, cxt->binary + section_header->sh_addr, section_header->sh_size) == -1) {
				return FALSE;
			}
		}
	}
	memcpy(cxt->binary, header, sizeof(Elf_Ehdr));

	// look for relocation sections
	for(i = 0; i < section_count; i++) {
		if(section_headers[i].sh_type == SHT_REL || section_headers[i].sh_type == SHT_RELA) {
			int with_addend = (section_headers[i].sh_type == SHT_RELA);
			Elf_Shdr *reloc_section_header = &section_headers[i];
			Elf_Shdr *text_section_header = &section_headers[reloc_section_header->sh_info];
			Elf_Shdr *symbol_section_header = &section_headers[reloc_section_header->sh_link];
			Elf_Shdr *string_section_header = &section_headers[symbol_section_header->sh_link];
			char *text_section = (char *) (cxt->binary + text_section_header->sh_addr);
			char *string_section = (char *) (cxt->binary + string_section_header->sh_addr);
			Elf_Sym *symbols = (Elf_Sym *) (cxt->binary + symbol_section_header->sh_addr);
			Elf_Rel *relocations = (Elf_Rel *) (cxt->binary + reloc_section_header->sh_addr);
			Elf_Rela *relocation_addends = (Elf_Rela *) (cxt->binary + reloc_section_header->sh_addr);
			uint32_t relocation_count = (with_addend) ? (uint32_t) (reloc_section_header->sh_size / sizeof(Elf_Rela)) : (uint32_t) (reloc_section_header->sh_size / sizeof(Elf_Rel));

			for(j = 0; j < relocation_count; j++) {
				Elf_Rela *relocation_addend = (with_addend) ? &relocation_addends[j] : NULL;
				Elf_Rel *relocation = (with_addend) ? (Elf_Rel *) relocation_addend : &relocations[j];
				int reloc_type = ELF_R_TYPE(relocation->r_info);
				int symbol_index = ELF_R_SYM(relocation->r_info);
				Elf_Sym *symbol = &symbols[symbol_index];
				int symbol_bind = ELF_ST_BIND(symbol->st_info);
				char *symbol_name = string_section + symbol->st_name;
				void *symbol_address;
				void *target_address = (text_section + relocation->r_offset);

				if(symbol_bind == STB_LOCAL) {
					// links to something in another section
					symbol_address = (cxt->binary + section_headers[symbol->st_shndx].sh_addr + symbol->st_value);
				} else if(symbol_bind == STB_GLOBAL) {
					// links to a symbol symbol
#ifdef LP64_USE_PIC
					if(reloc_type == R_X86_64_PLT32) {
						symbol_address = qb_find_symbol_plt_entry(cxt, symbol_name);

					} else {
						symbol_address = qb_find_symbol(cxt, symbol_name);
					}
#else
					symbol_address = qb_find_symbol(cxt, symbol_name);
#endif
					if(!symbol_address) {
						qb_report_missing_native_symbol_exception(0, symbol_name);
						missing_symbol_count++;
						continue;
					}
				} else {
					return FALSE;
				}

				intptr_t P = (intptr_t) target_address;
				intptr_t S = (intptr_t) symbol_address;
				intptr_t A = (with_addend) ? relocation_addend->r_addend : *((intptr_t *) target_address);

				switch(reloc_type) {
#if defined(__x86_64__)
					case R_X86_64_NONE:
						break;
					case R_X86_64_64:
						*((intptr_t *) target_address) = S + A;
						break;
					case R_X86_64_PLT32:
					case R_X86_64_PC32:
						*((int32_t *) target_address) = S - P + A;
						break;
					case R_X86_64_32:
						*((uint32_t *) target_address) = S + A;
						break;
					case R_X86_64_32S:
						*((int32_t *) target_address) = S + A;
						break;
#elif defined(__i386__)
#	ifndef R_386_NONE
#		define R_386_NONE		0
#	endif
#	ifndef R_386_32
#		define R_386_32			1
#	endif
#	ifndef R_386_PC32
#		define R_386_PC32		2
#	endif

					case R_386_NONE:
						break;
					case R_386_32:
						*((intptr_t *) target_address) = S + A;
						break;
					case R_386_PC32:
						*((intptr_t *) target_address) = S - P + A;
						break;
#elif defined(__ARM_ARCH_7A__)

#	ifndef R_ARM_MOVW_ABS_NC
#		define R_ARM_MOVW_ABS_NC		43
#	endif
#	ifndef R_ARM_MOVT_ABS
#		define R_ARM_MOVT_ABS			44
#	endif
#	ifndef R_ARM_THM_MOVW_ABS_NC
#		define R_ARM_THM_MOVW_ABS_NC	47
#	endif
#	ifndef R_ARM_THM_MOVT_ABS
#		define R_ARM_THM_MOVT_ABS		48
#	endif

typedef struct {
	unsigned int bit19_16:4;
	unsigned int bit25_20:6;
	unsigned int bit26:1;
	unsigned int bit31_27:5;
	unsigned int bit7_0:8;
	unsigned int bit11_8:4;
	unsigned int bit14_12:3;
	unsigned int bit15:1;
} __attribute__ ((__packed__)) thumb32_movw, thumb32_movt;

typedef struct {
	unsigned int bit11_0:12;
	unsigned int bit15_12:4;
	unsigned int bit19_16:4;
	unsigned int bit31_20:12;
} __attribute__ ((__packed__)) arm_movw, arm_movt;

					case R_ARM_ABS32:
						*((intptr_t *) target_address) = S + A;
						break;
					case R_ARM_MOVW_ABS_NC: {
						int32_t R;
						arm_movw *insn = (arm_movw *) target_address;
						A = insn->bit11_0;
						if(A & 0x0800) {
							A |= 0xF800;
						}
						R = (S + A) & 0x0000FFFF;
						insn->bit19_16 = (R >> 12);
						insn->bit11_0 = (R >> 0) & 0xFFF;
					}	break;
					case R_ARM_MOVT_ABS: {
						int32_t R;
						arm_movt *insn = (arm_movt *) target_address;
						A = insn->bit11_0;
						if(A & 0x0800) {
							A |= 0xF800;
						}
						R = (S + A) & 0xFFFF0000;
						insn->bit19_16 = (R >> 28);
						insn->bit11_0 = (R >> 16) & 0xFFF;
					}	break;
					case R_ARM_THM_MOVW_ABS_NC: {
						int32_t R;
						thumb32_movw *insn = (thumb32_movw *) target_address;
						A = (insn->bit14_12 << 8) | insn->bit7_0;
						if(A & 0x0400) {
							A |= 0xFC00;
						}
						R = (S + A) & 0x0000FFFF;
						insn->bit19_16 = (R >> 12);
						insn->bit26 = (R >> 11) & 0x1;
						insn->bit14_12 = (R >> 8) & 0x7;
						insn->bit7_0 = (R >> 0) & 0xFF;
					}	break;
					case R_ARM_THM_MOVT_ABS:  {
						int32_t R;
						thumb32_movt *insn = (thumb32_movt *) target_address;
						A = (insn->bit14_12 << 8) | insn->bit7_0;
						if(A & 0x0400) {
							A |= 0xFC00;
						}
						R = (S + A) & 0xFFFF0000;
						insn->bit19_16 = (R >> 28);
						insn->bit26 = (R >> 27) & 0x1;
						insn->bit14_12 = (R >> 24) & 0x7;
						insn->bit7_0 = (R >> 16) & 0xFF;
					}	break;
#endif
					default:
						printf("Missing relocation type (%d) for %s\n", reloc_type, symbol_name);
						return FALSE;
				}
			}
		}
	}
	mprotect(cxt->binary, cxt->binary_size, PROT_EXEC | PROT_READ);
	if(missing_symbol_count > 0) {
		return FALSE;
	}

#if defined(__ARM_ARCH_7A__)
// ARM employs separate data and instruction caches. It's possible for the
// I-cache to hold a stale copy of the code (i.e. before relocation happened),
// since our the modifications will only show up in the D-cache. We therefore
// need to flush the memory range.
//
// see: http://community.arm.com/groups/processors/blog/2010/02/17/caches-and-self-modifying-code
	__clear_cache(cxt->binary, cxt->binary + cxt->binary_size);
#endif

	// look for symbol section
	uint32_t count = 0;
	for(i = 0; i < section_count; i++) {
		if(section_headers[i].sh_type == SHT_SYMTAB) {
			Elf_Shdr *symbol_section_header = &section_headers[i];
			char *symbol_section_name = section_string_section + symbol_section_header->sh_name;
			if(strcmp(symbol_section_name, ".symtab") == 0) {
				Elf_Shdr *string_section_header = &section_headers[symbol_section_header->sh_link];
				char *string_section = (char *) (cxt->binary + string_section_header->sh_addr);
				Elf_Sym *symbols = (Elf_Sym *) (cxt->binary + symbol_section_header->sh_addr);
				uint32_t symbol_count = (uint32_t) (symbol_section_header->sh_size / sizeof(Elf_Sym));

				for(i = 0; i < symbol_count; i++) {
					Elf_Sym *symbol = &symbols[i];
					if(symbol->st_shndx < symbol_count) {
						int symbol_type = ELF_ST_TYPE(symbol->st_info);
						char *symbol_name = string_section + symbol->st_name;
						void *symbol_address = cxt->binary + section_headers[symbol->st_shndx].sh_addr + symbol->st_value;
						if(symbol_type == STT_FUNC) {
							uint32_t attached = qb_attach_symbol(cxt, symbol_name, symbol_address);
							if(!attached) {
								// error out if there's an unrecognized function
								if(!qb_check_symbol(cxt, symbol_name)) {
									if(!qb_check_symbol_strip_trailing_tag(cxt, symbol_name)) {
										return FALSE;
									}
								}
							}
							count += attached;
						} else if(symbol_type == STT_OBJECT) {
							if(strncmp(symbol_name, "QB_VERSION", 10) == 0) {
								uint32_t *p_version = symbol_address;
								cxt->qb_version = *p_version;
							}
						}
					}
				}
			}
		}
	}
	return (count > 0);
}

static int32_t qb_load_object_file(qb_native_compiler_context *cxt) {
	// map the file into memory 
	int fd = open(cxt->obj_file_path, O_RDONLY);
	int32_t result;
	if(fd == -1) {
		return FALSE;
	}
	result = qb_parse_object_file(cxt, fd);
	close(fd);
	return result;
}

static void qb_remove_object_file(qb_native_compiler_context *cxt) {
	if(cxt->binary) {
		munmap(cxt->binary, cxt->binary_size);
		cxt->binary = NULL;
		cxt->binary_size = 0;
	}
	unlink(cxt->obj_file_path);
}

void qb_free_native_code(qb_native_code_bundle *bundle) {
	munmap(bundle->memory, bundle->size);
}

#if defined(HAVE_CEXP) || defined(HAVE_CEXPF)
	#include <complex.h>
#endif

static void * qb_get_intrinsic_function_address(const char *name) {
	void *address = NULL;
#ifdef HAVE_SINCOS
	if(!address) {
		if(strcmp(name, "sincos") == 0) {
			address = sincos;
		} else if(strcmp(name, "sincosf") == 0) {
			address = sincosf;
		}
	}
#endif
#ifdef HAVE_COMPLEX_H
#	ifdef __clang__
#	else
	if(!address) {
		if(strcmp(name, "__muldc3") == 0) {
			address = __muldc3;
		} else if(strcmp(name, "__mulsc3") == 0) {
			address = __mulsc3;
		} else if(strcmp(name, "__divdc3") == 0) {
			address = __divdc3;
		} else if(strcmp(name, "__divsc3") == 0) {
			address = __divsc3;
		}
	}
#	endif	
#endif
#ifdef __INTEL_COMPILER
	extern void __libm_sse2_sincos(void);
	extern void __libm_sse2_sincosf(void);

	if(!address) {
		if(strcmp(name, "__libm_sse2_sincos") == 0) {
			address = __libm_sse2_sincos;
		} else if(strcmp(name, "__libm_sse2_sincosf") == 0) {
			address = __libm_sse2_sincosf;
		}
	}
#endif
#if defined(__ARM_ARCH_7A__)
	extern void __aeabi_d2f(void);
	extern void __aeabi_f2d(void);
	extern void __aeabi_f2ulz(void);
	extern void __aeabi_f2lz(void);
	extern void __aeabi_d2ulz(void);
	extern void __aeabi_d2lz(void);
	extern void __aeabi_ul2f(void);
	extern void __aeabi_l2f(void);
	extern void __aeabi_ul2d(void);
	extern void __aeabi_l2d(void);

	if(!address) {
		if(strcmp(name, "__aeabi_d2f") == 0) {
			address = __aeabi_d2f;
		} else if(strcmp(name, "__aeabi_f2d") == 0) {
			address = __aeabi_f2d;
		} else if(strcmp(name, "__aeabi_f2ulz") == 0) {
			address = __aeabi_f2ulz;
		} else if(strcmp(name, "__aeabi_f2lz") == 0) {
			address = __aeabi_f2lz;
		} else if(strcmp(name, "__aeabi_d2ulz") == 0) {
			address = __aeabi_d2ulz;
		} else if(strcmp(name, "__aeabi_d2lz") == 0) {
			address = __aeabi_d2lz;
		} else if(strcmp(name, "__aeabi_ul2f") == 0) {
			address = __aeabi_ul2f;
		} else if(strcmp(name, "__aeabi_l2f") == 0) {
			address = __aeabi_l2f;
		} else if(strcmp(name, "__aeabi_ul2d") == 0) {
			address = __aeabi_ul2d;
		} else if(strcmp(name, "__aeabi_l2d") == 0) {
			address = __aeabi_l2d;
		}
	}
#endif
	return address;
}
