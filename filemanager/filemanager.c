#include <stdint.h>
#include <stddef.h>

/*
 * DoomCPU File Manager
 * Guest program for DoomCPU's RV32I + SDL host-call interface.
 * The host bridge needs ECALL 38 (list directory) and ECALL 39 (run guest ELF).
 */
#define WIDTH 320
#define HEIGHT 200
#define MAX_ENTRIES 128
#define LIST_BYTES 16384
#define PATH_CAP 1024
#define NAME_CAP 256
#define VISIBLE_ROWS 12

#define KEY_UP    0xad
#define KEY_DOWN  0xaf
#define KEY_LEFT  0xac
#define KEY_RIGHT 0xae
#define KEY_ESC   27
#define KEY_ENTER 13

#define C_BG       0x00101925u
#define C_PANEL    0x001a293bu
#define C_HEADER   0x0023344bu
#define C_TEXT     0x00e5edf7u
#define C_MUTED    0x0096a9bfu
#define C_SELECT   0x002f5876u
#define C_DIR      0x007fcae8u
#define C_ELF      0x0086e0a0u
#define C_FILE     0x00aab6c5u
#define C_WARNING  0x00f4c77bu
#define C_ERROR    0x00ff8b8bu

static uint32_t framebuffer[WIDTH * HEIGHT];
static char current_path[PATH_CAP] = "filemanager";
static char list_buffer[LIST_BYTES];
static char status_text[80] = "ARROWS SELECT  ENTER OPEN/RUN  ESC QUIT";

typedef struct {
    char type;             /* D=directory, E=ELF, F=regular file, X=other */
    char name[NAME_CAP];
} Entry;

static Entry entries[MAX_ENTRIES];
static int entry_count;
static int selected;
static int scroll_top;

/* Small 5x7 uppercase bitmap font; unsupported characters render as blank. */
static const uint8_t FONT[128][7] = {
 ['A']={14,17,17,31,17,17,17}, ['B']={30,17,17,30,17,17,30},
 ['C']={14,17,16,16,16,17,14}, ['D']={30,17,17,17,17,17,30},
 ['E']={31,16,16,30,16,16,31}, ['F']={31,16,16,30,16,16,16},
 ['G']={14,17,16,23,17,17,15}, ['H']={17,17,17,31,17,17,17},
 ['I']={31,4,4,4,4,4,31}, ['J']={7,2,2,2,18,18,12},
 ['K']={17,18,20,24,20,18,17}, ['L']={16,16,16,16,16,16,31},
 ['M']={17,27,21,21,17,17,17}, ['N']={17,25,25,21,19,19,17},
 ['O']={14,17,17,17,17,17,14}, ['P']={30,17,17,30,16,16,16},
 ['Q']={14,17,17,17,21,18,13}, ['R']={30,17,17,30,20,18,17},
 ['S']={15,16,16,14,1,1,30}, ['T']={31,4,4,4,4,4,4},
 ['U']={17,17,17,17,17,17,14}, ['V']={17,17,17,17,17,10,4},
 ['W']={17,17,17,21,21,21,10}, ['X']={17,17,10,4,10,17,17},
 ['Y']={17,17,10,4,4,4,4}, ['Z']={31,1,2,4,8,16,31},
 ['0']={14,17,19,21,25,17,14}, ['1']={4,12,4,4,4,4,14},
 ['2']={14,17,1,2,4,8,31}, ['3']={30,1,1,14,1,1,30},
 ['4']={2,6,10,18,31,2,2}, ['5']={31,16,16,30,1,1,30},
 ['6']={14,16,16,30,17,17,14}, ['7']={31,1,2,4,8,8,8},
 ['8']={14,17,17,14,17,17,14}, ['9']={14,17,17,15,1,1,14},
 ['.']={0,0,0,0,0,12,12}, [',']={0,0,0,0,4,4,8},
 [':']={0,12,12,0,12,12,0}, [';']={0,12,12,0,4,4,8},
 ['-']={0,0,0,31,0,0,0}, ['_']={0,0,0,0,0,0,31},
 ['/']={1,2,2,4,8,8,16}, ['\\']={16,8,8,4,2,2,1},
 ['[']={14,8,8,8,8,8,14}, [']']={14,2,2,2,2,2,14},
 ['(']={2,4,8,8,8,4,2}, [')']={8,4,2,2,2,4,8},
 ['>']={16,8,4,2,4,8,16}, ['<']={1,2,4,8,4,2,1},
 ['!']={4,4,4,4,4,0,4}, ['?']={14,17,1,2,4,0,4},
 ['=']={0,31,0,31,0,0,0}, ['+']={0,4,4,31,4,4,0},
 ['*']={0,17,10,31,10,17,0}, ['#']={10,31,10,10,31,10,0},
 ['%']={17,2,4,8,17,0,0}, [' ']={0,0,0,0,0,0,0}
};

