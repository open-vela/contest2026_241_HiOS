/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * This file contains code derived from MimiClaw (https://github.com/memovai/mimiclaw)
 * Copyright (c) 2026 Ziboyan Wang, licensed under the MIT License.
 * See NOTICE file for the original MIT License terms.
 */

#include "core/session_mgr.h"
#include "agent_config.h"
#include "agent_compat.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <time.h>
#include "cJSON.h"

static const char *TAG = "session";
static pthread_mutex_t s_session_lock = PTHREAD_MUTEX_INITIALIZER;

static void sanitize_filename(const char *src, char *dst, size_t dst_size)
{
    size_t i;

    for (i = 0; i < dst_size - 1 && src[i] != '\0'; i++) {
        unsigned char c = (unsigned char)src[i];
        /* Strict allowlist: only alphanumeric, hyphen, underscore, dot */
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            dst[i] = (char)c;
        } else {
            dst[i] = '_';
        }
    }
    dst[i] = '\0';
}

static void session_path(const char *chat_id, char *buf, size_t size)
{
    char safe_id[64];

    sanitize_filename(chat_id, safe_id, sizeof(safe_id));
    snprintf(buf, size, "%s/tg_%s.jsonl", AGENT_SESSION_DIR, safe_id);
}

int session_mgr_init(void)
{
    syslog(LOG_INFO, "[%s] Session manager initialized at %s\n", TAG, AGENT_SESSION_DIR);
    return OK;
}

/**
 * Truncate session file to keep only the last max_lines entries.
 * Uses a temp file + rename for atomicity.
 */
static void session_truncate(const char *chat_id, int max_lines)
{
    char path[128];
    session_path(chat_id, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;
    char *line = NULL;
    size_t cap = 0;
    int total = 0;
    while (getline(&line, &cap, f) >= 0) {
        if (line[0] && line[0] != '\n') ++total;
    }
    if (total <= max_lines) { free(line); fclose(f); return; }
    rewind(f);
    int skip = total - max_lines;
    while (skip > 0 && getline(&line, &cap, f) >= 0) {
        if (line[0] && line[0] != '\n') --skip;
    }
    char temporary[140];
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    FILE *tmp = fopen(temporary, "w");
    bool ok = tmp != NULL;
    if (tmp) {
        while (getline(&line, &cap, f) >= 0) {
            if (fputs(line, tmp) < 0) { ok = false; break; }
        }
        if (ferror(f) || fflush(tmp) != 0) ok = false;
        if (fclose(tmp) != 0) ok = false;
    }
    free(line);
    fclose(f);
    if (ok && rename(temporary, path) == 0) return;
    remove(temporary);
}

int session_append(const char *chat_id, const char *role, const char *content)
{
    pthread_mutex_lock(&s_session_lock);

    char path[128];
    session_path(chat_id, path, sizeof(path));

    FILE *f = fopen(path, "a");
    if (!f) {
        syslog(LOG_ERR, "[%s] Cannot open session file %s\n", TAG, path);
        pthread_mutex_unlock(&s_session_lock);
        return ERROR;
    }

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "role", role);
    cJSON_AddStringToObject(obj, "content", content);
    cJSON_AddNumberToObject(obj, "ts", (double)time(NULL));

    char *line = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    if (line) {
        fprintf(f, "%s\n", line);
        free(line);
    }

    fclose(f);

    /* Auto-truncate: check periodically to avoid overhead on every append.
     * Truncate when file exceeds 2x the max to amortize the cost. */
    {
        static int s_append_count = 0;
        if (++s_append_count >= 10) {
            s_append_count = 0;
            session_truncate(chat_id, AGENT_SESSION_MAX_MSGS * 2);
        }
    }

    pthread_mutex_unlock(&s_session_lock);
    return OK;
}

