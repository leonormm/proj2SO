#include "board.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <libgen.h>
#include <ctype.h>
#include <pthread.h>

#define STRIDE 4096

FILE * debugfile;

// Bloqueia dois mutexes numa ordem fixa (baseada no índice) para evitar Deadlocks
static void lock_positions(board_t* board, int idx1, int idx2) {
    if (idx1 == idx2) {
        pthread_mutex_lock(&board->board[idx1].mutex);
    } else if (idx1 < idx2) {
        pthread_mutex_lock(&board->board[idx1].mutex);
        pthread_mutex_lock(&board->board[idx2].mutex);
    } else {
        pthread_mutex_lock(&board->board[idx2].mutex);
        pthread_mutex_lock(&board->board[idx1].mutex);
    }
}

// Desbloqueia os mutexes das posições
static void unlock_positions(board_t* board, int idx1, int idx2) {
    pthread_mutex_unlock(&board->board[idx1].mutex);
    if (idx1 != idx2) {
        pthread_mutex_unlock(&board->board[idx2].mutex);
    }
}


// Helper private function to find and kill pacman at specific position
static int find_and_kill_pacman(board_t* board, int new_x, int new_y) {
    for (int p = 0; p < board->n_pacmans; p++) {
        pacman_t* pac = &board->pacmans[p];
        if (pac->pos_x == new_x && pac->pos_y == new_y && pac->alive) {
            pac->alive = 0;
            kill_pacman(board, p);
            return DEAD_PACMAN;
        }
    }
    return VALID_MOVE;
}

// Helper private function for getting board position index
static inline int get_board_index(board_t* board, int x, int y) {
    return y * board->width + x;
}

// Helper private function for checking valid position
static inline int is_valid_position(board_t* board, int x, int y) {
    return (x >= 0 && x < board->width) && (y >= 0 && y < board->height); 
}

