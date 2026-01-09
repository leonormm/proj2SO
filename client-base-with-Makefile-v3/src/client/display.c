#include "board.h"
#include "api.h"
#include "display.h"
#include <stdlib.h>
#include <ctype.h>
#include <ncurses.h>

int terminal_init() {
    setenv("ESCDELAY", "25", 1);
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    timeout(20); 
    curs_set(0);
    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_YELLOW, COLOR_BLACK);
        init_pair(2, COLOR_RED, COLOR_BLACK);
        init_pair(3, COLOR_BLUE, COLOR_BLACK);
        init_pair(4, COLOR_WHITE, COLOR_BLACK);
        init_pair(5, COLOR_GREEN, COLOR_BLACK);
        init_pair(6, COLOR_MAGENTA, COLOR_BLACK);
        init_pair(7, COLOR_CYAN, COLOR_BLACK);
    }
    clear();
    return 0;
}

void draw_board_client(Board board) {
    clear();
    attron(COLOR_PAIR(5));
    mvprintw(0, 0, "=== PACMAN CLIENT ===");
    if (board.game_over) mvprintw(1, 0, " GAME OVER ");
    else if (board.victory) mvprintw(1, 0, " VICTORY ");
    else mvprintw(1, 0, "Use W/A/S/D to move | Q to quit");
    attroff(COLOR_PAIR(5));

    int start_row = 3;
    for (int y = 0; y < board.height; y++) {
        for (int x = 0; x < board.width; x++) {
            int idx = y * board.width + x;
            char ch = board.data[idx];
            move(start_row + y, x);
            
            switch (ch) {
                case '#': case 'W':
                    attron(COLOR_PAIR(3)); addch('#'); attroff(COLOR_PAIR(3));
                    break;
                case 'C': case 'P':
                    attron(COLOR_PAIR(1)|A_BOLD); addch('C'); attroff(COLOR_PAIR(1)|A_BOLD);
                    break;
                case 'M': // Fantasma Normal
                    attron(COLOR_PAIR(2)|A_BOLD); addch('M'); attroff(COLOR_PAIR(2)|A_BOLD);
                    break;
                case 'm': // Fantasma Carregado (Recebido do servidor modificado)
                    attron(COLOR_PAIR(2)|A_BOLD|A_DIM); addch('M'); attroff(COLOR_PAIR(2)|A_BOLD|A_DIM);
                    break;
                case '.':
                    attron(COLOR_PAIR(4)); addch('.'); attroff(COLOR_PAIR(4));
                    break;
                case '@':
                    attron(COLOR_PAIR(6)); addch('@'); attroff(COLOR_PAIR(6));
                    break;
                default:
                    addch(ch);
            }
        }
    }
    mvprintw(start_row + board.height + 1, 0, "Points: %d", board.accumulated_points);
    refresh();
}

void refresh_screen() { refresh(); }

char get_input() { 
    int ch = getch(); 
    return (ch == ERR) ? '\0' : toupper((char)ch); 
}

void terminal_cleanup() { 
    clear();
    refresh();
    endwin();
}

void set_timeout(int ms) { 
    timeout(ms); 
}