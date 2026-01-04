#include "board.h"
#include "display.h"
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define DO_BACKUP 3
#define EXIT_PACMAN_DIED 5

// Estrutura para passar argumentos às threads
typedef struct {
    board_t *board;
    int id;
} thread_arg_t;

// Tarefa da Thread do Pacman
void* pacman_task(void* arg) {
    thread_arg_t *data = (thread_arg_t *)arg;
    board_t *board = data->board;
    int index = data->id;
    pacman_t * pac = &board->pacmans[index];

    while (board->game_running && pac->alive) {
        command_t *cmd_ptr = NULL;
        command_t cmd_manual;

        if (pac->n_moves > 0) {
            cmd_ptr = &pac->moves[pac->current_move % pac->n_moves];
        } else {
            cmd_manual.turns = 1;
            cmd_manual.turns_left = 0;
            char dir = pac->next_direction;
            if (dir != '\0') {
                cmd_manual.command = dir;
                pac->next_direction = '\0'; // Consome o comando
            } else {
                cmd_manual.command = '\0';
            }
            cmd_ptr = &cmd_manual;
        }

        if (cmd_ptr->command != '\0') {
            int result = move_pacman(board, index, cmd_ptr);
            if (result == REACHED_PORTAL) {
                board->game_running = 0;
            } 
        }

        sleep_ms(board->tempo);
    }
    free(data);
    return NULL;
}

// Tarefa da Thread dos Fantasmas
void* ghost_task(void* arg) {
    thread_arg_t *data = (thread_arg_t*)arg;
    board_t *board = data->board;
    int index = data->id;
    ghost_t * ghost = &board->ghosts[index];

    while (board->game_running) {
        // Fantasmas movem-se autonomamente
        command_t *cmd = &ghost->moves[ghost->current_move % ghost->n_moves];
        move_ghost(board, index, cmd);
        sleep_ms(board->tempo);
    }
    free(data);
    return NULL;
} 

// Função para atualizar o ecrã
void screen_refresh(board_t * game_board, int mode) {
    draw_board(game_board, mode);
    refresh_screen();
    if(game_board->tempo != 0)
        sleep_ms(game_board->tempo);       
}