void sleep_ms(int milliseconds) {
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

int move_pacman(board_t* board, int pacman_index, command_t* command) {
    if (pacman_index < 0 || !board->pacmans[pacman_index].alive) {
        return DEAD_PACMAN; // Invalid or dead pacman
    }

    pacman_t* pac = &board->pacmans[pacman_index];
    int new_x = pac->pos_x;
    int new_y = pac->pos_y;

    if (pac->waiting > 0) {
        pac->waiting -= 1;
        return VALID_MOVE;
    }
    pac->waiting = pac->passo;

    char direction = command->command;

    if (direction == 'R') {
        char directions[] = {'W', 'S', 'A', 'D'};
        direction = directions[rand() % 4];
    }

    // Calculate new position based on direction
    switch (direction) {
        case 'W': // Up
            new_y--;
            break;
        case 'S': // Down
            new_y++;
            break;
        case 'A': // Left
            new_x--;
            break;
        case 'D': // Right
            new_x++;
            break;
        case 'T': // Wait
            if (command->turns_left == 1) {
                pac->current_move += 1; // move on
                command->turns_left = command->turns;
            }
            else command->turns_left -= 1;
            return VALID_MOVE;
        default:
            return INVALID_MOVE; // Invalid direction
    }

    // Logic for the WASD movement
    pac->current_move += 1;

    // Check boundaries
    if (!is_valid_position(board, new_x, new_y)) {
        return INVALID_MOVE;
    }

    int new_index = get_board_index(board, new_x, new_y);
    int old_index = get_board_index(board, pac->pos_x, pac->pos_y);
    char target_content = board->board[new_index].content;

    lock_positions(board, old_index, new_index);

    // Check for walls
    if (target_content == 'W') {
        unlock_positions(board, old_index, new_index);
        return INVALID_MOVE;
    }

    // Check for ghosts
    if (target_content == 'M') {
        kill_pacman(board, pacman_index); // Assume-se que temos o lock da posição atual
        unlock_positions(board, old_index, new_index);
        return DEAD_PACMAN;
    }

    if (board->board[new_index].has_portal) {
        board->board[old_index].content = ' ';
        board->board[new_index].content = 'P';
        pac->pos_x = new_x;
        pac->pos_y = new_y;
        unlock_positions(board, old_index, new_index);
        return REACHED_PORTAL;
    }

    // Collect points
    if (board->board[new_index].has_dot) {
        pac->points++;
        board->board[new_index].has_dot = 0;
    }

    board->board[old_index].content = ' ';
    pac->pos_x = new_x;
    pac->pos_y = new_y;
    board->board[new_index].content = 'P';

    unlock_positions(board, old_index, new_index);

    return VALID_MOVE;
}

// Helper private function for charged ghost movement in one direction
static int move_ghost_charged_direction(board_t* board, ghost_t* ghost, char direction, int* new_x, int* new_y) {
    int x = ghost->pos_x;
    int y = ghost->pos_y;
    *new_x = x;
    *new_y = y;
    
    #define CHECK_CELL_SAFE(cx, cy) \
        int idx = get_board_index(board, cx, cy); \
        pthread_mutex_lock(&board->board[idx].mutex); \
        char t_content = board->board[idx].content; \
        if (t_content == 'W' || t_content == 'M') { \
            pthread_mutex_unlock(&board->board[idx].mutex); \
            return VALID_MOVE;  \
        } \
        if (t_content == 'P') { \
            *new_x = cx; *new_y = cy; \
            int res = find_and_kill_pacman(board, cx, cy); \
            pthread_mutex_unlock(&board->board[idx].mutex); \
            return res; \
        } \
        pthread_mutex_unlock(&board->board[idx].mutex);

    switch (direction) {
        case 'W': // Cima
            if (y == 0) return INVALID_MOVE;
            for (int i = y - 1; i >= 0; i--) { 
                CHECK_CELL_SAFE(x, i);
                *new_y = i; 
            }
            break;
        case 'S': // Baixo
            if (y == board->height - 1) return INVALID_MOVE; 
            for (int i = y + 1; i < board->height; i++) { 
                CHECK_CELL_SAFE(x, i);
                *new_y = i; 
            }
            break;
        case 'A': // Esquerda
            if (x == 0) return INVALID_MOVE;
            for (int j = x - 1; j >= 0; j--) { 
                CHECK_CELL_SAFE(j, y);
                *new_x = j; 
            }
            break;
        case 'D': // Direita
            if (x == board->width - 1) return INVALID_MOVE;
            for (int j = x + 1; j < board->width; j++) { 
                CHECK_CELL_SAFE(j, y);
                *new_x = j; 
            }
            break;
        default:
            debug("DEFAULT CHARGED MOVE - direction = %c\n", direction);
            return INVALID_MOVE;
    }
    return VALID_MOVE;
}   

int move_ghost_charged(board_t* board, int ghost_index, char direction) {
    ghost_t* ghost = &board->ghosts[ghost_index];
    int x = ghost->pos_x;
    int y = ghost->pos_y;
    int new_x = x;
    int new_y = y;

    ghost->charged = 0; //uncharge
    int result = move_ghost_charged_direction(board, ghost, direction, &new_x, &new_y);
    if (result == INVALID_MOVE) {
        debug("DEFAULT CHARGED MOVE - direction = %c\n", direction);
        return INVALID_MOVE;
    }

    // Get board indices
    int old_index = get_board_index(board, ghost->pos_x, ghost->pos_y);
    int new_index = get_board_index(board, new_x, new_y);

    lock_positions(board, old_index, new_index);

    // Update board - clear old position (restore what was there)
    board->board[old_index].content = ' '; // Or restore the dot if ghost was on one
    // Update ghost position
    ghost->pos_x = new_x;
    ghost->pos_y = new_y;
    // Update board - set new position
    board->board[new_index].content = 'M';

    unlock_positions(board, old_index, new_index);

    return result;
}

int move_ghost(board_t* board, int ghost_index, command_t* command) {
    ghost_t* ghost = &board->ghosts[ghost_index];
    int new_x = ghost->pos_x;
    int new_y = ghost->pos_y;

    // check passo
    if (ghost->waiting > 0) {
        ghost->waiting -= 1;
        return VALID_MOVE;
    }
    ghost->waiting = ghost->passo;

    char direction = command->command;
    
    if (direction == 'R') {
        char directions[] = {'W', 'S', 'A', 'D'};
        direction = directions[rand() % 4];
    }

    // Calculate new position based on direction
    switch (direction) {
        case 'W': // Up
            new_y--;
            break;
        case 'S': // Down
            new_y++;
            break;
        case 'A': // Left
            new_x--;
            break;
        case 'D': // Right
            new_x++;
            break;
        case 'C': // Charge
            ghost->current_move += 1;
            ghost->charged = 1;
            return VALID_MOVE;
        case 'T': // Wait
            if (command->turns_left == 1) {
                ghost->current_move += 1; // move on
                command->turns_left = command->turns;
            }
            else command->turns_left -= 1;
            return VALID_MOVE;
        default:
            return INVALID_MOVE; // Invalid direction
    }

    // Logic for the WASD movement
    ghost->current_move++;
    if (ghost->charged)
        return move_ghost_charged(board, ghost_index, direction);

    // Check boundaries
    if (!is_valid_position(board, new_x, new_y)) {
        return INVALID_MOVE;
    }

    // Check board position
    int new_index = get_board_index(board, new_x, new_y);
    int old_index = get_board_index(board, ghost->pos_x, ghost->pos_y);
    char target_content = board->board[new_index].content;

    lock_positions(board, old_index, new_index);

    // Check for walls and ghosts
    if (target_content == 'W' || target_content == 'M') {
        unlock_positions(board, old_index, new_index);
        return INVALID_MOVE;
    }

    int result = VALID_MOVE;
    // Check for pacman
    if (target_content == 'P') {
        result = find_and_kill_pacman(board, new_x, new_y);
    }

    // Update board - clear old position (restore what was there)
    board->board[old_index].content = ' '; // Or restore the dot if ghost was on one
    // Update ghost position
    ghost->pos_x = new_x;
    ghost->pos_y = new_y;
    // Update board - set new position
    board->board[new_index].content = 'M';

    unlock_positions(board, old_index, new_index);

    return result;
}

void kill_pacman(board_t* board, int pacman_index) {
    debug("Killing %d pacman\n\n", pacman_index);
    pacman_t* pac = &board->pacmans[pacman_index];
    int index = pac->pos_y * board->width + pac->pos_x;

    // Remove pacman from the board
    board->board[index].content = ' ';

    // Mark pacman as dead
    pac->alive = 0;
}

// Static Loading
int load_pacman(board_t* board, int points) {
    if(board->n_pacmans == 0) {
        board->n_pacmans = 1;
        board->pacmans = calloc(1, sizeof(pacman_t));
    }
    // Coloca 'P' no tabuleiro (assumindo single-thread durante loading)
    board->board[1 * board->width + 1].content = 'P'; 
    board->pacmans[0].pos_x = 1;
    board->pacmans[0].pos_y = 1;
    board->pacmans[0].alive = 1;
    board->pacmans[0].points = points;
    return 0;
}

//Loads a pacman from file
int load_pacman_file(board_t* board, const char* filepath, int points) {
    debug("Loading Pacman file: %s\n", filepath);
    
    char** tokens = read_file((char*)filepath, board, -1);
    
    if (tokens == NULL) {
        debug("Failed to read pacman file, loading default.\n");
        load_pacman(board, points);
        return -1;
    }

    if (board->cnt_moves < 3) {
        debug("Not enough tokens in pacman file.\n");
        for(int i=0; tokens[i] != NULL; i++) free(tokens[i]);
        free(tokens);
        load_pacman(board, points);
        return -1;
    }

    board->pacmans[0].passo = atoi(tokens[0]);
    board->pacmans[0].pos_y = atoi(tokens[1]); 
    board->pacmans[0].pos_x = atoi(tokens[2]); 

    board->pacmans[0].alive = 1;
    board->pacmans[0].points = points;
    board->pacmans[0].waiting = board->pacmans[0].passo;
    board->pacmans[0].current_move = 0;
    
    int idx = board->pacmans[0].pos_y * board->width + board->pacmans[0].pos_x;
    if(idx >= 0 && idx < board->width * board->height)
        board->board[idx].content = 'P';

    int move_idx = 0;
    for (int i = 3; tokens[i] != NULL && move_idx < MAX_MOVES; i++) {
        char cmd = tokens[i][0];
        board->pacmans[0].moves[move_idx].command = cmd;
        board->pacmans[0].moves[move_idx].turns = 1; 
        
        if (cmd == 'T') {
            if (tokens[i+1] != NULL && isdigit(tokens[i+1][0])) {
                board->pacmans[0].moves[move_idx].turns = atoi(tokens[i+1]);
                i++; 
            }
        }
        board->pacmans[0].moves[move_idx].turns_left = board->pacmans[0].moves[move_idx].turns;
        move_idx++;
    }
    board->pacmans[0].n_moves = move_idx;

    if (board->pacmans[0].n_moves == 0) {
        board->pacmans[0].moves[0].command = 'T'; // Wait default
        board->pacmans[0].moves[0].turns = 1;
        board->pacmans[0].moves[0].turns_left = 1;
        board->pacmans[0].n_moves = 1;
    }

    for(int i=0; tokens[i] != NULL; i++) free(tokens[i]);
    free(tokens);

    debug("Pacman loaded at (%d,%d) with %d moves.\n", board->pacmans[0].pos_x, board->pacmans[0].pos_y, board->pacmans[0].n_moves);
    return 0;
}

// Static Loading
int load_ghost(board_t* board) {
    // Ghost 0
    board->board[3 * board->width + 1].content = 'M';
    board->ghosts[0].pos_x = 1;
    board->ghosts[0].pos_y = 3;
    board->ghosts[0].passo = 0;
    board->ghosts[0].waiting = 0;
    board->ghosts[0].current_move = 0;
    board->ghosts[0].n_moves = 16;
    for (int i = 0; i < 8; i++) {
        board->ghosts[0].moves[i].command = 'D';
        board->ghosts[0].moves[i].turns = 1; 
    }
    for (int i = 8; i < 16; i++) {
        board->ghosts[0].moves[i].command = 'A';
        board->ghosts[0].moves[i].turns = 1; 
    }

    // Ghost 1
    board->board[2 * board->width + 4].content = 'M';
    board->ghosts[1].pos_x = 4;
    board->ghosts[1].pos_y = 2;
    board->ghosts[1].passo = 1;
    board->ghosts[1].waiting = 1;
    board->ghosts[1].current_move = 0;
    board->ghosts[1].n_moves = 1;
    board->ghosts[1].moves[0].command = 'R'; 
    board->ghosts[1].moves[0].turns = 1; 
    
    return 0;
}

// Loads a ghost from file
int load_ghost_file(board_t* board, const char* filepath, int ghost_index) {
    debug("Loading Ghost %d from file: %s\n", ghost_index, filepath);
    char** tokens = read_file((char*)filepath, board, -1);
    
    if (tokens == NULL) {
        debug("Failed to read ghost file. Using fallback.\n");
        board->ghosts[ghost_index].n_moves = 1;
        board->ghosts[ghost_index].moves[0].command = 'T';
        board->ghosts[ghost_index].moves[0].turns = 1;
        board->ghosts[ghost_index].moves[0].turns_left = 1;
        board->ghosts[ghost_index].pos_x = 1;
        board->ghosts[ghost_index].pos_y = 1;
        board->ghosts[ghost_index].passo = 10;
        return -1;
    }
    
    if (board->cnt_moves < 3) {
        for(int i=0; tokens[i] != NULL; i++) free(tokens[i]);
        free(tokens);
        board->ghosts[ghost_index].n_moves = 1;
        board->ghosts[ghost_index].moves[0].command = 'T';
        board->ghosts[ghost_index].moves[0].turns = 1;
        return -1;
    }

    board->ghosts[ghost_index].passo = atoi(tokens[0]);
    board->ghosts[ghost_index].pos_y = atoi(tokens[1]); 
    board->ghosts[ghost_index].pos_x = atoi(tokens[2]); 
    
    int idx = board->ghosts[ghost_index].pos_y * board->width + board->ghosts[ghost_index].pos_x;
    if(idx >= 0 && idx < board->width * board->height)
        board->board[idx].content = 'M';
        
    board->ghosts[ghost_index].waiting = board->ghosts[ghost_index].passo;
    board->ghosts[ghost_index].current_move = 0;
    
    int move_idx = 0;
    for (int i = 3; tokens[i] != NULL && move_idx < MAX_MOVES; i++) {
        char cmd = tokens[i][0];
        board->ghosts[ghost_index].moves[move_idx].command = cmd;
        board->ghosts[ghost_index].moves[move_idx].turns = 1;

        if (cmd == 'T') {
            if (tokens[i+1] != NULL && isdigit(tokens[i+1][0])) {
                board->ghosts[ghost_index].moves[move_idx].turns = atoi(tokens[i+1]);
                i++;
            }
        }
        board->ghosts[ghost_index].moves[move_idx].turns_left = board->ghosts[ghost_index].moves[move_idx].turns;
        move_idx++;
    }
    board->ghosts[ghost_index].n_moves = move_idx;

    if (board->ghosts[ghost_index].n_moves == 0) {
        board->ghosts[ghost_index].moves[0].command = 'T';
        board->ghosts[ghost_index].moves[0].turns = 1;
        board->ghosts[ghost_index].moves[0].turns_left = 1;
        board->ghosts[ghost_index].n_moves = 1;
    }

    for(int i=0; tokens[i] != NULL; i++) free(tokens[i]);
    free(tokens);
    
    return 0;
}

// Static Loading
int load_level(board_t *board, int points) {
    board->height = 5;
    board->width = 10;
    board->tempo = 10;

    board->n_ghosts = 2;
    board->n_pacmans = 1;

    board->board = calloc(board->width * board->height, sizeof(board_pos_t));
    
    for (int i = 0; i < board->width * board->height; i++) {
        pthread_mutex_init(&board->board[i].mutex, NULL);
    }

    board->pacmans = calloc(board->n_pacmans, sizeof(pacman_t));
    board->ghosts = calloc(board->n_ghosts, sizeof(ghost_t));

    sprintf(board->level_name, "Static Level");

    for (int i = 0; i < board->height; i++) {
        for (int j = 0; j < board->width; j++) {
            if (i == 0 || j == 0 || j == (board->width - 1)) {
                board->board[i * board->width + j].content = 'W'; 
            }
            else if (i == 4 && j == 8) {
                board->board[i * board->width + j].content = ' ';
                board->board[i * board->width + j].has_portal = 1;
            }
            else {
                board->board[i * board->width + j].content = ' ';
                board->board[i * board->width + j].has_dot = 1;
            }
        }
    }

    load_ghost(board);
    load_pacman(board, points);

    return 0;
}

// Loads level from a file
int load_level_file(board_t *board, const char *filepath, int max_files_to_load, int points) {
    (void)max_files_to_load; 
    
    debug("Loading level from file: %s\n", filepath);
    
    board->n_pacmans = 0;
    board->n_ghosts = 0;
    memset(board->pacman_file, 0, sizeof(board->pacman_file));
    
    read_file((char*)filepath, board, 1); 
    
    debug("Level structure read. Pacman file: %s, Ghosts: %d\n", board->pacman_file, board->n_ghosts);

    if (strlen(board->pacman_file) > 0) {
        char path_buffer[512];
        char *dirc = strdup(filepath);
        char *dname = dirname(dirc);
        snprintf(path_buffer, sizeof(path_buffer), "%s/%s", dname, board->pacman_file);
        load_pacman_file(board, path_buffer, points);
        free(dirc);
    } else {
        load_pacman(board, points);
    }

    char *dirc = strdup(filepath);
    char *dname = dirname(dirc);
    for (int i = 0; i < board->n_ghosts; i++) {
        char path_buffer[512];
        snprintf(path_buffer, sizeof(path_buffer), "%s/%s", dname, board->ghosts_files[i]);
        load_ghost_file(board, path_buffer, i);
    }
    free(dirc);

    sprintf(board->level_name, "%s", basename((char*)filepath));
    return 0;
}

// Reads a file and parses it into the board structure or tokens
char** read_file(char* filepath, board_t *board, int max_files_to_load) {
    debug("Reading file: %s (Mode: %d)\n", filepath, max_files_to_load);
    
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        perror("Failed to open file");
        return NULL;
    }
    
    char *file_content = malloc(STRIDE);
    if (!file_content) {
        close(fd);
        return NULL;
    }
    
    ssize_t bytes_read;
    size_t total_read = 0;
    size_t current_capacity = STRIDE;

    while ((bytes_read = read(fd, file_content + total_read, current_capacity - total_read - 1)) > 0) {
        total_read += bytes_read;
        if (total_read >= current_capacity - 1) {
            current_capacity *= 2;
            char *new_content = realloc(file_content, current_capacity);
            if (!new_content) {
                free(file_content);
                close(fd);
                return NULL;
            }
            file_content = new_content;
        }
    }
    file_content[total_read] = '\0';
    close(fd);

    if (max_files_to_load != -1) {
        board->current_board_line = 0;
        char *saveptr;
        char *line = strtok_r(file_content, "\n", &saveptr);
        while (line != NULL) {
            if (line[0] != '#' && line[0] != '\0' && line[0] != '\r') {
                parse_line(board, line);
            }
            line = strtok_r(NULL, "\n", &saveptr);
        }
        free(file_content);
        return NULL;
    } 
    
    else {
        board->cnt_moves = 0;
        size_t tokens_cap = 64;
        size_t tokens_count = 0;
        char **tokens = malloc(sizeof(char*) * tokens_cap);
        if(!tokens) {
            free(file_content);
            return NULL;
        }

        char *saveptr_line;
        char *line = strtok_r(file_content, "\n", &saveptr_line);
        while (line != NULL) {
             if (line[0] == '#' || line[0] == '\0') {
                line = strtok_r(NULL, "\n", &saveptr_line);
                continue;
             }
             
             if (strncmp(line, "PASSO", 5) == 0) {
                 char val[32];
                 if (sscanf(line + 5, "%s", val) == 1) {
                    if (tokens_count >= tokens_cap - 1) {
                        tokens_cap *= 2;
                        char** new_tokens = realloc(tokens, sizeof(char*) * tokens_cap);
                        if(new_tokens) tokens = new_tokens;
                    }
                    tokens[tokens_count++] = strdup(val);
                 }
            } 
            else if (strncmp(line, "POS", 3) == 0) {
                char x[32], y[32];
                if (sscanf(line + 3, "%s %s", x, y) == 2) {
                    if (tokens_count >= tokens_cap - 2) {
                        tokens_cap *= 2;
                        char** new_tokens = realloc(tokens, sizeof(char*) * tokens_cap);
                        if(new_tokens) tokens = new_tokens;
                    }
                    tokens[tokens_count++] = strdup(x);
                    tokens[tokens_count++] = strdup(y);
                }
            }
            else {
                char *saveptr_tok;
                char *tok = strtok_r(line, " \t\r", &saveptr_tok);
                while(tok) {
                    if (tokens_count >= tokens_cap - 1) {
                        tokens_cap *= 2;
                        char** new_tokens = realloc(tokens, sizeof(char*) * tokens_cap);
                        if(new_tokens) tokens = new_tokens;
                    }
                    tokens[tokens_count++] = strdup(tok);
                    tok = strtok_r(NULL, " \t\r", &saveptr_tok);
                }
                board->cnt_moves++;
            }
            line = strtok_r(NULL, "\n", &saveptr_line);
        }
        tokens[tokens_count] = NULL;
        free(file_content);
        return tokens;
    }
}

