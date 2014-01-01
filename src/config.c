#include "flog.h"
#include "config.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>

/* 配置参数名长度限制，包括结尾的 '\0' */
#define MAX_ARGV_NAME_LEN 32

static int get_config(config *cfg, const char *key, const char *value)
{
    if (!strcmp(key, "server")) {
        strncpy(cfg->server, value, MAX_SERVER_LEN + 1);
        return 1;
    }
    if (!strcmp(key, "server-port")) {
        strncpy(cfg->server_port, value, MAX_PORT_LEN + 1);
        return 2;
    }
    if (!strcmp(key, "local-port")) {
        strncpy(cfg->local_port, value, MAX_PORT_LEN + 1);
        return 3;   
    } 
    if (!strcmp(key, "key")) {
        int i;
        for (i = 0; i < MAX_KEY_LEN; i++) {
            cfg->key[i] = value[i];
        }
        return 4;
    }
    return 0;
}

static int parser_string(config *cfg, const char *str)
{
    char key[MAX_ARGV_NAME_LEN], value[MAX_SERVER_LEN];
    
    while (*str == ' ') str++;
    if (*str == '#' || *str == '\n' || *str == '\0') return -1;

    int i = 0, j = 0;
    while (*str != '=') {
        if (*str == '\n' || *str == '\0') return -1;
        if (*str != ' ') {
            key[i++] = *str;   
        }
        str++;
        if (i > MAX_ARGV_NAME_LEN - 2) break;
    }
    key[i] = '\0';
    
    if (i > MAX_ARGV_NAME_LEN - 2) {
        while (*str != '=') str++;
    }

    while (*str != '\n' && *str != '\0') {
        if (*str != ' ' && *str != '=') {
            value[j++] = *str;
        }
        str++;
        if (j > MAX_SERVER_LEN - 2) break;
    }
    value[j] = '\0';
    int r = get_config(cfg, key, value);
    return r;
}

void load_config_file(config *cfg, const char *path)
{
    char buffer[MAX_ARGV_NAME_LEN + MAX_SERVER_LEN];
    
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        LOG_ERROR("Can't load config file: %s", path);
    }

    while (!feof(f)) {
        if (fgets(buffer, 100, f) == NULL) continue;
        if (buffer[0] == '#' || buffer[0] == '\n') continue;
        parser_string(cfg, buffer);
    }
    fclose(f);
}