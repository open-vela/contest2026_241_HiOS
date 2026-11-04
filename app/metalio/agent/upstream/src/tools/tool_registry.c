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

#include "tools/tool_registry.h"
#ifdef CONFIG_AI_AGENT_MCP
#include "tools/mcp_client.h"
#include "tools/mcp_bridge.h"
#endif
#include "tools/tool_guard.h"
#include "tools/tool_control.h"
#include "tools/tool_cron.h"
#include "tools/tool_feishu_chat.h"
#include "tools/tool_feishu_doc.h"
#include "tools/tool_fetch_url.h"
#include "tools/tool_files.h"
#include "tools/tool_get_time.h"
#include "tools/tool_health.h"
#include "tools/tool_media.h"
#include "tools/tool_proxyquickapp.h"
#include "tools/tool_shell.h"
#include "tools/tool_system.h"
#include "tools/tool_vision.h"
#include "tools/tool_camera.h"
#include "tools/tool_web_search.h"
#include "agent_compat.h"
#include "agent_config.h"

#include "cJSON.h"
#include <pthread.h>
#include <string.h>

static const char* TAG = "tools";

#define MAX_TOOLS 48
#define MAX_PROVIDERS 4

/* ── External tool provider registry ───────────────────────── */

typedef struct {
    const char* name;
    tool_provider_fn get_tools;
    tool_executor_fn execute;
} tool_provider_t;

static tool_provider_t s_providers[MAX_PROVIDERS];
static int s_provider_count = 0;

static agent_tool_t s_tools[MAX_TOOLS];
static int s_tool_count;
static char* s_tools_json;
static bool s_tools_dirty = true;
static pthread_mutex_t s_tools_mtx = PTHREAD_MUTEX_INITIALIZER;

void tool_registry_register_provider(const char* name,
                                     tool_provider_fn get_tools,
                                     tool_executor_fn execute)
{
    pthread_mutex_lock(&s_tools_mtx);
    if (s_provider_count >= MAX_PROVIDERS) {
        syslog(LOG_ERR, "[%s] Provider registry full, cannot add '%s'\n",
               TAG, name ? name : "(null)");
        pthread_mutex_unlock(&s_tools_mtx);
        return;
    }
    s_providers[s_provider_count].name = name;
    s_providers[s_provider_count].get_tools = get_tools;
    s_providers[s_provider_count].execute = execute;
    s_provider_count++;
    s_tools_dirty = true;
    pthread_mutex_unlock(&s_tools_mtx);
    syslog(LOG_INFO, "[%s] Registered tool provider: %s\n", TAG, name);
}

static void register_tool(const agent_tool_t* tool)
{
    if (s_tool_count >= MAX_TOOLS) {
        syslog(LOG_ERR, "[%s] Tool registry full\n", TAG);
        return;
    }
    s_tools[s_tool_count++] = *tool;
    syslog(LOG_INFO, "[%s] Registered tool: %s\n", TAG, tool->name);
}

/* Rebuild tools JSON under lock. Only rebuilds when dirty flag is set. */
static void build_tools_json_locked(void)
{
    if (!s_tools_dirty) {
        return;
    }

    cJSON* arr = cJSON_CreateArray();

    for (int i = 0; i < s_tool_count; i++) {
        cJSON* tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", s_tools[i].name);
        cJSON_AddStringToObject(tool, "description", s_tools[i].description);
        cJSON* schema = cJSON_Parse(s_tools[i].input_schema_json);
        if (schema)
            cJSON_AddItemToObject(tool, "input_schema", schema);
        cJSON_AddItemToArray(arr, tool);
    }

#ifdef CONFIG_AI_AGENT_MCP
    char* mcp_json = mcp_bridge_get_tools_json();
    if (mcp_json) {
        cJSON* mcp_arr = cJSON_Parse(mcp_json);
        if (mcp_arr && cJSON_IsArray(mcp_arr)) {
            cJSON* item = NULL;
            cJSON_ArrayForEach(item, mcp_arr)
            {
                cJSON_AddItemToArray(arr, cJSON_Duplicate(item, 1));
            }
        }
        cJSON_Delete(mcp_arr);
        free(mcp_json);
    }
#endif

    /* Collect tools from all registered providers */
    for (int p = 0; p < s_provider_count; p++) {
        if (!s_providers[p].get_tools) {
            continue;
        }
        char* provider_json = s_providers[p].get_tools();
        if (provider_json) {
            cJSON* provider_arr = cJSON_Parse(provider_json);
            if (provider_arr && cJSON_IsArray(provider_arr)) {
                cJSON* item = NULL;
                cJSON_ArrayForEach(item, provider_arr)
                {
                    cJSON_AddItemToArray(arr, cJSON_Duplicate(item, 1));
                }
            }
            cJSON_Delete(provider_arr);
            free(provider_json);
        }
    }

    free(s_tools_json);
    s_tools_json = cJSON_PrintUnformatted(arr);
    s_tools_dirty = false;
    cJSON_Delete(arr);
    syslog(LOG_INFO, "[%s] Tools JSON built (%d builtin + %d providers)\n",
        TAG, s_tool_count, s_provider_count);
}

