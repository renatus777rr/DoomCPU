#include <stdint.h>
#include <stddef.h>

/* Snake guest program for DoomCPU's RV32I + SDL ECALL interface. */
#define WIDTH 320
#define HEIGHT 200
#define CELL 10
#define GRID_W (WIDTH / CELL)
#define GRID_H (HEIGHT / CELL)
#define MAX_SNAKE (GRID_W * GRID_H)

#define KEY_UP    0xad
#define KEY_DOWN  0xaf
#define KEY_LEFT  0xac
#define KEY_RIGHT 0xae
#define KEY_ESC   27

#define COLOR_BG       0x00131b27u
#define COLOR_GRID     0x001b2635u
#define COLOR_WALL     0x0062768cu
#define COLOR_SNAKE    0x0034cf83u
#define COLOR_HEAD     0x0088ffb4u
#define COLOR_FOOD     0x00ff6248u
#define COLOR_GAMEOVER 0x007b2636u

static uint32_t framebuffer[WIDTH * HEIGHT];
static int snake_x[MAX_SNAKE];
static int snake_y[MAX_SNAKE];
static int snake_len;
static int food_x;
static int food_y;
static int food_valid;
static uint32_t rng_state = 0x19780420u;

static inline void host_call3(unsigned service,
                              uintptr_t arg0,
                              uintptr_t arg1,
                              uintptr_t arg2)
{
    register uintptr_t a0 asm("a0") = arg0;
    register uintptr_t a1 asm("a1") = arg1;
    register uintptr_t a2 asm("a2") = arg2;
    register uintptr_t a7 asm("a7") = service;
    asm volatile("ecall"
                 : "+r"(a0)
                 : "r"(a1), "r"(a2), "r"(a7)
                 : "memory");
}

static void set_title(const char *title)
{
    host_call3(24, (uintptr_t)title, 0, 0);
}

/* Put the score in the SDL window title without depending on a guest libc. */
static void set_score_title(int score, int state)
{
    static char title[128];
    char digits[12];
    int digit_count = 0;
    int i = 0;
    const char *prefix = "DoomCPU Snake - Score ";
    const char *suffix;

    while (*prefix != '\0')
        title[i++] = *prefix++;

    if (score == 0)
        digits[digit_count++] = '0';
    while (score > 0) {
        digits[digit_count++] = (char)('0' + score % 10);
        score /= 10;
    }
    while (digit_count > 0)
        title[i++] = digits[--digit_count];

    if (state == 1)
        suffix = " | GAME OVER: R restart, Q quit";
    else if (state == 2)
        suffix = " | YOU WIN: R restart, Q quit";
    else
        suffix = " | Arrows/WASD move, Q quits";
    while (*suffix != '\0')
        title[i++] = *suffix++;
    title[i] = '\0';
    set_title(title);
}

static void draw_frame(void)
{
    host_call3(20, (uintptr_t)framebuffer, WIDTH, HEIGHT);
}

static void sleep_ms(uint32_t ms)
{
    host_call3(22, ms, 0, 0);
}

static int poll_key(int *pressed, unsigned char *key)
{
    register uintptr_t a0 asm("a0") = (uintptr_t)pressed;
    register uintptr_t a1 asm("a1") = (uintptr_t)key;
    register uintptr_t a7 asm("a7") = 21;
    asm volatile("ecall"
                 : "+r"(a0)
                 : "r"(a1), "r"(a7)
                 : "memory");
    return (int)a0;
}

static void fill_rect(int x, int y, int w, int h, uint32_t color)
{
    int yy;
    for (yy = y; yy < y + h; ++yy) {
        int xx;
        if (yy < 0 || yy >= HEIGHT)
            continue;
        for (xx = x; xx < x + w; ++xx) {
            if (xx >= 0 && xx < WIDTH)
                framebuffer[yy * WIDTH + xx] = color;
        }
    }
}

static void draw_cell(int gx, int gy, uint32_t color)
{
    fill_rect(gx * CELL + 1, gy * CELL + 1,
              CELL - 2, CELL - 2, color);
}

