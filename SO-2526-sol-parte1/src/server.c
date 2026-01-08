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
#include "protocol.h"
#include "board.h"

// Estrutura para passar argumentos para a thread do cliente
typedef struct {
    char req_pipe[40];
    char notif_pipe[40];
    char level_dir[256];
} session_args_t;

// Declaração da função que corre o jogo (está no game.c)
int run_game_session(int req_fd, int notif_fd, char* level_dir);

// Semáforo para controlar o número máximo de sessões (max_games)
sem_t *session_sem;

void* client_thread(void* arg) {
    // 1. Detach para libertar recursos automaticamente ao terminar
    pthread_detach(pthread_self());

    session_args_t* args = (session_args_t*)arg;
    
    // Abrir pipes do cliente
    int req_fd = open(args->req_pipe, O_RDWR);
    int notif_fd = open(args->notif_pipe, O_RDWR);

    if (req_fd == -1 || notif_fd == -1) {
        perror("Erro ao abrir pipes do cliente");
        if (req_fd != -1) close(req_fd);
        if (notif_fd != -1) close(notif_fd);
        
        // Se falhou logo no início, devolve a vaga
        sem_post(session_sem);
        free(args);
        return NULL;
    }

    // 2. CORRER O JOGO (Bloqueia aqui até o jogo acabar)
    run_game_session(req_fd, notif_fd, args->level_dir);

    // 3. LIMPEZA FINAL (Crucial para permitir novos jogos)
    printf("Sessão terminada. A libertar vaga...\n");
    
    close(req_fd);
    close(notif_fd);
    free(args);

    // 4. IMPORTANTE: Libertar a vaga no semáforo
    sem_post(session_sem);
    
    return NULL;
}

int main(int argc, char* argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <levels_dir> <max_games> <register_pipe>\n", argv[0]);
        return 1;
    }

    char* level_dir = argv[1];
    int max_games = atoi(argv[2]);
    char* register_pipe_name = argv[3];

    // Ignorar SIGPIPE para evitar crashes se o cliente fechar abruptamente
    signal(SIGPIPE, SIG_IGN);

    // Criar semáforo
    char sem_name[64];
    snprintf(sem_name, 64, "/pacman_sem_%d", getpid());
    session_sem = sem_open(sem_name, O_CREAT, 0644, max_games);
    if (session_sem == SEM_FAILED) {
        perror("Erro ao criar semáforo");
        return 1;
    }
    sem_unlink(sem_name); 

    // Criar pipe de registo
    unlink(register_pipe_name);
    if (mkfifo(register_pipe_name, 0666) == -1) {
        perror("Erro ao criar pipe de registo");
        return 1;
    }

    int reg_fd = open(register_pipe_name, O_RDWR);
    if (reg_fd == -1) {
        perror("Erro ao abrir pipe de registo");
        return 1;
    }

    printf("Servidor PacmanIST (PID %d) pronto.\n", getpid());
    printf("Diretoria: %s | Max Jogos: %d\n", level_dir, max_games);

    while (1) {
        char buffer[81]; 
        
        // Ler pedido de conexão
        ssize_t n = read(reg_fd, buffer, 81);
        
        if (n > 0 && buffer[0] == OP_CODE_CONNECT) {
            // Tentar obter vaga. Se max_games for atingido, 
            // o servidor BLOQUEIA AQUI, fazendo o próximo cliente esperar no 'write'
            sem_wait(session_sem);

            session_args_t* args = malloc(sizeof(session_args_t));
            memcpy(args->req_pipe, buffer + 1, 40);
            memcpy(args->notif_pipe, buffer + 1 + 40, 40);
            strncpy(args->level_dir, level_dir, 256);

            pthread_t tid;
            if (pthread_create(&tid, NULL, client_thread, args) != 0) {
                perror("Erro ao criar thread");
                free(args);
                sem_post(session_sem); // Devolve vaga se falhar a criar thread
            }
        }
    }

    close(reg_fd);
    unlink(register_pipe_name);
    sem_close(session_sem);
    return 0;
}