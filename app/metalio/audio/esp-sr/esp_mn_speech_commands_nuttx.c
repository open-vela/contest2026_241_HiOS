#ifndef CONFIG_SR_MN_EN_MULTINET7_QUANT
#define CONFIG_SR_MN_EN_MULTINET7_QUANT 1
#endif
#include <stdio.h>
#include <stdlib.h>
#include "string.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mn_speech_commands.h"
#include "esp_mn_iface.h"
#include "../wake_words/hi_openvela_command.h"
#include <stdint.h>


static char *TAG = "MN_COMMAND";
static esp_mn_node_t *esp_mn_root = NULL;
const static esp_mn_iface_t *esp_mn_model_handle = NULL;
static model_iface_data_t *esp_mn_model_data = NULL;

void *_esp_mn_calloc_(int n, int size)
{
#ifdef ESP_PLATFORM
    void *data = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM);
    if (data == NULL) 
        data = calloc(n, size);
    return data;
#else
    return calloc(n, size);
#endif
}

#define ESP_RETURN_ON_FALSE(a, err_code, log_tag, format, ...) do {                             \
        if (!(a)) {                                                                             \
            ESP_LOGE(log_tag, "%s(%d): " format, __FUNCTION__, __LINE__, ##__VA_ARGS__);        \
            return err_code;                                                                    \
        }                                                                                       \
    } while(0)

esp_err_t esp_mn_commands_alloc(const esp_mn_iface_t *multinet, model_iface_data_t *model_data)
{
    if (esp_mn_root != NULL) {
        esp_mn_commands_free();
    }
    if (!multinet || !model_data) return ESP_ERR_INVALID_ARG;
    esp_mn_root = esp_mn_node_alloc(NULL);
    if (!esp_mn_root) return ESP_ERR_NO_MEM;
    esp_mn_model_handle = multinet;
    esp_mn_model_data = model_data;
    return ESP_OK;
}

esp_err_t esp_mn_commands_free(void)
{
    if (!esp_mn_root) {
        esp_mn_model_handle = NULL;
        esp_mn_model_data = NULL;
        return ESP_OK;
    }
    esp_mn_commands_clear();
    esp_mn_node_free(esp_mn_root);
    esp_mn_root = NULL;
    esp_mn_model_handle = NULL;
    esp_mn_model_data = NULL;

    return ESP_OK;
}

int esp_mn_commands_num(void)
{
    esp_mn_node_t *t = esp_mn_root;
    int i = 0;
    while (t && t->next) {
        t = t->next;
        i++;
    }
    return i;
}

esp_err_t esp_mn_commands_clear(void)
{
    ESP_RETURN_ON_FALSE(NULL != esp_mn_root, ESP_ERR_INVALID_STATE, TAG, "The mn commands is not initialized");

    esp_mn_node_t *t = esp_mn_root->next;
    while (t) {
        esp_mn_node_t *cur_node = t;
        t = t->next;
        esp_mn_node_free(cur_node);
    }
    esp_mn_root->next = NULL;

    return ESP_OK;
}

esp_mn_node_t *esp_mn_command_search(const char *string)
{
    esp_mn_node_t *temp = esp_mn_root;
    if(NULL == esp_mn_root) {
        return NULL;
    }

    while (temp->next) {
        temp = temp->next;
        if (strcmp(string, temp->phrase->string) == 0) {
            return temp;
        }
    }
    return NULL;
}