static size_t s_len(const char *s)
{
    size_t n = 0;
    while (s[n]) ++n;
    return n;
}

static void s_copy(char *dst, const char *src, size_t cap)
{
    size_t i = 0;
    if (!cap) return;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
}

static int s_equal(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { ++a; ++b; }
    return *a == *b;
}

static char upper_ascii(char c)
{
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    return c;
}

static int name_compare(const char *a, const char *b)
{
    while (*a && *b) {
        char aa = upper_ascii(*a), bb = upper_ascii(*b);
        if (aa != bb) return aa < bb ? -1 : 1;
        ++a; ++b;
    }
    if (*a) return 1;
    if (*b) return -1;
    return 0;
}

static void host_call3(unsigned service, uintptr_t a0, uintptr_t a1, uintptr_t a2)
{
    register uintptr_t r0 asm("a0") = a0;
    register uintptr_t r1 asm("a1") = a1;
    register uintptr_t r2 asm("a2") = a2;
    register uintptr_t r7 asm("a7") = service;
    asm volatile("ecall" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
}

static int host_list_directory(const char *path, char *buffer, uint32_t cap)
{
    register uintptr_t r0 asm("a0") = (uintptr_t)path;
    register uintptr_t r1 asm("a1") = (uintptr_t)buffer;
    register uintptr_t r2 asm("a2") = cap;
    register uintptr_t r7 asm("a7") = 38;
    asm volatile("ecall" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
    return (int32_t)r0;
}

static int host_run_guest(const char *path)
{
    register uintptr_t r0 asm("a0") = (uintptr_t)path;
    register uintptr_t r7 asm("a7") = 39;
    asm volatile("ecall" : "+r"(r0) : "r"(r7) : "memory");
    return (int32_t)r0;
}

static int poll_key(int *pressed, unsigned char *key)
{
    register uintptr_t r0 asm("a0") = (uintptr_t)pressed;
    register uintptr_t r1 asm("a1") = (uintptr_t)key;
    register uintptr_t r7 asm("a7") = 21;
    asm volatile("ecall" : "+r"(r0) : "r"(r1), "r"(r7) : "memory");
    return (int)r0;
}

static void sleep_ms(uint32_t ms)
{
    host_call3(22, ms, 0, 0);
}

static void set_title(const char *title)
{
    host_call3(24, (uintptr_t)title, 0, 0);
}

static void put_pixel(int x, int y, uint32_t color)
{
    if ((unsigned)x < WIDTH && (unsigned)y < HEIGHT)
        framebuffer[y * WIDTH + x] = color;
}

static void fill_rect(int x, int y, int w, int h, uint32_t color)
{
    int yy, xx;
    for (yy = y; yy < y + h; ++yy)
        for (xx = x; xx < x + w; ++xx)
            put_pixel(xx, yy, color);
}

static void draw_char(int x, int y, char ch, uint32_t color)
{
    int row, col;
    unsigned char c = (unsigned char)ch;
    if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
    if (c >= 128) return;
    for (row = 0; row < 7; ++row)
        for (col = 0; col < 5; ++col)
            if (FONT[c][row] & (1u << (4 - col)))
                put_pixel(x + col, y + row, color);
}

static void draw_text(int x, int y, const char *text, uint32_t color)
{
    while (*text && x < WIDTH) {
        draw_char(x, y, *text++, color);
        x += 6;
    }
}

static void set_status(const char *text)
{
    s_copy(status_text, text, sizeof(status_text));
}

static int is_root_path(void)
{
    return s_equal(current_path, "filemanager");
}

static void sort_entries(int first)
{
    int i, j;
    for (i = first; i < entry_count; ++i) {
        for (j = i + 1; j < entry_count; ++j) {
            int swap = 0;
            if (entries[j].type < entries[i].type) swap = 1;
            else if (entries[j].type == entries[i].type &&
                     name_compare(entries[j].name, entries[i].name) < 0) swap = 1;
            if (swap) {
                char type = entries[i].type;
                int k;
                entries[i].type = entries[j].type;
                entries[j].type = type;
                for (k = 0; k < NAME_CAP; ++k) {
                    char ch = entries[i].name[k];
                    entries[i].name[k] = entries[j].name[k];
                    entries[j].name[k] = ch;
                }
            }
        }
    }
}

static void refresh_entries(void)
{
    int bytes, parent_offset = 0;
    uint32_t pos = 0;
    entry_count = 0;
    if (!is_root_path()) {
        entries[entry_count].type = 'D';
        s_copy(entries[entry_count].name, "..", NAME_CAP);
        ++entry_count;
        parent_offset = 1;
    }

    bytes = host_list_directory(current_path, list_buffer, sizeof(list_buffer));
    if (bytes < 0) {
        set_status("CANNOT READ DIRECTORY - CHECK PATH");
        selected = 0; scroll_top = 0;
        return;
    }
    while (pos < (uint32_t)bytes && entry_count < MAX_ENTRIES) {
        uint32_t start = pos;
        char type = list_buffer[pos++];
        uint32_t name_start = pos;
        while (pos < (uint32_t)bytes && list_buffer[pos] != '\0') ++pos;
        if (pos >= (uint32_t)bytes) break;
        uint32_t name_len = pos - name_start;
        ++pos;
        if (name_len == 0 || name_len >= NAME_CAP) continue;
        /* Avoid accidentally opening a second nested copy of this manager. */
        if (type == 'E' && name_len == 15) {
            const char *self = "filemanager.elf";
            uint32_t k;
            int same = 1;
            for (k = 0; k < name_len; ++k)
                if (upper_ascii(list_buffer[name_start + k]) != upper_ascii(self[k])) same = 0;
            if (same) continue;
        }
        entries[entry_count].type = type;
        {
            uint32_t k;
            for (k = 0; k < name_len; ++k)
                entries[entry_count].name[k] = list_buffer[name_start + k];
            entries[entry_count].name[name_len] = '\0';
        }
        ++entry_count;
        (void)start;
    }
    sort_entries(parent_offset);
    if (entry_count == 0) {
        selected = 0; scroll_top = 0;
        set_status("EMPTY DIRECTORY - PUT GUEST ELFS HERE");
    } else {
        if (selected >= entry_count) selected = entry_count - 1;
        if (selected < 0) selected = 0;
        if (selected < scroll_top) scroll_top = selected;
        if (selected >= scroll_top + VISIBLE_ROWS) scroll_top = selected - VISIBLE_ROWS + 1;
        set_status("ARROWS SELECT  ENTER OPEN/RUN  ESC QUIT");
    }
}

static void draw_entry(int index, int y, int is_selected)
{
    const Entry *e = &entries[index];
    uint32_t color = C_FILE;
    const char *tag = "[FILE] ";
    if (e->type == 'D') { tag = "[DIR]  "; color = C_DIR; }
    else if (e->type == 'E') { tag = "[ELF]  "; color = C_ELF; }
    else if (e->type == 'X') { tag = "[OTHER]"; color = C_MUTED; }
    if (is_selected) fill_rect(4, y - 2, WIDTH - 8, 11, C_SELECT);
    draw_text(8, y, is_selected ? ">" : " ", is_selected ? C_TEXT : C_MUTED);
    draw_text(20, y, tag, color);
    draw_text(20 + 6 * 8, y, e->name, is_selected ? C_TEXT : color);
}

static void draw_screen(void)
{
    int i, row;
    char pathline[PATH_CAP + 8];
    const char *shown_path = current_path;
    size_t path_len = s_len(current_path);
    int path_x = 42;
    int visible = entry_count < VISIBLE_ROWS ? entry_count : VISIBLE_ROWS;

    fill_rect(0, 0, WIDTH, HEIGHT, C_BG);
    fill_rect(0, 0, WIDTH, 20, C_HEADER);
    draw_text(8, 6, "DOOMCPU FILE MANAGER", C_TEXT);
    draw_text(8, 25, "PATH:", C_MUTED);
    if (path_len > 45) shown_path = current_path + path_len - 45;
    s_copy(pathline, shown_path, sizeof(pathline));
    draw_text(path_x, 25, pathline, C_TEXT);
    fill_rect(4, 37, WIDTH - 8, 1, C_PANEL);

    if (entry_count == 0) {
        draw_text(12, 52, "NO ENTRIES", C_WARNING);
    } else {
        for (row = 0; row < visible; ++row) {
            i = scroll_top + row;
            if (i >= entry_count) break;
            draw_entry(i, 43 + row * 11, i == selected);
        }
    }
    fill_rect(0, 176, WIDTH, 24, C_HEADER);
    draw_text(7, 179, status_text, C_WARNING);
    draw_text(7, 190, "UP/DOWN SELECT ENTER OPEN/RUN ESC QUIT", C_MUTED);
    host_call3(20, (uintptr_t)framebuffer, WIDTH, HEIGHT);
}

static void parent_directory(void)
{
    size_t n = s_len(current_path);
    while (n > 0 && current_path[n - 1] != '/') --n;
    if (n <= 11) {
        s_copy(current_path, "filemanager", sizeof(current_path));
    } else {
        current_path[n - 1] = '\0';
    }
    selected = 0; scroll_top = 0;
    refresh_entries();
}

static void open_selected(void)
{
    Entry *e;
    size_t path_len, name_len;
    if (entry_count == 0 || selected < 0 || selected >= entry_count) return;
    e = &entries[selected];
    if (e->type == 'D') {
        if (s_equal(e->name, "..")) { parent_directory(); return; }
        path_len = s_len(current_path);
        name_len = s_len(e->name);
        if (path_len + 1 + name_len + 1 >= sizeof(current_path)) {
            set_status("PATH TOO LONG"); return;
        }
        current_path[path_len] = '/';
        {
            size_t i;
            for (i = 0; i < name_len; ++i) current_path[path_len + 1 + i] = e->name[i];
            current_path[path_len + 1 + name_len] = '\0';
        }
        selected = 0; scroll_top = 0;
        refresh_entries();
        return;
    }
    if (e->type == 'E') {
        char run_path[PATH_CAP];
        int result;
        path_len = s_len(current_path);
        name_len = s_len(e->name);
        if (path_len + 1 + name_len + 1 >= sizeof(run_path)) {
            set_status("PATH TOO LONG"); return;
        }
        {
            size_t i;
            for (i = 0; i < path_len; ++i) run_path[i] = current_path[i];
            run_path[path_len] = '/';
            for (i = 0; i < name_len; ++i) run_path[path_len + 1 + i] = e->name[i];
            run_path[path_len + 1 + name_len] = '\0';
        }
        set_status("RUNNING GUEST ELF...");
        draw_screen();
        result = host_run_guest(run_path);
        set_title("DoomCPU File Manager");
        refresh_entries();
        if (result < 0) set_status("LAUNCH FAILED - CHECK TERMINAL OUTPUT");
        else set_status("PROGRAM RETURNED - ENTER TO RUN AGAIN");
        return;
    }
    set_status("ONLY DIRECTORIES AND .ELF FILES CAN BE OPENED");
}

int main(void)
{
    int init_result;
    /* a null framebuffer initializes the shared SDL window/renderer. */
    {
        register uintptr_t r0 asm("a0") = 0;
        register uintptr_t r1 asm("a1") = WIDTH;
        register uintptr_t r2 asm("a2") = HEIGHT;
        register uintptr_t r7 asm("a7") = 20;
        asm volatile("ecall" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
        init_result = (int32_t)r0;
    }
    if (init_result < 0) {
        set_status("SDL DISPLAY INIT FAILED");
        for (;;) sleep_ms(1000);
    }
    set_title("DoomCPU File Manager");
    refresh_entries();
    for (;;) {
        int pressed;
        unsigned char key;
        draw_screen();
        while (poll_key(&pressed, &key)) {
            if (!pressed) continue;
            if (key == KEY_ESC || key == 'q') {
                register uintptr_t r0 asm("a0") = 0;
                register uintptr_t r7 asm("a7") = 10;
                asm volatile("ecall" : "+r"(r0) : "r"(r7) : "memory");
                for (;;) {}
            } else if (key == KEY_UP || key == 'w') {
                if (entry_count > 0 && selected > 0) --selected;
                if (selected < scroll_top) scroll_top = selected;
            } else if (key == KEY_DOWN || key == 's') {
                if (entry_count > 0 && selected + 1 < entry_count) ++selected;
                if (selected >= scroll_top + VISIBLE_ROWS) scroll_top = selected - VISIBLE_ROWS + 1;
            } else if (key == KEY_ENTER) {
                open_selected();
            } else if (key == KEY_LEFT) {
                parent_directory();
            } else if (key == KEY_RIGHT) {
                open_selected();
            }
        }
        sleep_ms(16);
    }
    return 0;
}
