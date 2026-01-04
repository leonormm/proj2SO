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
#include <stdbool.h> // Include this header for bool type

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3 // Desativado na parte 2
#define CREATE_BACKUP 4 // Desativado na parte 2

// Estrutura partilhada entre as threads de uma sessão
typedef struct {
    board_t *board;
    int req_fd;
    int notif_fd;
    int game_exit_code;     // Código para terminar o jogo (QUIT_GAME, etc)
    char next_command;      // Comando recebido do cliente
    pthread_mutex_t cmd_lock; // Mutex para proteger o comando
} session_context_t;

typedef struct {
    board_t *board;
    int ghost_index;
} ghost_thread_arg_t;

int thread_shutdown = 0;

// Envia o estado do tabuleiro para o cliente via pipe de notificação
void send_board_update(int fd, board_t *board, int victory, int game_over) {
    int width = board->width;
    int height = board->height;
    
    // Tamanho: Header (1 char + 6 ints) + Dados (width * height chars)
    int header_size = 1 + (6 * sizeof(int));
    int data_size = width * height;
    int total_size = header_size + data_size;
    
    char *buffer = malloc(total_size);
    if (!buffer) return;

    char *ptr = buffer;

    // 1. OP Code
    *ptr = OP_CODE_BOARD; ptr++;

    // 2. Metadata (inteiros)
    memcpy(ptr, &width, sizeof(int)); ptr += sizeof(int);
    memcpy(ptr, &height, sizeof(int)); ptr += sizeof(int);
    memcpy(ptr, &board->tempo, sizeof(int)); ptr += sizeof(int);
    memcpy(ptr, &victory, sizeof(int)); ptr += sizeof(int);
    memcpy(ptr, &game_over, sizeof(int)); ptr += sizeof(int);
    
    // Pontos (Assumindo pacman 0)
    int points = (board->n_pacmans > 0) ? board->pacmans[0].points : 0;
    memcpy(ptr, &points, sizeof(int)); ptr += sizeof(int);

    // 3. Dados do Tabuleiro
    for (int i = 0; i < data_size; i++) {
        char content = board->board[i].content;
        // Se a posição estiver vazia (' '), verificar se tem elementos estáticos
        if (content == ' ') {
            if (board->board[i].has_portal) content = '@';
            else if (board->board[i].has_dot) content = '.';
        }
        *ptr = content;
        ptr++;
    }

    // Escrever no pipe de notificação (ignora SIGPIPE se o cliente fechou)
    write(fd, buffer, total_size);
    free(buffer);
}

// Thread que envia atualizações periódicas ao cliente
void* sender_thread(void *arg) {
    session_context_t *ctx = (session_context_t*) arg;
    board_t *board = ctx->board;

    // Pequena pausa inicial
    sleep_ms(board->tempo);

    while (1) {
        pthread_rwlock_rdlock(&board->state_lock);
        if (thread_shutdown) {
            pthread_rwlock_unlock(&board->state_lock);
            break;
        }

        // Enviar tabuleiro (victory=0, game_over=0 durante o jogo)
        send_board_update(ctx->notif_fd, board, 0, 0);
        
        pthread_rwlock_unlock(&board->state_lock);

        sleep_ms(board->tempo);
    }
    return NULL;
}

// Thread que recebe comandos do cliente via pipe de pedidos
void* input_listener_thread(void *arg) {
    session_context_t *ctx = (session_context_t*) arg;
    char buffer[2]; // OP_CODE + Payload

    while (1) {
        ssize_t n = read(ctx->req_fd, buffer, sizeof(char)); // Ler OP code
        if (n <= 0) break; // Pipe fechado ou erro

        if (buffer[0] == OP_CODE_PLAY) {
             // Ler o comando
            if (read(ctx->req_fd, buffer + 1, sizeof(char)) > 0) {
                pthread_mutex_lock(&ctx->cmd_lock);
                ctx->next_command = buffer[1];
                pthread_mutex_unlock(&ctx->cmd_lock);
            }
        } 
        else if (buffer[0] == OP_CODE_DISCONNECT) {
            // Cliente quer sair
            pthread_mutex_lock(&ctx->cmd_lock);
            ctx->next_command = 'Q'; // Simula comando de Quit
            pthread_mutex_unlock(&ctx->cmd_lock);
            break;
        }
    }
    return NULL;
}