int main(int argc, char** argv) {
    if (argc != 2) {
        printf("Usage: %s <levels_directory>\n", argv[0]);
        return EXIT_FAILURE;
    }
    srand((unsigned int)time(NULL));
    board_t game_board;

    memset(&game_board, 0, sizeof(board_t));
    open_debug_file("debug.log");
    terminal_init();
    
    int accumulated_points = 0;
    bool end_game = false;
    char **lvl_paths = NULL;
    int index_lp = 0;
    int cnt_lvl = 0;

    const char *dirpath = argv[1];
    debug("Loading levels from directory: %s\n", dirpath);
    
    DIR *dirp = opendir(dirpath);
    if (dirp != NULL) {
        struct dirent *dp;
        while ((dp = readdir(dirp)) != NULL) {
            if (dp->d_name[0] == '.') continue;
            const char *ext = strrchr(dp->d_name, '.');
            if (ext && strcmp(ext, ".lvl") == 0) cnt_lvl++;
        }
        closedir(dirp);

        if (cnt_lvl == 0) {
            terminal_cleanup();
            fprintf(stderr, "No .lvl files found in the directory.\n");
            return EXIT_FAILURE;
        }

        lvl_paths = malloc(cnt_lvl * sizeof(char*));
        dirp = opendir(dirpath);
        int idx = 0;
        while ((dp = readdir(dirp)) != NULL && idx < cnt_lvl) {
            if (dp->d_name[0] == '.') continue;
            const char *ext = strrchr(dp->d_name, '.');
            if (ext && strcmp(ext, ".lvl") == 0) {
                char *path = malloc(strlen(dirpath) + strlen(dp->d_name) + 2);
                sprintf(path, "%s/%s", dirpath, dp->d_name);
                lvl_paths[idx++] = path;
            }
        }
        closedir(dirp);
    }

    index_lp = 0;
    bool has_backup = false;

    while (!end_game) {
        if (index_lp >= cnt_lvl) {
            end_game = true;
            break;
        }
        load_level_file(&game_board, lvl_paths[index_lp++], 0, accumulated_points);


        int level_result = CONTINUE_PLAY;

        while (true) {
            game_board.game_running = 1;
            
            pthread_t pacman_tid;
            pthread_t ghost_tids[MAX_GHOSTS];

            if (game_board.n_pacmans > 0) {
                thread_arg_t *arg = malloc(sizeof(thread_arg_t));
                arg->board = &game_board;
                arg->id = 0;
                pthread_create(&pacman_tid, NULL, pacman_task, arg);
            }

            for (int i = 0; i < game_board.n_ghosts; i++) {
                thread_arg_t *arg = malloc(sizeof(thread_arg_t));
                arg->board = &game_board;
                arg->id = i;
                pthread_create(&ghost_tids[i], NULL, ghost_task, arg);
            }

            int exit_reason = CONTINUE_PLAY;
            
            while (game_board.game_running) {
                draw_board(&game_board, DRAW_MENU);
                refresh_screen();

                char input = get_input();
                
                if (input == 'Q') {
                    game_board.game_running = 0;
                    exit_reason = QUIT_GAME;
                } 
                else if (input == 'G') {
                    if (!has_backup) {
                        game_board.game_running = 0;
                        exit_reason = DO_BACKUP;
                    }
                } 
                else if (input != '\0' && game_board.n_pacmans > 0) {
                    game_board.pacmans[0].next_direction = input;
                }

                if (game_board.n_pacmans > 0 && !game_board.pacmans[0].alive) {
                    game_board.game_running = 0;
                    exit_reason = QUIT_GAME;
                }

                if (game_board.n_pacmans > 0 && game_board.game_running == 0 && exit_reason == CONTINUE_PLAY) {
                    if (game_board.pacmans[0].alive)
                        exit_reason = NEXT_LEVEL;
                    else
                        exit_reason = QUIT_GAME;
                }

                sleep_ms(game_board.tempo);
            }

            if (exit_reason == CONTINUE_PLAY) {
                if (game_board.n_pacmans > 0 && game_board.pacmans[0].alive)
                    exit_reason = NEXT_LEVEL;
                else
                    exit_reason = QUIT_GAME;
            }

            if (game_board.n_pacmans > 0) pthread_join(pacman_tid, NULL);
            
            for (int i = 0; i < game_board.n_ghosts; i++) {
                pthread_join(ghost_tids[i], NULL);
            }

            if (exit_reason == DO_BACKUP) {
                pid_t pid = fork();

                if (pid < 0) {
                    perror("Fork failed");
                    exit_reason = CONTINUE_PLAY;
                } 
                else if (pid == 0) {
                    has_backup = true;
                    continue; 
                } 
                else {
                    int status;
                    wait(&status);

                    if (WIFEXITED(status)) {
                        int exit_code = WEXITSTATUS(status);

                        if (exit_code == EXIT_PACMAN_DIED) {
                            screen_refresh(&game_board, DRAW_MENU);
                            continue; 
                        } 
                        else if (exit_code == NEXT_LEVEL) {
                            level_result = NEXT_LEVEL;
                            break;
                        } 
                        else if (exit_code == QUIT_GAME) {
                            level_result = QUIT_GAME;
                            break;
                        }
                    }
                    continue;
                }
            }
            else {
                level_result = exit_reason;
                break;
            }
        } 

        if (level_result == QUIT_GAME) {
            if (!game_board.pacmans[0].alive) {
                if (has_backup) {
                    exit(EXIT_PACMAN_DIED);
                }
                screen_refresh(&game_board, DRAW_GAME_OVER);
                sleep_ms(2000);
                end_game = true;
            } else {
                if (has_backup) {
                    exit(QUIT_GAME);
                }
                screen_refresh(&game_board, DRAW_GAME_OVER);
                sleep_ms(2000);
                end_game = true;
            }
        } 
        else if (level_result == NEXT_LEVEL) {
            accumulated_points = game_board.pacmans[0].points;
            if (index_lp >= cnt_lvl) {
                draw_board(&game_board, DRAW_WIN);
                refresh_screen();
                sleep_ms(2000);
            }
            
            if (has_backup) {
                exit(NEXT_LEVEL);
            }
        }

        unload_level(&game_board);
    }

    if (lvl_paths) {
        for (int i = 0; i < cnt_lvl; i++) {
            free(lvl_paths[i]);
        }
        free(lvl_paths);
    }
    terminal_cleanup();
    close_debug_file();
    return 0;
}