int session_get_history_json(const char *chat_id, char *buf, size_t size, int max_msgs)
{
    if (!buf || size < 3 || max_msgs < 1) return ERROR;
    pthread_mutex_lock(&s_session_lock);

    char path[128];
    session_path(chat_id, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(buf, size, "[]");
        pthread_mutex_unlock(&s_session_lock);
        return OK;
    }

    /* VLA alternative: static ring buffer (max_msgs is bounded by caller) */
    if (max_msgs > AGENT_SESSION_MAX_MSGS) max_msgs = AGENT_SESSION_MAX_MSGS;

    cJSON *messages[AGENT_SESSION_MAX_MSGS];
    int count = 0;
    int write_idx = 0;

    char *line = NULL;
    size_t line_cap = 0;
    while (getline(&line, &line_cap, f) >= 0) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (line[0] == '\0') continue;

        cJSON *obj = cJSON_Parse(line);
        if (!obj) continue;

        if (count >= max_msgs) {
            cJSON_Delete(messages[write_idx]);
        }
        messages[write_idx] = obj;
        write_idx = (write_idx + 1) % max_msgs;
        if (count < max_msgs) count++;
    }
    free(line);
    fclose(f);

    cJSON *arr = cJSON_CreateArray();
    int start = (count < max_msgs) ? 0 : write_idx;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % max_msgs;
        cJSON *src = messages[idx];

        cJSON *entry = cJSON_CreateObject();
        cJSON *role    = cJSON_GetObjectItem(src, "role");
        cJSON *content = cJSON_GetObjectItem(src, "content");
        if (cJSON_IsString(role) && cJSON_IsString(content)) {
            cJSON_AddStringToObject(entry, "role",    role->valuestring);
            cJSON_AddStringToObject(entry, "content", content->valuestring);
        }
        cJSON_AddItemToArray(arr, entry);
    }

    /* Cleanup ring buffer */
    int cleanup_start = (count < max_msgs) ? 0 : write_idx;
    for (int i = 0; i < count; i++) {
        cJSON_Delete(messages[(cleanup_start + i) % max_msgs]);
    }

    /* Return valid JSON even when long messages exceed the caller's
     * context buffer: trim oldest turns, then shorten the last content. */
    char *json_str = cJSON_PrintUnformatted(arr);
    while (json_str && strlen(json_str) >= size) {
        free(json_str);
        int entries = cJSON_GetArraySize(arr);
        if (entries > 1) cJSON_DeleteItemFromArray(arr, 0);
        else if (entries == 1) {
            cJSON *entry = cJSON_GetArrayItem(arr, 0);
            cJSON *content = cJSON_GetObjectItem(entry, "content");
            if (cJSON_IsString(content) && strlen(content->valuestring) > 8) {
                size_t keep = strlen(content->valuestring) / 2;
                while (keep && ((unsigned char)content->valuestring[keep] & 0xc0) == 0x80) --keep;
                content->valuestring[keep] = 0;
            } else cJSON_DeleteItemFromArray(arr, 0);
        } else break;
        json_str = cJSON_PrintUnformatted(arr);
    }
    if (json_str) { snprintf(buf, size, "%s", json_str); free(json_str); }
    else snprintf(buf, size, "[]");
    cJSON_Delete(arr);

    pthread_mutex_unlock(&s_session_lock);
    return OK;
}

int session_clear(const char *chat_id)
{
    char path[128];
    session_path(chat_id, path, sizeof(path));

    if (remove(path) == 0) {
        syslog(LOG_INFO, "[%s] Session %s cleared\n", TAG, chat_id);
        return OK;
    }
    return ERROR;
}

int session_clear_all(void)
{
    DIR *dir = opendir(AGENT_SESSION_DIR);
    if (!dir) {
        syslog(LOG_WARNING, "[%s] Cannot open session directory %s\n", TAG, AGENT_SESSION_DIR);
        return ERROR;
    }

    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, "tg_") && strstr(entry->d_name, ".jsonl")) {
            char path[256];
            snprintf(path, sizeof(path), "%s/%s", AGENT_SESSION_DIR, entry->d_name);
            if (remove(path) == 0) {
                count++;
            }
        }
    }
    closedir(dir);

    syslog(LOG_INFO, "[%s] Cleared all sessions (%d files)\n", TAG, count);
    return OK;
}

void session_list(void)
{
    DIR *dir = opendir(AGENT_SESSION_DIR);
    if (!dir) {
        dir = opendir(AGENT_DATA_DIR);
        if (!dir) {
            syslog(LOG_WARNING, "[%s] Cannot open data directory\n", TAG);
            return;
        }
    }

    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, "tg_") && strstr(entry->d_name, ".jsonl")) {
            syslog(LOG_INFO, "[%s]   Session: %s\n", TAG, entry->d_name);
            count++;
        }
    }
    closedir(dir);

    if (count == 0) {
        syslog(LOG_INFO, "[%s]   No sessions found\n", TAG);
    }
}
