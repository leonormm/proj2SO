#include "api.h"
#include "protocol.h"
#include "display.h"
#include "debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>

bool stop_execution = false;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static void *receiver_thread(void *arg) {
    (void)arg;
    while (true) {
        Board b = receive_board_update();
        if (!b.data) {
            pthread_mutex_lock(&mutex);
            stop_execution = true;
            pthread_mutex_unlock(&mutex);
            break;
        }
        
        draw_board_client(b);
        
        if (b.game_over == 1 || b.victory == 1) {
            pthread_mutex_lock(&mutex);
            stop_execution = true;
            pthread_mutex_unlock(&mutex);
            // Espera 3 segundos para veres o resultado final
            sleep(3);
            free(b.data);
            break;
        }
        free(b.data);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 3) return 1;
    const char *client_id = argv[1];
    const char *reg_pipe = argv[2];
    FILE *cmd_fp = (argc == 4) ? fopen(argv[3], "r") : NULL;
    char req_p[40], not_p[40];
    snprintf(req_p, 40, "/tmp/%s_req", client_id);
    snprintf(not_p, 40, "/tmp/%s_not", client_id);

    if (pacman_connect(req_p, not_p, reg_pipe) != 0) return 1;

    pthread_t r_tid;
    pthread_create(&r_tid, NULL, receiver_thread, NULL);

    terminal_init();

    while (1) {
        pthread_mutex_lock(&mutex);
        if (stop_execution) { pthread_mutex_unlock(&mutex); break; }
        pthread_mutex_unlock(&mutex);

        char cmd = '\0';
        if (cmd_fp) {
            int ch = fgetc(cmd_fp);
            if (ch == EOF) { rewind(cmd_fp); continue; }
            cmd = toupper((char)ch);
            if (cmd == '\n' || cmd == '\r') continue;
            sleep_ms(50);
        } else {
            cmd = get_input(); // Agora é rápido devido ao timeout(20)
        }

        if (cmd == 'Q') break;
        if (cmd != '\0') pacman_play(cmd);
    }

    pacman_disconnect();
    pthread_join(r_tid, NULL);
    if (cmd_fp) fclose(cmd_fp);
    terminal_cleanup();
    return 0;
}