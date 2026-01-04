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
#include "board.h"  // <--- 1. ADICIONAR ESTE INCLUDE

// Estrutura para passar argumentos para a thread do cliente
typedef struct {
    char req_pipe[40];
    char notif_pipe[40];
    char level_dir[256];
} session_args_t;

// Declaração da função que corre o jogo (estará no game.c modificado)
int run_game_session(int req_fd, int notif_fd, char* level_dir);

// Semáforo para controlar o número máximo de sessões (max_games)
sem_t *session_sem;

void* client_thread(void* arg) {
    session_args_t* args = (session_args_t*)arg;
    
    // Abrir pipes do cliente (O_RDWR como pedido)
    int req_fd = open(args->req_pipe, O_RDWR);
    int notif_fd = open(args->notif_pipe, O_RDWR);

    if (req_fd == -1 || notif_fd == -1) {
        perror("Erro ao abrir pipes do cliente");
        free(args);
        sem_post(session_sem);
        return NULL;
    }

    // Enviar confirmação de conexão (OP_CODE=1 | result=0)
    char response[2];
    response[0] = OP_CODE_CONNECT;
    response[1] = 0; // Sucesso
    write(notif_fd, response, 2);

    // Iniciar a lógica do jogo
    run_game_session(req_fd, notif_fd, args->level_dir);

    // Limpeza
    close(req_fd);
    close(notif_fd);
    free(args);

    sem_post(session_sem);
    return NULL;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <levels_dir> <max_games> <register_pipe>\n", argv[0]);
        return 1;
    }

    char* level_dir = argv[1];
    int max_games = atoi(argv[2]);
    char* register_pipe_name = argv[3];

    // --- 2. INICIALIZAR O FICHEIRO DE DEBUG ---
    // Isto evita o SegFault quando o parser tentar fazer logs
    open_debug_file("server_debug.log"); 
    // ------------------------------------------

    signal(SIGPIPE, SIG_IGN);

    // --- INICIALIZAÇÃO DO SEMÁFORO ---
    char sem_name[64];
    snprintf(sem_name, sizeof(sem_name), "/pacman_sem_%d", getpid());

    session_sem = sem_open(sem_name, O_CREAT, 0644, max_games);
    
    if (session_sem == SEM_FAILED) {
        perror("Erro ao criar semáforo (sem_open)");
        return 1;
    }

    sem_unlink(sem_name); 

    if (mkfifo(register_pipe_name, 0666) == -1) {
        // Ignorar erro se já existir
    }

    int reg_fd = open(register_pipe_name, O_RDWR);
    if (reg_fd == -1) {
        perror("Erro ao abrir FIFO de registo");
        sem_close(session_sem);
        return 1;
    }

    printf("Servidor PacmanIST iniciado (PID %d). A aguardar conexões...\n", getpid());

    while (1) {
        char buffer[1 + 40 + 40]; 
        
        ssize_t n = read(reg_fd, buffer, sizeof(buffer));
        
        if (n > 0 && buffer[0] == OP_CODE_CONNECT) {
            sem_wait(session_sem);

            session_args_t* args = malloc(sizeof(session_args_t));
            memcpy(args->req_pipe, buffer + 1, 40);
            memcpy(args->notif_pipe, buffer + 1 + 40, 40);
            strncpy(args->level_dir, level_dir, 256);

            pthread_t tid;
            if (pthread_create(&tid, NULL, client_thread, args) != 0) {
                perror("Erro ao criar thread de cliente");
                free(args);
                sem_post(session_sem);
            } else {
                pthread_detach(tid); 
            }
        }
    }

    close(reg_fd);
    sem_close(session_sem);
    
    // --- 3. FECHAR O FICHEIRO DE DEBUG ---
    close_debug_file();
    
    return 0;
}