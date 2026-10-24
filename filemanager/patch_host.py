#!/usr/bin/env python3
"""Patch DoomCPU src/cpu.c with the file manager's directory-list and guest-run ECALLs."""
from pathlib import Path
import shutil
import sys

root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.cwd()
source = root / "src" / "cpu.c"
if not source.is_file():
    raise SystemExit(f"Cannot find {source}; run from the DoomCPU repo root or pass the repo path")
text = source.read_text()
if "case 38:" in text and "case 39:" in text and "<dirent.h>" in text:
    print("DoomCPU host already appears to have file-manager ECALL 38/39. No changes made.")
    raise SystemExit(0)

replacements = [
    (
        "#include <unistd.h>\n",
        "#include <unistd.h>\n#include <dirent.h>\n",
    ),
    (
        "static uint64_t max_steps = DEFAULT_MAX_STEPS;\n",
        "static uint64_t max_steps = DEFAULT_MAX_STEPS;\n\nstatic int run_guest_program(const char *path);\n",
    ),
    (
        "     * 23 monotonic milliseconds, 24 window title.\n",
        "     * 23 monotonic milliseconds, 24 window title, 38 directory listing,\n"
        "     * 39 execute a nested guest ELF and return to the caller.\n",
    ),
]
for old, new in replacements:
    if old not in text:
        raise SystemExit(f"Patch anchor not found (no changes saved): {old!r}")
    text = text.replace(old, new, 1)

syscalls = r'''    case 38: {
        /* a0=directory path, a1=output buffer, a2=capacity.
           Each record is [type byte][name bytes][NUL]. Types:
           D directory, E ELF executable, F regular file, X other. */
        char path[4096];
        uint32_t addr = c->x[11], cap = c->x[12], used = 0;
        DIR *dir;
        struct dirent *de;
        if (!guest_string(c, c->x[10], path, sizeof(path))) {
            c->x[10] = (uint32_t)-EFAULT;
            break;
        }
        if (cap == 0 || !guest_span(addr, cap)) {
            c->x[10] = (uint32_t)-EFAULT;
            break;
        }
        dir = opendir(path);
        if (dir == NULL) {
            c->x[10] = (uint32_t)-errno;
            break;
        }
        while ((de = readdir(dir)) != NULL) {
            char full[8192];
            struct stat st;
            const char *name = de->d_name;
            const char *ext;
            size_t name_len;
            char type = 'X';
            int n;
            if (name[0] == '\0' || name[0] == '.')
                continue; /* dot files and dot entries are hidden */
            name_len = strlen(name);
            n = snprintf(full, sizeof(full), "%s/%s", path, name);
            if (n < 0 || (size_t)n >= sizeof(full))
                continue;
            if (lstat(full, &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    type = 'D';
                } else if (S_ISREG(st.st_mode)) {
                    ext = strrchr(name, '.');
                    if (ext != NULL && ext[1] != '\0' &&
                        (ext[1] == 'e' || ext[1] == 'E') &&
                        (ext[2] == 'l' || ext[2] == 'L') &&
                        (ext[3] == 'f' || ext[3] == 'F') && ext[4] == '\0')
                        type = 'E';
                    else
                        type = 'F';
                }
            }
            if (name_len + 2u > (size_t)(cap - used))
                break;
            c->ram[addr + used++] = (uint8_t)type;
            memcpy(c->ram + addr + used, name, name_len);
            used += (uint32_t)name_len;
            c->ram[addr + used++] = '\0';
        }
        closedir(dir);
        c->x[10] = used;
        break;
    }
    case 39: {
        /* a0=path to a guest ELF. Run it with isolated guest RAM, then return. */
        char path[4096];
        int result;
        if (!guest_string(c, c->x[10], path, sizeof(path))) {
            c->x[10] = (uint32_t)-EFAULT;
            break;
        }
        result = run_guest_program(path);
        c->x[10] = (uint32_t)result;
        break;
    }
'''
anchor = "#ifdef DOOMCPU_SDL\n    case 20: {"
if text.count(anchor) != 1:
    raise SystemExit("Could not uniquely locate the SDL ECALL insertion point (no changes saved)")
text = text.replace(anchor, syscalls + anchor, 1)