static uint32_t next_random(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

static int place_food(void)
{
    int tries;
    for (tries = 0; tries < MAX_SNAKE * 2; ++tries) {
        int i;
        int occupied = 0;
        food_x = (int)(next_random() % (GRID_W - 2)) + 1;
        food_y = (int)(next_random() % (GRID_H - 2)) + 1;
        for (i = 0; i < snake_len; ++i) {
            if (snake_x[i] == food_x && snake_y[i] == food_y) {
                occupied = 1;
                break;
            }
        }
        if (!occupied) {
            food_valid = 1;
            return 1;
        }
    }
    food_valid = 0;
    return 0;
}

static void draw_board(int game_over)
{
    int i;
    int x;
    int y;
    uint32_t bg = game_over ? COLOR_GAMEOVER : COLOR_BG;

    for (i = 0; i < WIDTH * HEIGHT; ++i)
        framebuffer[i] = bg;

    /* Subtle cell guides. */
    for (x = 0; x < WIDTH; x += CELL)
        fill_rect(x, 0, 1, HEIGHT, COLOR_GRID);
    for (y = 0; y < HEIGHT; y += CELL)
        fill_rect(0, y, WIDTH, 1, COLOR_GRID);

    /* The outer ring is a wall. */
    for (x = 0; x < GRID_W; ++x) {
        draw_cell(x, 0, COLOR_WALL);
        draw_cell(x, GRID_H - 1, COLOR_WALL);
    }
    for (y = 1; y < GRID_H - 1; ++y) {
        draw_cell(0, y, COLOR_WALL);
        draw_cell(GRID_W - 1, y, COLOR_WALL);
    }

    if (food_valid)
        draw_cell(food_x, food_y, COLOR_FOOD);

    for (i = snake_len - 1; i >= 0; --i)
        draw_cell(snake_x[i], snake_y[i], i == 0 ? COLOR_HEAD : COLOR_SNAKE);

    draw_frame();
}

/* Directions: 0=up, 1=right, 2=down, 3=left. */
static int play_round(void)
{
    int direction = 1;
    int score = 0;
    int won = 0;
    int i;

    snake_len = 4;
    for (i = 0; i < snake_len; ++i) {
        snake_x[i] = GRID_W / 2 - i;
        snake_y[i] = GRID_H / 2;
    }
    (void)place_food();
    set_score_title(0, 0);
    draw_board(0);

    while (1) {
        int pressed;
        unsigned char key;

        while (poll_key(&pressed, &key)) {
            int next_direction = direction;
            if (!pressed)
                continue;
            if (key == 'q' || key == KEY_ESC)
                return 0;

            if (key == KEY_UP || key == 'w')
                next_direction = 0;
            else if (key == KEY_RIGHT || key == 'd')
                next_direction = 1;
            else if (key == KEY_DOWN || key == 's')
                next_direction = 2;
            else if (key == KEY_LEFT || key == 'a')
                next_direction = 3;

            /* Do not allow an immediate 180-degree reversal. */
            if ((next_direction + 2) % 4 != direction)
                direction = next_direction;
        }

        {
            int nx = snake_x[0];
            int ny = snake_y[0];
            int eating;
            int body_count;

            if (direction == 0) --ny;
            else if (direction == 1) ++nx;
            else if (direction == 2) ++ny;
            else --nx;

            if (nx <= 0 || nx >= GRID_W - 1 ||
                ny <= 0 || ny >= GRID_H - 1)
                break;

            eating = food_valid && nx == food_x && ny == food_y;
            /* The tail moves away on ordinary steps, unless eating. */
            body_count = snake_len - (eating ? 0 : 1);
            for (i = 0; i < body_count; ++i) {
                if (snake_x[i] == nx && snake_y[i] == ny)
                    break;
            }
            if (i < body_count)
                break;

            if (eating) {
                if (snake_len < MAX_SNAKE)
                    ++snake_len;
                ++score;
                set_score_title(score, 0);
                if (!place_food())
                    won = 1;
            }

            for (i = snake_len - 1; i > 0; --i) {
                snake_x[i] = snake_x[i - 1];
                snake_y[i] = snake_y[i - 1];
            }
            snake_x[0] = nx;
            snake_y[0] = ny;
        }

        draw_board(0);
        if (won)
            break;
        sleep_ms(100);
    }

    set_score_title(score, won ? 2 : 1);
    draw_board(!won);
    for (;;) {
        int pressed;
        unsigned char key;
        while (poll_key(&pressed, &key)) {
            if (!pressed)
                continue;
            if (key == 'q' || key == KEY_ESC)
                return 0;
            if (key == 'r')
                return 1;
        }
        sleep_ms(20);
    }
}

int main(void)
{
    /* ECALL 20 initializes the SDL window when the framebuffer is null. */
    host_call3(20, 0, WIDTH, HEIGHT);

    while (play_round())
        ;
    return 0;
}
