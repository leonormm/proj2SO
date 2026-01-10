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
#include <stdint.h>

int req_fd = -1;
int notif_fd = -1;
char my_req_pipe[40];
char my_notif_pipe[40];

// Auxiliary function to read exact number of bytes
int read_exact(int fd, void *buf, size_t count) {
    size_t total_read = 0;
    while (total_read < count) {
        ssize_t n = read(fd, (char*)buf + total_read, count - total_read);
        if (n <= 0) return 0;
        total_read += n;
    }
    return 1;
}

// Connect to the Pacman server
int pacman_connect(const char *req_pipe, const char *notif_pipe, const char *server_pipe) {
    strncpy(my_req_pipe, req_pipe, 40);
    strncpy(my_notif_pipe, notif_pipe, 40);

    // Limpar pipes antigos se existirem
    unlink(req_pipe);
    unlink(notif_pipe);

    if (mkfifo(req_pipe, 0666) == -1) return -1;
    if (mkfifo(notif_pipe, 0666) == -1) return -1;

    // Enviar pedido de conexão
    int s_fd = open(server_pipe, O_WRONLY);
    if (s_fd == -1) {
        return -1;
    }

    char buf[81];
    memset(buf, 0, 81);
    buf[0] = OP_CODE_CONNECT;
    memcpy(buf + 1, req_pipe, 40);
    memcpy(buf + 41, notif_pipe, 40);
    write(s_fd, buf, 81);
    close(s_fd);
    
    req_fd = open(req_pipe, O_WRONLY); 
    notif_fd = open(notif_pipe, O_RDONLY);

    if (req_fd == -1 || notif_fd == -1) return -1;
    return 0;
}

// Disconnect from the Pacman server
int pacman_disconnect() {
    if (req_fd != -1) {
        unsigned char op = OP_CODE_DISCONNECT;
        write(req_fd, &op, 1);
        close(req_fd);
        close(notif_fd);
    }
    unlink(my_req_pipe);
    unlink(my_notif_pipe);
    return 0;
}

// Send a play command to the server
void pacman_play(char command) {
    unsigned char buf[2] = {OP_CODE_PLAY, (unsigned char)command};
    write(req_fd, buf, 2);
}

// Receive a board update from the server
Board receive_board_update() {
    Board b = {0};
    unsigned char header[25];
    
    if (!read_exact(notif_fd, header, 25)) return b;
    if (header[0] != OP_CODE_BOARD) return b;

    int32_t values[6];
    memcpy(values, header + 1, 24);
    
    b.width = (int)values[0];
    b.height = (int)values[1];
    b.tempo = (int)values[2];
    b.victory = (int)values[3];
    b.game_over = (int)values[4];
    b.accumulated_points = (int)values[5];

    int map_size = b.width * b.height;
    if (map_size > 0) {
        b.data = malloc(map_size);
        if (!read_exact(notif_fd, b.data, map_size)) {
            free(b.data);
            b.data = NULL;
        }
    }
    return b;
}