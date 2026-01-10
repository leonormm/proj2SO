#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <semaphore.h>
#include <errno.h>
#include "protocol.h"
#include "board.h"

// --- DEFINIÇÕES DO BUFFER ---
#define BUFF_SIZE 10

typedef struct {
    char req_pipe[40];
    char notif_pipe[40];
    char level_dir[256];
} session_request_t;

typedef struct {
    session_request_t buf[BUFF_SIZE];
    int in;
    int out;
    sem_t *sem_full;
    sem_t *sem_empty;
    pthread_mutex_t mutex;
} request_buffer_t;

request_buffer_t req_buffer;

typedef struct {
    int slot_id;
    int points;
    board_t *board;
} game_entry_t;

// --- GLOBAIS ---
board_t **active_boards;
// NOVO: Array para guardar nomes dos clientes ativos e evitar duplicados
char **active_player_names; 
pthread_mutex_t active_players_lock = PTHREAD_MUTEX_INITIALIZER;

int max_sessions = 0;
pthread_mutex_t boards_lock = PTHREAD_MUTEX_INITIALIZER;
volatile sig_atomic_t print_stats_request = 0;

char sem_full_name[64];
char sem_empty_name[64];

// Função declarada no game.c
int run_game_session(int req_fd, int notif_fd, char* level_dir, int slot_id, board_t **registry, pthread_mutex_t *registry_lock);

void handle_signal(int sig) {
    if (sig == SIGUSR1) {
        print_stats_request = 1;
    }
}

int compare_scores(const void *a, const void *b) {
    game_entry_t *entryA = (game_entry_t *)a;
    game_entry_t *entryB = (game_entry_t *)b;
    return entryB->points - entryA->points;
}

void log_active_games() {
    FILE *f = fopen("server_log.txt", "w");
    if (!f) return;

    pthread_mutex_lock(&boards_lock);
    
    // 1. Recolher todos os jogos ativos para um array temporário
    // Alocamos espaço para o máximo possível de sessões
    game_entry_t *entries = malloc(sizeof(game_entry_t) * max_sessions);
    int count = 0;

    for (int i = 0; i < max_sessions; i++) {
        // Verifica se o slot tem um jogo válido e ativo
        if (active_boards[i] != NULL && active_boards[i] != (board_t*)0x1) {
            entries[count].slot_id = i;
            entries[count].board = active_boards[i];
            
            // Assume-se sempre o pacman 0 (single player por sessão)
            if (active_boards[i]->pacmans && active_boards[i]->n_pacmans > 0) {
                entries[count].points = active_boards[i]->pacmans[0].points;
            } else {
                entries[count].points = 0;
            }
            count++;
        }
    }

    // 2. Ordenar por pontuação (Decrescente)
    qsort(entries, count, sizeof(game_entry_t), compare_scores);

    // 3. Escrever os Top 5 (ou menos, se houver menos jogos)
    fprintf(f, "=== PACMANIST SERVER LOG (PID %d) ===\n", getpid());
    fprintf(f, "Jogos Ativos: %d | A mostrar: Top 5\n\n", count);

    int limit = (count < 5) ? count : 5;
    
    for (int i = 0; i < limit; i++) {
        board_t *b = entries[i].board;
        fprintf(f, "-- Rank %d [Slot %d] --\n", i + 1, entries[i].slot_id);
        fprintf(f, "   Nível: %s | Dim: %dx%d\n", b->level_name, b->width, b->height);
        fprintf(f, "   Pacman Pos: (%d, %d)\n", b->pacmans[0].pos_x, b->pacmans[0].pos_y);
        fprintf(f, "   PONTOS: %d \n\n", entries[i].points);
    }
    
    if (count == 0) fprintf(f, "Nenhum jogo ativo no momento.\n");

    free(entries);
    pthread_mutex_unlock(&boards_lock);
    fclose(f);
    
    // Mensagem no terminal do servidor para confirmar que o log foi feito
    printf("Log gerado: Top %d jogos guardados em server_log.txt\n", limit);
}