helpers = r'''static int validate_guest_elf(const char *path)
{
    FILE *f = fopen(path, "rb");
    Elf32_Ehdr eh;
    long file_size;
    int entry_is_executable = 0;
    if (f == NULL)
        return -errno;
    if (fread(&eh, 1, sizeof(eh), f) != sizeof(eh)) {
        fclose(f);
        return -ENOEXEC;
    }
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
        eh.e_ident[EI_CLASS] != ELFCLASS32 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB ||
        eh.e_ident[EI_VERSION] != EV_CURRENT ||
        eh.e_type != ET_EXEC || eh.e_version != EV_CURRENT ||
        eh.e_ehsize != sizeof(Elf32_Ehdr) || eh.e_machine != EM_RISCV ||
        eh.e_phentsize != sizeof(Elf32_Phdr)) {
        fclose(f);
        return -ENOEXEC;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (file_size = ftell(f)) < 0) {
        int err = errno ? errno : EIO;
        fclose(f);
        return -err;
    }
    if ((uint64_t)eh.e_phoff + (uint64_t)eh.e_phnum * eh.e_phentsize >
        (uint64_t)file_size) {
        fclose(f);
        return -ENOEXEC;
    }
    for (unsigned i = 0; i < eh.e_phnum; ++i) {
        uint64_t off = (uint64_t)eh.e_phoff + (uint64_t)i * eh.e_phentsize;
        Elf32_Phdr ph;
        if (fseek(f, (long)off, SEEK_SET) != 0 ||
            fread(&ph, 1, sizeof(ph), f) != sizeof(ph)) {
            fclose(f);
            return -ENOEXEC;
        }
        if (ph.p_type != PT_LOAD)
            continue;
        if (ph.p_filesz > ph.p_memsz ||
            (uint64_t)ph.p_offset + ph.p_filesz > (uint64_t)file_size ||
            ph.p_vaddr > RAM_SIZE || ph.p_memsz > RAM_SIZE - ph.p_vaddr) {
            fclose(f);
            return -ENOEXEC;
        }
        if ((ph.p_flags & PF_X) && eh.e_entry >= ph.p_vaddr &&
            (uint64_t)eh.e_entry < (uint64_t)ph.p_vaddr + ph.p_memsz)
            entry_is_executable = 1;
    }
    fclose(f);
    if ((eh.e_entry & 3u) != 0 || eh.e_entry >= RAM_SIZE || !entry_is_executable)
        return -ENOEXEC;
    return 0;
}

static int run_guest_program(const char *path)
{
    int check = validate_guest_elf(path);
    CPU *child;
    int result = 0;
    if (check < 0) {
        fprintf(stderr, "DoomCPU: cannot launch '%s' (invalid ELF or host error %d)\n",
                path, -check);
        return check;
    }
    child = calloc(1, sizeof(*child));
    if (child == NULL)
        return -ENOMEM;
    child->pc = load_elf(child, path);
    child->x[2] = RAM_SIZE - 16;
    printf("DoomCPU: launching guest %s\n", path);
    fflush(stdout);
    while (!child->halted) {
        if (child->steps++ >= max_steps) {
            fprintf(stderr, "DoomCPU: guest '%s' hit the instruction limit\n", path);
            result = -ETIMEDOUT;
            goto done;
        }
        step(child);
    }
    result = (int32_t)child->x[10];
    printf("DoomCPU: guest returned %d after %llu instructions\n",
           result, (unsigned long long)child->steps);
done:
    free(child);
    return result;
}

'''
anchor = "int main(int argc, char **argv) {"
if text.count(anchor) != 1:
    raise SystemExit("Could not locate main() insertion point (no changes saved)")
text = text.replace(anchor, helpers + anchor, 1)

backup = source.with_suffix(source.suffix + ".before-filemanager")
if not backup.exists():
    shutil.copy2(source, backup)
source.write_text(text)
ignore = root / ".gitignore"
if ignore.is_file():
    ignore_text = ignore.read_text()
    ignore_rules = ["/filemanager/*.elf", "/filemanager/**/*.elf"]
    missing_rules = [rule for rule in ignore_rules if rule not in ignore_text]
    if missing_rules:
        with ignore.open("a") as f:
            if ignore_text and not ignore_text.endswith("\n"):
                f.write("\n")
            f.write("\n# Local guest programs launched by DoomCPU File Manager\n")
            for rule in missing_rules:
                f.write(rule + "\n")
        print(f"Added guest ELF ignore rules to {ignore}")
print(f"Patched {source}")
print(f"Backup saved as {backup}")
print("Added ECALL 38 (directory listing) and ECALL 39 (nested guest ELF execution).")
