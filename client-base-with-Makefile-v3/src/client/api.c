#include "api.h"
#include "protocol.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

int req_fd = -1;
int notif_fd = -1;
char my_req_pipe[40];
char my_notif_pipe[40];

// Função auxiliar para garantir leitura completa de N bytes
// Retorna 1 em sucesso, 0 em EOF/Erro
int read_exact(int fd, void *buf, size_t count) {
    size_t total_read = 0;
    while (total_read < count) {
        ssize_t n = read(fd, (char*)buf + total_read, count - total_read);
        if (n <= 0) return 0; // Erro ou pipe fechado
        total_read += n;
    }
    return 1;
}

int pacman_connect(const char *req_pipe, const char *notif_pipe, const char *server_pipe) {
    strncpy(my_req_pipe, req_pipe, 40);
    strncpy(my_notif_pipe, notif_pipe, 40);

    unlink(req_pipe);
    unlink(notif_pipe);

    if (mkfifo(req_pipe, 0666) == -1) {
        perror("Erro ao criar pipe de request");
        return -1;
    }
    if (mkfifo(notif_pipe, 0666) == -1) {
        perror("Erro ao criar pipe de notification");
        unlink(req_pipe);
        return -1;
    }

    char buffer[1 + 40 + 40];
    buffer[0] = OP_CODE_CONNECT;
    memset(buffer + 1, 0, 80); 
    strncpy(buffer + 1, req_pipe, 40);
    strncpy(buffer + 1 + 40, notif_pipe, 40);

    int server_fd = open(server_pipe, O_WRONLY);
    if (server_fd == -1) {
        perror("Erro ao abrir pipe de registo do servidor");
        return -1;
    }

    write(server_fd, buffer, sizeof(buffer));
    close(server_fd);

    notif_fd = open(notif_pipe, O_RDONLY);
    if (notif_fd == -1) return -1;

    req_fd = open(req_pipe, O_WRONLY);
    if (req_fd == -1) return -1;

    char response[2];
    if (!read_exact(notif_fd, response, 2)) return -1;

    if (response[0] == OP_CODE_CONNECT && response[1] == 0) {
        return 0;
    } else {
        return -1;
    }
}

int pacman_disconnect() {
    if (req_fd != -1) {
        char buffer[2] = {OP_CODE_DISCONNECT, 0};
        write(req_fd, buffer, 2);
        close(req_fd);
        close(notif_fd);
    }
    unlink(my_req_pipe);
    unlink(my_notif_pipe);
    return 0;
}

void pacman_play(char command) {
    if (req_fd == -1) return;
    char buffer[2] = {OP_CODE_PLAY, command};
    if (write(req_fd, buffer, 2) == -1) {
        perror("Erro ao enviar jogada");
    }
}

Board receive_board_update() {
    Board b = {0};
    
    // Header fixo de 25 bytes: [OP(1)] + 6 * [INT(4)]
    char header[25];
    
    // 1. Ler o header completo
    if (!read_exact(notif_fd, header, 25)) {
        b.game_over = 1; 
        return b;
    }

    if (header[0] != OP_CODE_BOARD) {
        b.game_over = 1;
        return b;
    }

    // 2. Extrair inteiros usando memcpy para evitar erros de alinhamento
    // O header começa no índice 1
    int values[6];
    memcpy(values, header + 1, 6 * sizeof(int));

    b.width = values[0];
    b.height = values[1];
    b.tempo = values[2];
    b.victory = values[3]; // Usado agora
    b.game_over = values[4];
    b.accumulated_points = values[5];

    // Se as dimensões forem inválidas, sair
    if (b.width <= 0 || b.height <= 0) {
        b.game_over = 1;
        return b;
    }

    // 3. Alocar e Ler dados do tabuleiro
    b.data = malloc(b.width * b.height);
    if (!b.data) {
        b.game_over = 1;
        return b;
    }

    if (!read_exact(notif_fd, b.data, b.width * b.height)) {
        free(b.data);
        b.data = NULL;
        b.game_over = 1;
        return b;
    }

    return b;
}