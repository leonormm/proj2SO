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

// --- ESTRUTURAS E GLOBAIS PARA EXERCÍCIO 2 ---

// Array global de ponteiros para os tabuleiros ativos
board_t **active_boards;
pthread_mutex_t boards_lock = PTHREAD_MUTEX_INITIALIZER;
int max_sessions = 0;

// Flag para o sinal (volatile para evitar otimizações do compilador)
volatile sig_atomic_t print_stats_request = 0;

// Handler do sinal SIGUSR1
void handle_signal(int sig) {
    if (sig == SIGUSR1) {
        print_stats_request = 1;
    }
}

// Função que escreve o log (Exercício 2)
void log_active_games() {
    FILE *f = fopen("server_log.txt", "w");
    if (!f) {
        perror("Erro ao criar log");
        return;
    }

    pthread_mutex_lock(&boards_lock);
    
    fprintf(f, "=== PACMANIST SERVER LOG (PID %d) ===\n", getpid());
    int active_count = 0;
    
    for (int i = 0; i < max_sessions; i++) {
        if (active_boards[i] != NULL) {
            board_t *b = active_boards[i];
            
            // Garantir leitura segura (usando o rwlock do board se possível, 
            // mas aqui faremos leitura direta rápida para evitar deadlocks complexos)
            
            fprintf(f, "-- Sessão Slot %d --\n", i);
            fprintf(f, "   Nível: %s\n", b->level_name);
            fprintf(f, "   Dimensões: %dx%d\n", b->width, b->height);
            if (b->n_pacmans > 0 && b->pacmans) {
                fprintf(f, "   Pacman: (%d, %d) | Pontos: %d | Vidas: %s\n", 
                        b->pacmans[0].pos_x, 
                        b->pacmans[0].pos_y, 
                        b->pacmans[0].points,
                        b->pacmans[0].alive ? "Vivo" : "Morto");
            }
            fprintf(f, "   Fantasmas: %d\n", b->n_ghosts);
            active_count++;
        }
    }
    
    if (active_count == 0) {
        fprintf(f, "Nenhum jogo ativo no momento.\n");
    }
    
    pthread_mutex_unlock(&boards_lock);
    fclose(f);
    printf("Log gerado em 'server_log.txt' (%d jogos ativos).\n", active_count);
}

// ------------------------------------------------

typedef struct {
    char req_pipe[40];
    char notif_pipe[40];
    char level_dir[256];
    int slot_index; // EX2: Saber qual o slot deste cliente
} session_args_t;

// Declaração atualizada para receber o slot e os globais
int run_game_session(int req_fd, int notif_fd, char* level_dir, int slot_id, board_t **registry, pthread_mutex_t *registry_lock);

sem_t *session_sem;

void* client_thread(void* arg) {
    pthread_detach(pthread_self());

    session_args_t* args = (session_args_t*)arg;
    
    int req_fd = open(args->req_pipe, O_RDWR);
    int notif_fd = open(args->notif_pipe, O_RDWR);

    if (req_fd == -1 || notif_fd == -1) {
        perror("Erro pipes");
        if (req_fd != -1) close(req_fd);
        if (notif_fd != -1) close(notif_fd);
        sem_post(session_sem);
        
        // Libertar slot no erro
        pthread_mutex_lock(&boards_lock);
        active_boards[args->slot_index] = NULL;
        pthread_mutex_unlock(&boards_lock);
        
        free(args);
        return NULL;
    }

    // EX2: Passamos o slot e o registo global para o jogo se registar
    run_game_session(req_fd, notif_fd, args->level_dir, args->slot_index, active_boards, &boards_lock);

    printf("Sessão %d terminada.\n", args->slot_index);
    
    close(req_fd);
    close(notif_fd);
    
    // EX2: Limpeza final garantida
    pthread_mutex_lock(&boards_lock);
    active_boards[args->slot_index] = NULL;
    pthread_mutex_unlock(&boards_lock);

    free(args);
    sem_post(session_sem);
    return NULL;
}