// Parses a single line from the level file
char* parse_line(board_t *board, char *line) {
    if (strncmp(line, "DIM", 3) == 0) {
        sscanf(line + 3, "%d %d", &board->width, &board->height);
        board->board = calloc(board->width * board->height, sizeof(board_pos_t));
        
        for (int i = 0; i < board->width * board->height; i++) {
            pthread_mutex_init(&board->board[i].mutex, NULL);
        }

    } else if (strncmp(line, "TEMPO", 5) == 0) {
        sscanf(line + 5, "%d", &board->tempo);
    } else if (strncmp(line, "PAC", 3) == 0) {
        sscanf(line + 3, "%s", board->pacman_file);
        board->n_pacmans = 1;
        board->pacmans = calloc(1, sizeof(pacman_t));
    } else if (strncmp(line, "MON", 3) == 0) {
        char *saveptr;
        char *token = strtok_r(line + 3, " ", &saveptr);
        int idx = 0;
        while (token != NULL && idx < MAX_GHOSTS) {
            token[strcspn(token, "\r\n")] = 0;
            if(strlen(token) > 0) {
                strcpy(board->ghosts_files[idx++], token);
            }
            token = strtok_r(NULL, " ", &saveptr);
        }
        board->n_ghosts = idx;
        board->ghosts = calloc(board->n_ghosts, sizeof(ghost_t));
    } else {
        if (board->board == NULL) return NULL;
        line[strcspn(line, "\r\n")] = 0;
        int row = board->current_board_line;
        if (row < board->height) {
            for (int col = 0; col < board->width && line[col] != '\0'; col++) {
                int index = row * board->width + col;
                char c = line[col];
                
                if (c == 'X') {
                    board->board[index].content = 'W';
                } else if (c == '@') {
                    board->board[index].content = ' ';
                    board->board[index].has_portal = 1;
                } else if (c == 'o') {
                    board->board[index].content = ' ';
                    board->board[index].has_dot = 1;
                } else {
                    board->board[index].content = ' ';
                }
            }
            board->current_board_line++;
        }
    }
    return NULL;
}

