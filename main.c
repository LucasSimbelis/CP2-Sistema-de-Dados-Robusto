#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "MULTIMOD";

#define SENSOR_TASK_PRIO    6
#define RECV_TASK_PRIO      4
#define LOG_TASK_PRIO       2
#define MONITOR_TASK_PRIO   5

#define SENSOR_STACK_WORDS  4096
#define RECV_STACK_WORDS    4096
#define LOG_STACK_WORDS     3072
#define MONITOR_STACK_WORDS 4096

#define QUEUE_LEN           10
#define QUEUE_ITEM_SIZE     sizeof(message_t)
#define QUEUE_RX_TMO_MS     1000
#define MONITOR_PERIOD_MS   2000
#define MAX_TIMEOUTS_BEFORE_RECOVERY 5
#define WDT_TIMEOUT_S       5

typedef struct {
    uint32_t seq;
    int value;
} message_t;

static QueueHandle_t g_queue = NULL;
static TaskHandle_t g_task_sensor = NULL;
static TaskHandle_t g_task_recv   = NULL;
static TaskHandle_t g_task_log    = NULL;
static TaskHandle_t g_task_monitor= NULL;

static volatile TickType_t sensor_hb = 0;
static volatile TickType_t recv_hb   = 0;

typedef struct {
    volatile bool sensor_alive;
    volatile bool recv_alive;
} status_flags_t;

static status_flags_t g_flags = {
    .sensor_alive = false,
    .recv_alive = false
};

static void task_sensor(void *pv);
static void task_recv(void *pv);
static void task_log(void *pv);
static void task_monitor(void *pv);
static void create_tasks(void);
static void recreate_recv_task(void);
static void recreate_sensor_task(void);

static void task_sensor(void *pv) {
    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "[SENSOR] Iniciando tarefa de geração");
    uint32_t seq = 0;
    for (;;) {
        message_t msg = { .seq = seq++, .value = (int)(esp_random() % 1000) };
        if (xQueueSend(g_queue, &msg, 0) != pdTRUE) {
            printf("[SENSOR] Fila cheia, mensagem perdida (seq=%" PRIu32 ")\n", msg.seq);
        } else {
            sensor_hb = xTaskGetTickCount();
            g_flags.sensor_alive = true;
            printf("[SENSOR] Enviada mensagem seq=%" PRIu32 " valor=%d\n", msg.seq, msg.value);
        }
        UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
        if (watermark < 100) {
            printf("[SENSOR] Atenção: pouca pilha restante (%u words)\n", (unsigned int)watermark);
        }
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
}

static void task_recv(void *pv) {
    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "[RECV] Iniciando tarefa de recepção");
    int timeouts = 0;
    for (;;) {
        message_t *pmsg = NULL;
        TickType_t tmo = pdMS_TO_TICKS(QUEUE_RX_TMO_MS);
        if (tmo == 0) tmo = 1;
        message_t temp;
        if (xQueueReceive(g_queue, &temp, tmo) == pdTRUE) {
            timeouts = 0;
            pmsg = malloc(sizeof(message_t));
            if (pmsg == NULL) {
                printf("[RECV] Erro: malloc falhou (seq=%" PRIu32 "). Ignorando.\n", temp.seq);
            } else {
                memcpy(pmsg, &temp, sizeof(message_t));
                recv_hb = xTaskGetTickCount();
                g_flags.recv_alive = true;
                printf("[RECV] Transmitindo (malloc): seq=%" PRIu32 " valor=%d\n", pmsg->seq, pmsg->value);
                size_t free_heap = xPortGetFreeHeapSize();
                size_t min_heap  = xPortGetMinimumEverFreeHeapSize();
                if (free_heap < 20 * 1024) {
                    printf("[RECV] Atenção: pouca heap livre (%u bytes, mínimo histórico %u)\n",
                           (unsigned int)free_heap, (unsigned int)min_heap);
                }
                free(pmsg);
                pmsg = NULL;
            }
        } else {
            timeouts++;
            printf("[RECV] Timeout na fila (%d)\n", timeouts);
            if (timeouts == 1) {
                printf("[RECV] Aviso: primeira falta de dados\n");
            } else if (timeouts == 2) {
                printf("[RECV] Recuperação leve: tentar limpar caches locais / reconfig.\n");
            } else if (timeouts == 4) {
                printf("[RECV] Recuperação moderada: resetando fila para limpar possíveis bloqueios\n");
                xQueueReset(g_queue);
            } else if (timeouts >= MAX_TIMEOUTS_BEFORE_RECOVERY) {
                printf("[RECV] Falha persistente: encerrando tarefa para recriação pelo monitor\n");
                break;
            }
        }
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    printf("[RECV] Liberando recursos e finalizando tarefa (para monitoração)\n");
    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
}