int main(int argc, char* argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <levels_dir> <max_games> <register_pipe>\n", argv[0]);
        return 1;
    }

    char* level_dir = argv[1];
    max_sessions = atoi(argv[2]); // EX2: Guardar max_sessions globalmente
    char* register_pipe_name = argv[3];

    // EX2: Inicializar array de boards
    active_boards = calloc(max_sessions, sizeof(board_t*));

    // EX2: Configurar Signal Handler
    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sa.sa_flags = 0; // Faz com que system calls (como read) falhem com EINTR
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    
    signal(SIGPIPE, SIG_IGN);

    char sem_name[64];
    snprintf(sem_name, 64, "/pacman_sem_%d", getpid());
    session_sem = sem_open(sem_name, O_CREAT, 0644, max_sessions);
    if (session_sem == SEM_FAILED) { perror("Semáforo"); return 1; }
    sem_unlink(sem_name); 

    unlink(register_pipe_name);
    if (mkfifo(register_pipe_name, 0666) == -1) { perror("FIFO"); return 1; }

    int reg_fd = open(register_pipe_name, O_RDWR);
    if (reg_fd == -1) { perror("Open FIFO"); return 1; }

    printf("Servidor (PID %d) pronto. Max Jogos: %d\n", getpid(), max_sessions);
    printf("Para gerar log: kill -SIGUSR1 %d\n", getpid());

    while (1) {
        // EX2: Verificar se houve pedido de log
        if (print_stats_request) {
            log_active_games();
            print_stats_request = 0;
        }

        char buffer[81]; 
        ssize_t n = read(reg_fd, buffer, 81);
        
        // EX2: Se read foi interrompido pelo sinal, repete o loop
        if (n == -1 && errno == EINTR) {
            continue;
        }
        
        if (n > 0 && buffer[0] == OP_CODE_CONNECT) {
            // Tentar reservar vaga no semáforo
            // Nota: Se o servidor receber sinal enquanto espera aqui,
            // sem_wait também retorna -1/EINTR. Devemos tratar isso.
            while (sem_wait(session_sem) == -1) {
                if (errno == EINTR) {
                    if (print_stats_request) {
                        log_active_games();
                        print_stats_request = 0;
                    }
                    continue;
                }
                perror("sem_wait");
                break;
            }

            // Encontrar slot livre no array para EX2
            int slot = -1;
            pthread_mutex_lock(&boards_lock);
            for(int i=0; i<max_sessions; i++) {
                if (active_boards[i] == NULL) {
                    // Marcamos logo como "reservado" (usando um placeholder ou apenas o índice)
                    // Na verdade, client_thread vai por o ponteiro real.
                    // Mas para garantir que não damos o mesmo slot, vamos confiar no semáforo
                    // que garante que há slots livres.
                    // O active_boards[i] só será preenchido dentro do jogo.
                    // Mas precisamos garantir que dois threads não apanham o mesmo i.
                    // O semáforo garante quantidade, não exclusão de índice.
                    // Precisamos de um array de "slots ocupados" ou usar o active_boards.
                    // Vamos usar uma logica simples: client_thread é responsável.
                    // Mas client_thread corre depois. 
                    // Correção: Vamos assumir que active_boards[i] NULL significa livre.
                    // Mas o client_thread demora a arrancar.
                    // Solução rápida: passar apenas o índice e deixar o client_thread gerir.
                    // Como não estamos a marcar "ocupado" aqui, pode haver race condition nos slots?
                    // Sim. Vamos corrigir isso marcando com um valor dummy temporário.
                    active_boards[i] = (board_t*)0x1; // Marcador temporário "Ocupado"
                    slot = i;
                    break;
                }
            }
            pthread_mutex_unlock(&boards_lock);

            if (slot == -1) {
                // Não devia acontecer se o semáforo funciona
                sem_post(session_sem);
                continue;
            }

            session_args_t* args = malloc(sizeof(session_args_t));
            memcpy(args->req_pipe, buffer + 1, 40);
            memcpy(args->notif_pipe, buffer + 1 + 40, 40);
            strncpy(args->level_dir, level_dir, 256);
            args->slot_index = slot;

            pthread_t tid;
            if (pthread_create(&tid, NULL, client_thread, args) != 0) {
                perror("Thread create");
                
                pthread_mutex_lock(&boards_lock);
                active_boards[slot] = NULL;
                pthread_mutex_unlock(&boards_lock);
                
                free(args);
                sem_post(session_sem);
            }
        }
    }

    close(reg_fd);
    unlink(register_pipe_name);
    sem_close(session_sem);
    return 0;
}