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
#define BUFF_SIZE 10 // Tamanho do buffer de pedidos pendentes

typedef struct {
    char req_pipe[40];
    char notif_pipe[40];
    char level_dir[256];
} session_request_t;

typedef struct {
    session_request_t buf[BUFF_SIZE];
    int in;
    int out;
    sem_t *sem_full;   // ALTERADO: Ponteiro para named semaphore (macOS fix)
    sem_t *sem_empty;  // ALTERADO: Ponteiro para named semaphore (macOS fix)
    pthread_mutex_t mutex;
} request_buffer_t;

request_buffer_t req_buffer;

// --- GLOBAIS ---
board_t **active_boards;
int max_sessions = 0;
pthread_mutex_t boards_lock = PTHREAD_MUTEX_INITIALIZER;
volatile sig_atomic_t print_stats_request = 0;

// Nomes dos semáforos para limpeza
char sem_full_name[64];
char sem_empty_name[64];

// Função declarada no game.c
int run_game_session(int req_fd, int notif_fd, char* level_dir, int slot_id, board_t **registry, pthread_mutex_t *registry_lock);

// Handler do sinal SIGUSR1
void handle_signal(int sig) {
    if (sig == SIGUSR1) {
        print_stats_request = 1;
    }
}

void log_active_games() {
    FILE *f = fopen("server_log.txt", "w");
    if (!f) return;

    pthread_mutex_lock(&boards_lock);
    fprintf(f, "=== PACMANIST SERVER LOG (PID %d) ===\n", getpid());
    int active_count = 0;
    
    for (int i = 0; i < max_sessions; i++) {
        // Marcador 0x1 significa ocupado mas sem board pronto, NULL é livre
        if (active_boards[i] != NULL && active_boards[i] != (board_t*)0x1) {
            board_t *b = active_boards[i];
            fprintf(f, "-- Sessão Slot %d --\n", i);
            fprintf(f, "   Nível: %s | Dim: %dx%d\n", b->level_name, b->width, b->height);
            if (b->n_pacmans > 0 && b->pacmans) {
                fprintf(f, "   Pacman: (%d, %d) | Pontos: %d\n", 
                        b->pacmans[0].pos_x, b->pacmans[0].pos_y, b->pacmans[0].points);
            }
            active_count++;
        }
    }
    if (active_count == 0) fprintf(f, "Nenhum jogo ativo.\n");
    pthread_mutex_unlock(&boards_lock);
    fclose(f);
}

// --- TAREFA TRABALHADORA (WORKER) ---
void* worker_thread(void* arg) {
    int slot_id = *(int*)arg;
    free(arg);

    // EX2: Bloquear SIGUSR1 nesta thread
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    int s = pthread_sigmask(SIG_BLOCK, &set, NULL);
    if (s != 0) fprintf(stderr, "Erro ao mascarar sinal na thread %d\n", slot_id);

    while (1) {
        // Consumir pedido do buffer
        session_request_t req;

        // ALTERADO: Removeu-se o '&' pois já é ponteiro
        sem_wait(req_buffer.sem_full); 
        
        pthread_mutex_lock(&req_buffer.mutex);
        req = req_buffer.buf[req_buffer.out];
        req_buffer.out = (req_buffer.out + 1) % BUFF_SIZE;
        pthread_mutex_unlock(&req_buffer.mutex);
        
        // ALTERADO: Removeu-se o '&' pois já é ponteiro
        sem_post(req_buffer.sem_empty);

        // Abrir pipes
        int req_fd = open(req.req_pipe, O_RDWR);
        int notif_fd = open(req.notif_pipe, O_RDWR);

        if (req_fd == -1 || notif_fd == -1) {
            if (req_fd != -1) close(req_fd);
            if (notif_fd != -1) close(notif_fd);
            continue;
        }

        // Executar sessão (Slot fixo para esta thread)
        run_game_session(req_fd, notif_fd, req.level_dir, slot_id, active_boards, &boards_lock);

        close(req_fd);
        close(notif_fd);
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

    // Inicialização
    active_boards = calloc(max_sessions, sizeof(board_t*));
    
    // Init Buffer
    req_buffer.in = 0; req_buffer.out = 0;
    pthread_mutex_init(&req_buffer.mutex, NULL);
    
    // ALTERADO: Inicialização de Semáforos com sem_open para macOS
    snprintf(sem_empty_name, sizeof(sem_empty_name), "/sem_empty_%d", getpid());
    snprintf(sem_full_name, sizeof(sem_full_name), "/sem_full_%d", getpid());

    // Unlink preventivo caso tenha sobrado de execução anterior
    sem_unlink(sem_empty_name);
    sem_unlink(sem_full_name);

    req_buffer.sem_empty = sem_open(sem_empty_name, O_CREAT, 0644, BUFF_SIZE);
    req_buffer.sem_full  = sem_open(sem_full_name, O_CREAT, 0644, 0);

    if (req_buffer.sem_empty == SEM_FAILED || req_buffer.sem_full == SEM_FAILED) {
        perror("Erro ao criar semáforos");
        return 1;
    }

    // Configurar Sinal
    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sa.sa_flags = 0; 
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    // Criar Pool de Threads
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
            // Preparar pedido
            session_request_t new_req;
            memcpy(new_req.req_pipe, buffer + 1, 40);
            memcpy(new_req.notif_pipe, buffer + 1 + 40, 40);
            strncpy(new_req.level_dir, level_dir, 256);

            // Produtor: Inserir no buffer
            
            // ALTERADO: Removeu-se o '&'
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
            
            // ALTERADO: Removeu-se o '&'
            sem_post(req_buffer.sem_full);
        }
    }

    // Limpeza (inalcançável no loop infinito, mas boa prática)
    close(reg_fd);
    unlink(register_pipe_name);
    sem_close(req_buffer.sem_empty);
    sem_close(req_buffer.sem_full);
    sem_unlink(sem_empty_name);
    sem_unlink(sem_full_name);
    
    return 0;
}