// Fixed English vocabulary uses explicit phonemes, avoiding runtime G2P.
esp_err_t esp_mn_commands_add(int command_id, const char *string)
{
    if (!string) return ESP_ERR_INVALID_ARG;
    if (command_id==HI_OPENVELA_COMMAND_ID) {
        if (!strcmp(string, HI_OPENVELA_PHRASE) || !strcmp(string, "HI OPENVELA"))
            return esp_mn_commands_phoneme_add(command_id, HI_OPENVELA_PHRASE, HI_OPENVELA_PHONEMES);
        const char *variants[]={HI_OPENVELA_PHONEMES,HI_OPENVELA_PHONEMES_EY,HI_OPENVELA_PHONEMES_IY};
        for (unsigned i=0;i<3;++i) if (!strcmp(string,variants[i]))
            return esp_mn_commands_phoneme_add(command_id,HI_OPENVELA_PHRASE,variants[i]);
    } else if (command_id==HI_OPENVELA_REJECT_COMMAND_ID) {
        const char *variants[]={HI_OPENVELA_REJECT_PHONEMES,HI_OPENVELA_REJECT_PHONEMES_EY,HI_OPENVELA_REJECT_PHONEMES_IY};
        for (unsigned i=0;i<3;++i) if (!strcmp(string,variants[i]))
            return esp_mn_commands_phoneme_add(command_id,HI_OPENVELA_REJECT_PHRASE,variants[i]);
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t esp_mn_commands_phoneme_add(int command_id, const char *string, const char *phonemes)
{
    if (!string || !*string || !phonemes || !*phonemes || command_id<=0 || command_id>INT16_MAX)
        return ESP_ERR_INVALID_ARG;
    if (NULL == esp_mn_root || esp_mn_model_handle == NULL || esp_mn_model_data == NULL) {
        ESP_LOGE(TAG, "Please create mn model first.\n");
        return ESP_ERR_INVALID_STATE;
    }
    esp_mn_node_t *temp = esp_mn_root;
    int last_node_elem_num = esp_mn_commands_num();
    ESP_RETURN_ON_FALSE(ESP_MN_MAX_PHRASE_NUM > last_node_elem_num, ESP_ERR_INVALID_STATE, TAG, "The number of speech commands exceed ESP_MN_MAX_PHRASE_NUM");

#if CONFIG_SR_MN_EN_MULTINET7_QUANT || CONFIG_SR_MN_EN_MULTINET5_SINGLE_RECOGNITION_QUANT8
    // the unit of multinet7 or multinet5q8 is phoneme
    if (esp_mn_model_handle->check_speech_command(esp_mn_model_data, phonemes) == 0) {
        // error message is printed inside check_speech_command
        ESP_LOGE(TAG, "invalid command, please check format, %s (%s).\n", string, phonemes);
        return ESP_ERR_INVALID_STATE;
    }
#else
    // The unit of multinet6 is grapheme
    if (esp_mn_model_handle->check_speech_command(esp_mn_model_data, string) == 0) {
        // error message is printed inside check_speech_command
        ESP_LOGE(TAG, "invalid command, please check format, %s.\n", string);
        return ESP_ERR_INVALID_STATE;
    }
#endif

    /* The same displayed command may have several pronunciation variants.
     * Deduplicate the (text, phonemes) pair, keeping their command ID equal. */
    for (temp=esp_mn_root->next; temp; temp=temp->next) {
        if (!strcmp(string, temp->phrase->string) && temp->phrase->phonemes &&
            !strcmp(phonemes, temp->phrase->phonemes)) {
            temp->phrase->command_id=command_id;
            return ESP_OK;
        }
    }

    temp = esp_mn_root;
#if CONFIG_SR_MN_EN_MULTINET5_SINGLE_RECOGNITION_QUANT8
    //TODO:: add string for mn5
    esp_mn_phrase_t *phrase = esp_mn_phrase_alloc(command_id, phonemes);
#else
    esp_mn_phrase_t *phrase = esp_mn_phrase_alloc(command_id, string);
#endif
    if (phrase == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (phonemes) {
        int phoneme_len = strlen(phonemes);
        phrase->phonemes = _esp_mn_calloc_(phoneme_len+1, sizeof(char));
        if (!phrase->phonemes) { esp_mn_phrase_free(phrase); return ESP_ERR_NO_MEM; }
        memcpy(phrase->phonemes, phonemes, phoneme_len);
        phrase->phonemes[phoneme_len] = '\0';
    }

    esp_mn_node_t *new_node = esp_mn_node_alloc(phrase);
    if (!new_node) { esp_mn_phrase_free(phrase); return ESP_ERR_NO_MEM; }
    while (temp->next != NULL) {
        temp = temp->next;
    }
    temp->next = new_node;

    return ESP_OK;
}

esp_err_t esp_mn_commands_modify(const char *old_string, const char *new_string)
{
    (void)old_string; (void)new_string;
    return ESP_ERR_INVALID_STATE;
}

esp_err_t esp_mn_commands_remove(const char *string)
{
    esp_mn_node_t *temp = esp_mn_root;
    ESP_RETURN_ON_FALSE(NULL != esp_mn_root, ESP_ERR_INVALID_STATE, TAG, "The mn commands is not initialized");

    // search string to get node point
    bool flag = false;
    while (temp->next) {
        if (strcmp(string, temp->next->phrase->string) == 0) {
            flag = true;
            break;
        }
        temp = temp->next;
    }

    if (flag) {
        esp_mn_node_t *rm_node = temp->next;
        temp->next = temp->next->next;
        esp_mn_node_free(rm_node);
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "No such speech command: \"%s\"", string);
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

esp_mn_phrase_t *esp_mn_commands_get_from_index(int index)
{
    if (NULL == esp_mn_root) {
        ESP_LOGW(TAG, "%s", "The mn commands is not initialized");
        return NULL;
    }

    // phrase index also is phrase id, which is the depth from this phrase node to root node
    esp_mn_node_t *temp = esp_mn_root;
    for (int i = -1; i < index; i++) {
        if (temp->next == NULL) {
            return NULL;
        }
        temp = temp->next;
    }

    return temp->phrase;
}

esp_mn_phrase_t *esp_mn_commands_get_from_string(const char *string)
{
    if (NULL == esp_mn_root) {
        ESP_LOGW(TAG, "%s", "The mn commands is not initialized");
        return NULL;
    }

    // phrase index also is phrase id, which is the depth from this phrase node to root node
    esp_mn_node_t *temp = esp_mn_root;
    while (temp->next) {
        if (strcmp(string, temp->next->phrase->string) == 0) {
            return temp->next->phrase;
        }
        temp = temp->next;
    }

    return NULL;
}

char *esp_mn_commands_get_string(int command_id)
{
    if (NULL == esp_mn_root) {
        ESP_LOGW(TAG, "%s", "The mn commands is not initialized");
        return NULL;
    }

    // phrase index also is phrase id, which is the depth from this phrase node to root node
    esp_mn_node_t *temp = esp_mn_root;
    while (temp->next) {
        if (temp->next->phrase->command_id == command_id) {
            return temp->next->phrase->string;
        }
        temp = temp->next;
    }

    return NULL;
}

esp_mn_error_t *esp_mn_commands_update()
{
    static esp_mn_error_t unavailable = { .num = 1, .phrases = NULL };
    if (!esp_mn_root || !esp_mn_model_handle || !esp_mn_model_data ||
        !esp_mn_model_handle->set_speech_commands) return &unavailable;
    esp_mn_error_t *error = esp_mn_model_handle->set_speech_commands(esp_mn_model_data, esp_mn_root);
    if (!error) return &unavailable;

    if (error->num == 0) {
        return NULL;
    }

    return error;
}

void esp_mn_commands_print(void)
{
    ESP_LOGI(TAG, "---------------------SPEECH COMMANDS---------------------");
    esp_mn_node_t *temp = esp_mn_root;
    int phrase_id = 0;
    while (temp->next) {
        temp = temp->next;
        ESP_LOGI(TAG, "Command ID%d, phrase ID%d: %s", temp->phrase->command_id, phrase_id, temp->phrase->string);
        phrase_id++;
    }
    ESP_LOGI(TAG, "---------------------------------------------------------\n");
}

void esp_mn_active_commands_print(void)
{
    ESP_LOGI(TAG, "---------------------ACTIVE SPEECH COMMANDS---------------------");
    esp_mn_model_handle->print_active_speech_commands(esp_mn_model_data);
    ESP_LOGI(TAG, "---------------------------------------------------------\n");
}

esp_mn_phrase_t *esp_mn_phrase_alloc(int command_id, const char *string)
{

    int string_len = strlen(string);
    ESP_RETURN_ON_FALSE( string_len > 0, NULL, TAG, "input string is empty");

    esp_mn_phrase_t *phrase = _esp_mn_calloc_(1, sizeof(esp_mn_phrase_t));
    ESP_RETURN_ON_FALSE(NULL != phrase, NULL, TAG, "Fail to alloc mn phrase");

    phrase->string = _esp_mn_calloc_(string_len+1, sizeof(char));
    if (!phrase->string) { free(phrase); return NULL; }
    memcpy(phrase->string, string, string_len);
    phrase->string[string_len] = '\0';
    phrase->command_id = command_id;
    phrase->threshold = 0;
    phrase->wave = NULL;
    phrase->phonemes = NULL;

    return phrase;
}

void esp_mn_phrase_free(esp_mn_phrase_t *phrase)
{
    if (phrase != NULL) {
        if (phrase->wave != NULL) {
            free(phrase->wave);
        }
        if (phrase->string != NULL) {
            free(phrase->string);
        }
        if (phrase->phonemes != NULL) {
            free(phrase->phonemes);
        }
        free(phrase);
    }
}

esp_mn_node_t *esp_mn_node_alloc(esp_mn_phrase_t *phrase)
{
    esp_mn_node_t *node = _esp_mn_calloc_(1, sizeof(esp_mn_node_t));
    ESP_RETURN_ON_FALSE(NULL != node, NULL, TAG, "Fail to alloc mn node");
    node->next = NULL;
    node->phrase = phrase;
    return node;
}

void esp_mn_node_free(esp_mn_node_t *node)
{
    if (node != NULL) {
        esp_mn_phrase_free(node->phrase);
        free(node);
    }
}