void* pacman_thread(void *arg) {
    session_context_t *ctx = (session_context_t*) arg;
    board_t *board = ctx->board;
    pacman_t* pacman = &board->pacmans[0];
    int *retval = malloc(sizeof(int));
    *retval = CONTINUE_PLAY;

    while (true) {
        if(!pacman->alive) {
            *retval = LOAD_BACKUP; // Tratar como morte/restart
            break;
        }

        sleep_ms(board->tempo * (1 + pacman->passo));

        command_t c_struct;
        command_t* play = &c_struct;

        // Verificar input do cliente
        if (pacman->n_moves == 0) {
            pthread_mutex_lock(&ctx->cmd_lock);
            char cmd = ctx->next_command;
            ctx->next_command = '\0'; // Limpar comando após ler
            pthread_mutex_unlock(&ctx->cmd_lock);

            if(cmd == '\0') {
                continue;
            }
            
            c_struct.command = cmd;
            c_struct.turns = 1;
        } else {
            // Movimento automático (se houver ficheiro .p)
            play = &pacman->moves[pacman->current_move % pacman->n_moves];
        }

        debug("CMD: %c\n", play->command);

        if (play->command == 'Q') {
            *retval = QUIT_GAME;
            break;
        }
        
        // Comando 'G' desativado na parte 2
        if (play->command == 'G') {
            debug("Comando G ignorado na Parte 2\n");
            continue;
        }

        pthread_rwlock_rdlock(&board->state_lock);
        
        int result = move_pacman(board, 0, play);
        
        if (result == REACHED_PORTAL) {
            *retval = NEXT_LEVEL;
            pthread_rwlock_unlock(&board->state_lock);
            break;
        }

        if(result == DEAD_PACMAN) {
            // Na parte 2, sem backups, morte pode significar fim de jogo ou restart level
            // Aqui mantemos a lógica de LOAD_BACKUP para indicar fim da vida
            *retval = LOAD_BACKUP; 
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
    free(ghost_arg);

    ghost_t* ghost = &board->ghosts[ghost_ind];

    while (true) {
        sleep_ms(board->tempo * (1 + ghost->passo));

        pthread_rwlock_rdlock(&board->state_lock);
        if (thread_shutdown) {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
        
        move_ghost(board, ghost_ind, &ghost->moves[ghost->current_move % ghost->n_moves]);
        pthread_rwlock_unlock(&board->state_lock);
    }
}

// Substitui o main original. É chamado pelo server.c
int run_game_session(int req_fd, int notif_fd, char* level_dir_path) {
    srand((unsigned int)time(NULL));

    DIR* level_dir = opendir(level_dir_path);
    if (level_dir == NULL) {
        perror("Failed to open directory");
        return -1;
    }

    // Inicializar contexto da sessão
    session_context_t ctx;
    ctx.req_fd = req_fd;
    ctx.notif_fd = notif_fd;
    ctx.next_command = '\0';
    pthread_mutex_init(&ctx.cmd_lock, NULL);

    // debug log específico para cada processo/sessão poderia ser configurado aqui
    // open_debug_file("server_session.log"); 

    int accumulated_points = 0;
    bool end_game = false;
    board_t game_board;

    struct dirent* entry;
    // Lógica simplificada de leitura de níveis (pode precisar de ordenação alfabética para ser determinístico)
    while ((entry = readdir(level_dir)) != NULL && !end_game) {
        if (entry->d_name[0] == '.') continue;
        char *dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".lvl") != 0) continue;

        load_level(&game_board, entry->d_name, level_dir_path, accumulated_points);
        ctx.board = &game_board;

        // Enviar estado inicial
        send_board_update(notif_fd, &game_board, 0, 0);

        while(true) {
            pthread_t pacman_tid, input_tid, sender_tid;
            pthread_t *ghost_tids = malloc(game_board.n_ghosts * sizeof(pthread_t));

            thread_shutdown = 0;

            // Criar threads
            pthread_create(&input_tid, NULL, input_listener_thread, (void*) &ctx);
            pthread_create(&sender_tid, NULL, sender_thread, (void*) &ctx);
            pthread_create(&pacman_tid, NULL, pacman_thread, (void*) &ctx);

            for (int i = 0; i < game_board.n_ghosts; i++) {
                ghost_thread_arg_t *arg = malloc(sizeof(ghost_thread_arg_t));
                arg->board = &game_board;
                arg->ghost_index = i;
                pthread_create(&ghost_tids[i], NULL, ghost_thread, (void*) arg);
            }

            // Esperar pelo Pacman (lógica do jogo)
            int *retval;
            pthread_join(pacman_tid, (void**)&retval);
            int result = *retval;
            free(retval);

            // Sinalizar shutdown para ghosts e sender
            pthread_rwlock_wrlock(&game_board.state_lock);
            thread_shutdown = 1;
            pthread_rwlock_unlock(&game_board.state_lock);

            // Cancelar leitura de input se estiver bloqueada
            pthread_cancel(input_tid); 
            pthread_join(input_tid, NULL);
            pthread_join(sender_tid, NULL);
            
            for (int i = 0; i < game_board.n_ghosts; i++) {
                pthread_join(ghost_tids[i], NULL);
            }
            free(ghost_tids);

            // Processar resultado
            if(result == NEXT_LEVEL) {
                // Enviar notificação de vitória do nível? 
                // O enunciado diz apenas periodicamente updates, mas podemos mandar uma flag victory se quisermos
                // Por agora, avança para o proximo loop
                break; // Sai do loop interno, vai para o próximo nível
            }

            if(result == LOAD_BACKUP) { // Morte do Pacman
                // Enviar Game Over
                send_board_update(notif_fd, &game_board, 0, 1);
                end_game = true;
                break;
            }

            if(result == QUIT_GAME) {
                end_game = true;
                break;
            }
            
            accumulated_points = game_board.pacmans[0].points;      
        }
        
        unload_level(&game_board);
    } 

    // Se saiu do loop porque acabaram os niveis
    if (!end_game) {
        // Enviar Vitória final (tabuleiro vazio, flag victory=1)
        // Necessário criar um board dummy ou usar o último estado
        //board_t empty_board = {0}; // Cuidado aqui, melhor usar flags no último send
        send_board_update(notif_fd, &game_board, 1, 0); // Exemplo
    }

    pthread_mutex_destroy(&ctx.cmd_lock);
    closedir(level_dir);
    return 0;
}