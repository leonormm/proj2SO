#include "board.h"
#include "protocol.h" 
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3 

typedef struct {
    board_t *board;
    int req_fd;
    int notif_fd;
    int game_exit_code;
    char next_command;
    pthread_mutex_t cmd_lock;
} session_context_t;

typedef struct {
    board_t *board;
    int ghost_index;
} ghost_thread_arg_t;

int thread_shutdown = 0;

void send_board_update(int fd, board_t *board, int victory, int game_over) {
    if (!board || fd < 0) return;
    
    int width = (board->board) ? board->width : 1;
    int height = (board->board) ? board->height : 1;
    
    int32_t metadata[6];
    metadata[0] = (int32_t)width;
    metadata[1] = (int32_t)height;
    metadata[2] = (int32_t)board->tempo;
    metadata[3] = (int32_t)victory;
    metadata[4] = (int32_t)game_over;
    metadata[5] = (int32_t)((board->n_pacmans > 0 && board->pacmans) ? board->pacmans[0].points : 0);

    int header_size = 1 + sizeof(metadata);
    int map_size = width * height;
    unsigned char *packet = malloc(header_size + map_size);
    if (!packet) return;

    packet[0] = (unsigned char)OP_CODE_BOARD;
    memcpy(packet + 1, metadata, sizeof(metadata));

    if (board->board) {
        // Copia o conteúdo base do tabuleiro
        for (int i = 0; i < map_size; i++) {
            char content = board->board[i].content;
            if (content == ' ') {
                if (board->board[i].has_portal) content = '@';
                else if (board->board[i].has_dot) content = '.';
            }
            packet[header_size + i] = (unsigned char)content;
        }

        // Sobreposição de Fantasmas com Estado (Charged)
        // O loop acima copia o que está na grelha (que pode ser 'M').
        // Aqui garantimos que enviamos 'm' se estiver charged.
        for (int k = 0; k < board->n_ghosts; k++) {
            ghost_t *g = &board->ghosts[k];
            // Verifica limites e se o fantasma está vivo/ativo
            if (g->pos_x >= 0 && g->pos_x < width && g->pos_y >= 0 && g->pos_y < height) {
                int idx = g->pos_y * width + g->pos_x;
                // Se o fantasma estiver charged, enviamos 'm'
                if (g->charged) {
                    packet[header_size + idx] = 'm';
                }
            }
        }

    } else {
        memset(packet + header_size, ' ', map_size);
    }
    
    write(fd, packet, header_size + map_size);
    free(packet);
}

void* input_listener_thread(void *arg) {
    session_context_t *ctx = (session_context_t*) arg;
    unsigned char op;
    while (read(ctx->req_fd, &op, 1) > 0) {
        if (op == OP_CODE_PLAY) {
            char cmd;
            if (read(ctx->req_fd, &cmd, 1) > 0) {
                pthread_mutex_lock(&ctx->cmd_lock);
                ctx->next_command = cmd;
                pthread_mutex_unlock(&ctx->cmd_lock);
            }
        } else if (op == OP_CODE_DISCONNECT) {
            pthread_mutex_lock(&ctx->cmd_lock);
            ctx->next_command = 'Q';
            pthread_mutex_unlock(&ctx->cmd_lock);
            break;
        }
    }
    pthread_mutex_lock(&ctx->cmd_lock);
    if(ctx->next_command != 'Q') {
        ctx->next_command = 'Q';
    }
    pthread_mutex_unlock(&ctx->cmd_lock);
    return NULL;
}

void* pacman_thread(void *arg) {
    session_context_t *ctx = (session_context_t*) arg;
    board_t *board = ctx->board;
    pacman_t* pacman = &board->pacmans[0];
    int *retval = malloc(sizeof(int));
    *retval = CONTINUE_PLAY;

    while (true) {
        // 1. Obter o próximo comando (input do utilizador ou movimento automático)
        pthread_mutex_lock(&ctx->cmd_lock);
        char cmd = ctx->next_command;
        ctx->next_command = '\0';
        pthread_mutex_unlock(&ctx->cmd_lock);

        command_t c_struct = {0};
        command_t* play = NULL;

        if (cmd != '\0') {
            c_struct.command = cmd;
            c_struct.turns = 1;
            play = &c_struct;
        } else if (pacman->n_moves > 0) {
            play = &pacman->moves[pacman->current_move % pacman->n_moves];
        }

        // 2. Executar o movimento (lógica de jogo)
        if (play != NULL) {
            if (play->command == 'Q') { *retval = QUIT_GAME; break; }

            // Write lock para alterar o estado do tabuleiro
            pthread_rwlock_wrlock(&board->state_lock);
            int res = move_pacman(board, 0, play);
            pthread_rwlock_unlock(&board->state_lock);

            // Verificações de fim de nível ou morte
            if (res == REACHED_PORTAL) { *retval = NEXT_LEVEL; break; }
            if (res == DEAD_PACMAN) { *retval = LOAD_BACKUP; break; }
        }
        
        // 3. Enviar atualização ao cliente (ALTERADO)
        // Fazemos isto antes do sleep para o cliente ver o movimento imediatamente.
        // Usamos read lock porque apenas vamos ler o estado para enviar.
        pthread_rwlock_rdlock(&board->state_lock);
        if (!thread_shutdown && pacman->alive) {
             send_board_update(ctx->notif_fd, board, 0, 0);
        }
        pthread_rwlock_unlock(&board->state_lock);

        // 4. Aguardar pelo próximo ciclo (Ritmo do Jogo)
        sleep_ms(board->tempo); 
        
        // 5. Verificar se o jogo deve terminar
        pthread_rwlock_rdlock(&board->state_lock);
        if (thread_shutdown || !pacman->alive) { 
            pthread_rwlock_unlock(&board->state_lock); 
            break; 
        }
        pthread_rwlock_unlock(&board->state_lock);
    }
    return (void*) retval;
}