void tool_registry_rebuild_json(void) { build_tools_json_locked(); }

int tool_registry_init(void)
{
    s_tool_count = 0;
    REGISTER_TOOL_NO_PARAMS("get_current_time", "Get current date and time.", tool_get_time_execute);
    REGISTER_TOOL("read_file", "Read an Agent data file.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("path", "Absolute path under Agent data directory") TOOL_SCHEMA_END_REQUIRED("\"path\""), tool_read_file_execute);
    REGISTER_TOOL("write_file", "Write an Agent data file.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("path", "Absolute path under Agent data directory") "," TOOL_PARAM_STR("content", "File content") TOOL_SCHEMA_END_REQUIRED("\"path\",\"content\""), tool_write_file_execute);
    build_tools_json_locked();
    return OK;
}

char* tool_registry_get_tools_json(void)
{
    char* copy;

    pthread_mutex_lock(&s_tools_mtx);
    build_tools_json_locked();
    copy = s_tools_json ? strdup(s_tools_json) : NULL;
    pthread_mutex_unlock(&s_tools_mtx);
    return copy;
}

void tool_registry_invalidate(void)
{
    pthread_mutex_lock(&s_tools_mtx);
    s_tools_dirty = true;
    pthread_mutex_unlock(&s_tools_mtx);
}

void tool_registry_cleanup(void)
{
    pthread_mutex_lock(&s_tools_mtx);
    free(s_tools_json);
    s_tools_json = NULL;
    s_tool_count = 0;
    pthread_mutex_unlock(&s_tools_mtx);
}

int tool_registry_execute(const char *name, const char *input_json,
    char *output, size_t output_size)
{
    /* Security guard check before any tool execution */
    size_t input_len = input_json ? strlen(input_json) : 0;
    tool_guard_result_t guard = tool_guard_check(name, input_json, input_len);

    if (guard != GUARD_ALLOW) {
        const char *reason = "unknown";
        switch (guard) {
        case GUARD_DENY_DISABLED:
            reason = "tool disabled by config";
            break;
        case GUARD_DENY_RATE_LIMIT:
            reason = "rate limit exceeded";
            break;
        case GUARD_DENY_INPUT_SIZE:
            reason = "input too large";
            break;
        case GUARD_DENY_INPUT_INVALID:
            reason = "invalid input";
            break;
        default:
            break;
        }
        syslog(LOG_WARNING, "[%s] Tool '%s' blocked: %s\n",
               TAG, name ? name : "(null)", reason);
        snprintf(output, output_size,
                 "Error: tool '%s' blocked — %s",
                 name ? name : "(null)", reason);
        return ERROR;
    }

    /* Try builtin tools first */
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            syslog(LOG_INFO, "[%s] Executing tool: %s\n", TAG, name);
            int ret = s_tools[i].execute(input_json, output, output_size);
            if (ret == OK) {
                tool_guard_record_call(name);
            }
            return ret;
        }
    }

    /* Try MCP bridge */
#ifdef CONFIG_AI_AGENT_MCP
    if (mcp_bridge_execute(name, input_json, output, output_size) == OK) {
        syslog(LOG_INFO, "[%s] Executed MCP tool: %s\n", TAG, name);
        tool_guard_record_call(name);
        return OK;
    }
#endif

    /* Try registered providers */
    for (int p = 0; p < s_provider_count; p++) {
        if (!s_providers[p].execute) {
            continue;
        }
        int ret = s_providers[p].execute(name, input_json, output, output_size);
        if (ret == OK) {
            syslog(LOG_INFO, "[%s] Executed %s tool: %s\n",
                   TAG, s_providers[p].name, name);
            tool_guard_record_call(name);
            return OK;
        }
    }

    syslog(LOG_WARNING, "[%s] Unknown tool: %s\n", TAG, name);
    snprintf(output, output_size, "Error: unknown tool '%s'", name);
    return ERROR;
}