static void task_log(void *pv) {
    ESP_LOGI(TAG, "[LOG] Iniciando tarefa de log");
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        printf("[LOG] Status - sensor_alive=%s recv_alive=%s sensor_hb=%u recv_hb=%u\n",
               g_flags.sensor_alive ? "TRUE" : "FALSE",
               g_flags.recv_alive   ? "TRUE" : "FALSE",
               (unsigned int)sensor_hb, (unsigned int)recv_hb);
        g_flags.sensor_alive = false;
        g_flags.recv_alive = false;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void task_monitor(void *pv) {
    ESP_LOGI(TAG, "[MON] Iniciando tarefa de monitoramento");
    int recv_restart_count = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(MONITOR_PERIOD_MS));
        TickType_t now = xTaskGetTickCount();
        if ((now - sensor_hb) > pdMS_TO_TICKS(2 * MONITOR_PERIOD_MS)) {
            printf("[MON] Sensor possivelmente travado. Reiniciando tarefa sensor...\n");
            if (g_task_sensor) {
                vTaskDelete(g_task_sensor);
                g_task_sensor = NULL;
            }
            recreate_sensor_task();
            sensor_hb = xTaskGetTickCount();
        }
        if (g_task_recv == NULL || (now - recv_hb) > pdMS_TO_TICKS(5 * MONITOR_PERIOD_MS)) {
            printf("[MON] Receptor inativo. Recriando tarefa de recepção...\n");
            if (g_task_recv) {
                vTaskDelete(g_task_recv);
                g_task_recv = NULL;
            }
            recreate_recv_task();
            recv_restart_count++;
            if (recv_restart_count >= 3) {
                size_t free_heap = xPortGetFreeHeapSize();
                if (free_heap < 16 * 1024) {
                    printf("[MON] Memória crítica detectada (%u bytes). Reiniciando o sistema.\n", (unsigned int)free_heap);
                    esp_restart();
                }
            }
            recv_hb = xTaskGetTickCount();
        }
        size_t free = xPortGetFreeHeapSize();
        size_t min  = xPortGetMinimumEverFreeHeapSize();
        printf("[MON] Memória livre=%u bytes (mín histor=%u)\n", (unsigned int)free, (unsigned int)min);
        if (min < 8 * 1024) {
            printf("[MON] Memória mínima histórica muito baixa. Reiniciando sistema...\n");
            esp_restart();
        }
        esp_task_wdt_reset();
    }
}

static void create_tasks(void) {
    xTaskCreatePinnedToCore(task_sensor, "task_sensor", SENSOR_STACK_WORDS, NULL, SENSOR_TASK_PRIO, &g_task_sensor, 1);
    xTaskCreatePinnedToCore(task_recv,   "task_recv",   RECV_STACK_WORDS,   NULL, RECV_TASK_PRIO,   &g_task_recv,   1);
    xTaskCreatePinnedToCore(task_log,    "task_log",    LOG_STACK_WORDS,    NULL, LOG_TASK_PRIO,    &g_task_log,    1);
    xTaskCreatePinnedToCore(task_monitor,"task_monitor",MONITOR_STACK_WORDS,NULL, MONITOR_TASK_PRIO, &g_task_monitor, 1);
}

static void recreate_recv_task(void) {
    if (g_task_recv != NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));
        vTaskDelete(g_task_recv);
        g_task_recv = NULL;
    }
    xTaskCreatePinnedToCore(task_recv, "task_recv", RECV_STACK_WORDS, NULL, RECV_TASK_PRIO, &g_task_recv, 1);
    printf("[MON] Tarefa de recepção recriada.\n");
}

static void recreate_sensor_task(void) {
    if (g_task_sensor != NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));
        vTaskDelete(g_task_sensor);
        g_task_sensor = NULL;
    }
    xTaskCreatePinnedToCore(task_sensor, "task_sensor", SENSOR_STACK_WORDS, NULL, SENSOR_TASK_PRIO, &g_task_sensor, 1);
    printf("[MON] Tarefa sensor recriada.\n");
}

void app_main(void) {
    esp_log_level_set(TAG, ESP_LOG_INFO);
    printf("\n\n=== Sistema multicamadas com FreeRTOS + WDT (ESP32) ===\n");
    esp_err_t err = esp_task_wdt_init(WDT_TIMEOUT_S, true);
    if (err != ESP_OK) {
        printf("[MAIN] Falha ao inicializar task WDT (err=%d). Continuando sem WDT.\n", err);
    } else {
        printf("[MAIN] Task WDT inicializado com timeout = %d s\n", WDT_TIMEOUT_S);
    }
    g_queue = xQueueCreate(QUEUE_LEN, QUEUE_ITEM_SIZE);
    if (!g_queue) {
        printf("[MAIN] Falha ao criar fila. Reiniciando sistema...\n");
        esp_restart();
    }
    create_tasks();
    printf("[MAIN] Tarefas criadas. Sistema operacional rodando.\n");
}