void* ghost_thread(void *arg) {
    ghost_thread_arg_t *ghost_arg = (ghost_thread_arg_t*) arg;
    board_t *board = ghost_arg->board;
    int ghost_ind = ghost_arg->ghost_index;
    ghost_t* ghost = &board->ghosts[ghost_ind];
    free(ghost_arg);
    while (true) {
        sleep_ms(board->tempo * (1 + ghost->passo));
        pthread_rwlock_wrlock(&board->state_lock);
        if (thread_shutdown) { pthread_rwlock_unlock(&board->state_lock); break; }
        move_ghost(board, ghost_ind, &ghost->moves[ghost->current_move % ghost->n_moves]);
        pthread_rwlock_unlock(&board->state_lock);
    }
    return NULL;
}

int run_game_session(int req_fd, int notif_fd, char* level_dir_path, int slot_id, board_t **registry, pthread_mutex_t *registry_lock) {
    srand((unsigned int)time(NULL));
    DIR* level_dir = opendir(level_dir_path);
    if (!level_dir) return -1;
    
    session_context_t ctx = {.req_fd = req_fd, .notif_fd = notif_fd, .next_command = '\0'};
    pthread_mutex_init(&ctx.cmd_lock, NULL);
    int accumulated_points = 0;
    bool session_active = true;
    board_t game_board;
    struct dirent* entry;
    
    while ((entry = readdir(level_dir)) != NULL && session_active) {
        if (entry->d_name[0] == '.' || !strstr(entry->d_name, ".lvl")) continue;
        
        memset(&game_board, 0, sizeof(board_t));
        if (load_level(&game_board, entry->d_name, level_dir_path, accumulated_points) != 0) continue;
        
        pthread_mutex_lock(registry_lock);
        registry[slot_id] = &game_board;
        pthread_mutex_unlock(registry_lock);

        ctx.board = &game_board;
        thread_shutdown = 0;
        
        pthread_t pac_tid, in_tid;
        pthread_create(&in_tid, NULL, input_listener_thread, &ctx);
        pthread_create(&pac_tid, NULL, pacman_thread, &ctx);
        
        pthread_t *g_tids = malloc(game_board.n_ghosts * sizeof(pthread_t));
        for (int i = 0; i < game_board.n_ghosts; i++) {
            ghost_thread_arg_t *a = malloc(sizeof(ghost_thread_arg_t));
            a->board = &game_board; a->ghost_index = i;
            pthread_create(&g_tids[i], NULL, ghost_thread, a);
        }
        
        int *rv; pthread_join(pac_tid, (void**)&rv);
        int res = *rv; free(rv);
        
        pthread_rwlock_wrlock(&game_board.state_lock);
        thread_shutdown = 1;
        pthread_rwlock_unlock(&game_board.state_lock);
        
        pthread_cancel(in_tid); pthread_join(in_tid, NULL);
        for (int i = 0; i < game_board.n_ghosts; i++) pthread_join(g_tids[i], NULL);
        free(g_tids);
        
        if (res == NEXT_LEVEL) accumulated_points = game_board.pacmans[0].points;
        else { 
            send_board_update(notif_fd, &game_board, 0, 1); 
            session_active = false; 
        }
        
        pthread_mutex_lock(registry_lock);
        registry[slot_id] = (board_t*)0x1;
        pthread_mutex_unlock(registry_lock);

        unload_level(&game_board);
    }
    
    pthread_mutex_lock(registry_lock);
    registry[slot_id] = NULL; // Slot livre
    pthread_mutex_unlock(registry_lock);

    if (session_active) { 
        board_t eb = {0};
        pacman_t p = {0};
        p.points = accumulated_points;
        eb.n_pacmans = 1;
        eb.pacmans = &p;
        send_board_update(notif_fd, &eb, 1, 0); 
    }
    
    pthread_mutex_destroy(&ctx.cmd_lock);
    closedir(level_dir);
    return 0;
}