// --- TAREFA TRABALHADORA (WORKER) ---
void* worker_thread(void* arg) {
    int slot_id = *(int*)arg;
    free(arg);

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    while (1) {
        session_request_t req;

        sem_wait(req_buffer.sem_full); 
        
        pthread_mutex_lock(&req_buffer.mutex);
        req = req_buffer.buf[req_buffer.out];
        req_buffer.out = (req_buffer.out + 1) % BUFF_SIZE;
        pthread_mutex_unlock(&req_buffer.mutex);
        
        sem_post(req_buffer.sem_empty);

        // --- VERIFICAÇÃO DE DUPLICADOS ---
        int is_duplicate = 0;
        pthread_mutex_lock(&active_players_lock);
        for (int i = 0; i < max_sessions; i++) {
            if (active_player_names[i][0] != '\0' && strncmp(active_player_names[i], req.req_pipe, 40) == 0) {
                is_duplicate = 1;
                break;
            }
        }

        if (is_duplicate) {
            pthread_mutex_unlock(&active_players_lock);
            printf("Rejeitado cliente duplicado: %s\n", req.req_pipe);
            // Abrir e fechar pipes para desbloquear o cliente (que falhará a seguir)
            int fd1 = open(req.req_pipe, O_RDWR);
            int fd2 = open(req.notif_pipe, O_RDWR);
            if (fd1 != -1) close(fd1);
            if (fd2 != -1) close(fd2);
            continue;
        }

        // Registar cliente neste slot (podemos usar o slot_id desta thread ou procurar livre)
        // Como o slot_id é fixo por thread e a thread só corre um jogo de cada vez, usamos active_player_names[slot_id]
        strncpy(active_player_names[slot_id], req.req_pipe, 40);
        pthread_mutex_unlock(&active_players_lock);
        // ---------------------------------

        int req_fd = open(req.req_pipe, O_RDWR);
        int notif_fd = open(req.notif_pipe, O_RDWR);

        if (req_fd == -1 || notif_fd == -1) {
            if (req_fd != -1) close(req_fd);
            if (notif_fd != -1) close(notif_fd);
        } else {
            run_game_session(req_fd, notif_fd, req.level_dir, slot_id, active_boards, &boards_lock);
            close(req_fd);
            close(notif_fd);
        }

        // Limpar registo do cliente
        pthread_mutex_lock(&active_players_lock);
        memset(active_player_names[slot_id], 0, 40);
        pthread_mutex_unlock(&active_players_lock);
        
        printf("Sessão no slot %d terminou.\n", slot_id);
    }
    return NULL;
}

int main(int argc, char* argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <levels_dir> <max_games> <register_pipe>\n", argv[0]);
        return 1;
    }

    char* level_dir = argv[1];
    max_sessions = atoi(argv[2]);
    char* register_pipe_name = argv[3];

    active_boards = calloc(max_sessions, sizeof(board_t*));
    
    // NOVO: Inicializar array de nomes ativos
    active_player_names = calloc(max_sessions, sizeof(char*));
    for(int i = 0; i < max_sessions; i++) {
        active_player_names[i] = calloc(1, 40);
    }
    
    req_buffer.in = 0; req_buffer.out = 0;
    pthread_mutex_init(&req_buffer.mutex, NULL);
    
    snprintf(sem_empty_name, sizeof(sem_empty_name), "/sem_empty_%d", getpid());
    snprintf(sem_full_name, sizeof(sem_full_name), "/sem_full_%d", getpid());

    sem_unlink(sem_empty_name);
    sem_unlink(sem_full_name);

    req_buffer.sem_empty = sem_open(sem_empty_name, O_CREAT, 0644, BUFF_SIZE);
    req_buffer.sem_full  = sem_open(sem_full_name, O_CREAT, 0644, 0);

    if (req_buffer.sem_empty == SEM_FAILED || req_buffer.sem_full == SEM_FAILED) {
        perror("Erro ao criar semáforos");
        return 1;
    }

    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sa.sa_flags = 0; 
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    pthread_t *workers = malloc(sizeof(pthread_t) * max_sessions);
    for (int i = 0; i < max_sessions; i++) {
        int *id = malloc(sizeof(int));
        *id = i;
        pthread_create(&workers[i], NULL, worker_thread, id);
    }

    unlink(register_pipe_name);
    if (mkfifo(register_pipe_name, 0666) == -1) { perror("FIFO"); return 1; }
    int reg_fd = open(register_pipe_name, O_RDWR);
    if (reg_fd == -1) return 1;

    printf("Servidor (PID %d) pronto. Threads: %d\n", getpid(), max_sessions);

    while (1) {
        if (print_stats_request) {
            log_active_games();
            print_stats_request = 0;
        }

        char buffer[81]; 
        ssize_t n = read(reg_fd, buffer, 81);
        
        if (n == -1 && errno == EINTR) continue;
        
        if (n > 0 && buffer[0] == OP_CODE_CONNECT) {
            session_request_t new_req;
            memcpy(new_req.req_pipe, buffer + 1, 40);
            memcpy(new_req.notif_pipe, buffer + 1 + 40, 40);
            strncpy(new_req.level_dir, level_dir, 256);

            while (sem_wait(req_buffer.sem_empty) == -1) {
                if (errno == EINTR) {
                    if (print_stats_request) { log_active_games(); print_stats_request = 0; }
                    continue;
                }
            }

            pthread_mutex_lock(&req_buffer.mutex);
            req_buffer.buf[req_buffer.in] = new_req;
            req_buffer.in = (req_buffer.in + 1) % BUFF_SIZE;
            pthread_mutex_unlock(&req_buffer.mutex);
            
            sem_post(req_buffer.sem_full);
        }
    }

    close(reg_fd);
    unlink(register_pipe_name);
    sem_close(req_buffer.sem_empty);
    sem_close(req_buffer.sem_full);
    sem_unlink(sem_empty_name);
    sem_unlink(sem_full_name);
    
    return 0;
}