void unload_level(board_t * board) {
    if(board->board) {
        for (int i = 0; i < board->width * board->height; i++) {
            pthread_mutex_destroy(&board->board[i].mutex);
        }
        free(board->board);
    }
    if(board->pacmans) free(board->pacmans);
    if(board->ghosts) free(board->ghosts);
    board->board = NULL;
    board->pacmans = NULL;
    board->ghosts = NULL;
}

void open_debug_file(char *filename) {
    debugfile = fopen(filename, "w");
}

void close_debug_file() {
    fclose(debugfile);
}

void debug(const char * format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(debugfile, format, args);
    va_end(args);

    fflush(debugfile);
}

void print_board(board_t *board) {
    if (!board || !board->board) {
        debug("[%d] Board is empty or not initialized.\n", getpid());
        return;
    }

    // Large buffer to accumulate the whole output
    char buffer[8192];
    size_t offset = 0;

    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "=== [%d] LEVEL INFO ===\n"
                       "Dimensions: %d x %d\n"
                       "Tempo: %d\n"
                       "Pacman file: %s\n",
                       getpid(), board->height, board->width, board->tempo, board->pacman_file);

    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "Monster files (%d):\n", board->n_ghosts);

    for (int i = 0; i < board->n_ghosts; i++) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                           "  - %s\n", board->ghosts_files[i]);
    }

    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\n=== BOARD ===\n");
    
    for (int y = 0; y < board->height; y++) {
        for (int x = 0; x < board->width; x++) {
            int idx = y * board->width + x;
            if (offset < sizeof(buffer) - 2) {
                buffer[offset++] = board->board[idx].content;
            }
        }
        if (offset < sizeof(buffer) - 2) {
            buffer[offset++] = '\n';
        }
    }

    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "==================\n");

    buffer[offset] = '\0';

    debug("%s", buffer);
}