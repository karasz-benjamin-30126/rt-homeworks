// Device-ul IoT din tema 01, de data aceasta pe un ESP32-C3 Super Mini.
//
// Aceeași logică și același protocol ca device-simulator din tema 01, cu trei
// taskuri FreeRTOS în loc de trei thread-uri Java:
//
//   task periodic --(coada de alarme)--> task sporadic
//        |                                     |
//        +---- telemetry ---> MQTT <--- alarm -+
//                              |
//                          commands --> command_received() --(coada de comenzi)--> task aperiodic ---> status
//
// Gata implementate (nu trebuie modificate):
//   config.h         setările: Wi-Fi, broker, STUDENT, valorile inițiale; DEVICE_ID
//                    se citește din adresa MAC a plăcii
//   connection.h/cpp Wi-Fi, ora exactă (SNTP), clientul MQTT, mqtt_publish(), epoch_ms()
//
// Placa: ESP32C3 Dev Module, USB CDC On Boot: Enabled. Serial Monitor: 115200 baud.

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "connection.h"

// ---------------------------------------------------------------------------
// Starea comună și cozile
// ---------------------------------------------------------------------------

// Perioada și pragul: scrise de taskul aperiodic, citite de taskul periodic la
// fiecare job. Sunt pe 32 de biți, deci o citire/scriere este atomică pe
// ESP32-C3; volatile obligă compilatorul să le citească din memorie de fiecare dată.
volatile uint32_t g_period_ms = PERIOD_MS_INITIAL;
volatile float g_threshold = THRESHOLD_INITIAL;

// Evenimentul de alarmă, pus în coadă prin valoare (xQueueSend îl copiază).
struct AlarmEvent {
    float temperature;
    float threshold;
    int64_t timestamp;  // epoch_ms() în momentul citirii senzorului
};

// Comanda primită, cu textul copiat în element (nu un pointer: textul primit
// în command_received() este valid doar în timpul apelului).
struct Command {
    char text[COMMAND_MAX_LEN];
};

const UBaseType_t ALARM_QUEUE_LEN = 8;
const UBaseType_t COMMAND_QUEUE_LEN = 8;

QueueHandle_t g_alarm_queue = NULL;
QueueHandle_t g_command_queue = NULL;

// Prioritățile: între 2 și 19 (peste loopTask = 1, sub taskurile sistemului
// pentru evenimente și Wi-Fi = 20, 23). Taskul periodic are prioritatea cea mai
// mare, ca eliberările lui să nu fie întârziate de celelalte; comenzile sunt
// tratate ca activitate aperiodică de fond, cu prioritatea cea mai mică.
const UBaseType_t PRIO_PERIODIC = 5;
const UBaseType_t PRIO_SPORADIC = 4;
const UBaseType_t PRIO_APERIODIC = 3;

const uint32_t TASK_STACK_BYTES = 4096;

// limitele acceptate pentru SET_INTERVAL
const uint32_t PERIOD_MS_MIN = 100;
const uint32_t PERIOD_MS_MAX = 3600000;  // o oră

// Temperatura cipului, în °C, de la senzorul intern al ESP32-C3.
// Este temperatura siliciului, nu a camerei: imediat după pornirea Wi-Fi-ului
// poate fi în jur de 50 °C, apoi scade în câteva minute spre 35-40 °C.
// Rezoluția este de aproximativ 1 °C. Ca să provocați alarme, puneți pragul
// cu SET_THRESHOLD cu un grad sub valoarea citită.
float read_temperature(void) {
    return temperatureRead();
}

// ---------------------------------------------------------------------------
// TODO 1: TASK PERIODIC
// ---------------------------------------------------------------------------
// Eliberări pe grila r_k = r_0 + kT, cu așteptare absolută (xTaskDelayUntil).
// Perioada se citește la fiecare job: o comandă SET_INTERVAL se aplică de la
// următoarea eliberare.
void periodic_task(void* arg) {
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();  // r_0
    uint32_t job = 0;

    for (;;) {
        float threshold = g_threshold;  // o singură citire per job: valoare coerentă
        float temperature = read_temperature();
        int64_t ts = epoch_ms();

        char msg[96];
        snprintf(msg, sizeof(msg), "%s;%lld;%.2f", DEVICE_ID, (long long)ts, temperature);
        mqtt_publish("telemetry", msg);
        Serial.printf("[periodic] job %lu la %lu ms: %.2f °C (T = %lu ms)\n", (unsigned long)job,
                      (unsigned long)millis(), temperature, (unsigned long)g_period_ms);

        if (temperature > threshold) {
            AlarmEvent ev = {temperature, threshold, ts};
            // timeout 0: taskul periodic nu se blochează niciodată din cauza alarmelor
            if (xQueueSend(g_alarm_queue, &ev, 0) != pdTRUE) {
                Serial.println("[periodic] coada de alarme este plină, eveniment pierdut");
            }
        }

        job++;
        // așteaptă până la last_wake + T și actualizează last_wake (fără derivă)
        uint32_t period_ms = g_period_ms;
        TickType_t period_ticks = pdMS_TO_TICKS(period_ms);
        if (period_ticks == 0) period_ticks = 1;
        xTaskDelayUntil(&last_wake, period_ticks);
    }
}

// ---------------------------------------------------------------------------
// TODO 2: TASK SPORADIC
// ---------------------------------------------------------------------------
// Fiecare eveniment din coada de alarme este o sosire. Sosirile la mai puțin
// de MIN_INTERARRIVAL_MS după ultima alarmă publicată sunt respinse.
// Intervalele se măsoară cu millis() (monoton), nu cu epoch_ms().
void sporadic_task(void* arg) {
    (void)arg;
    bool has_last = false;
    uint32_t last_published_ms = 0;

    for (;;) {
        AlarmEvent ev;
        if (xQueueReceive(g_alarm_queue, &ev, portMAX_DELAY) != pdTRUE) continue;

        uint32_t now = millis();
        // diferența fără semn rămâne corectă și când millis() trece prin 0
        if (has_last && (uint32_t)(now - last_published_ms) < MIN_INTERARRIVAL_MS) {
            Serial.printf("[sporadic] alarmă respinsă: %.2f °C > %.2f, la %lu ms după ultima (minim %lu ms)\n",
                          ev.temperature, ev.threshold, (unsigned long)(now - last_published_ms),
                          (unsigned long)MIN_INTERARRIVAL_MS);
            continue;
        }

        char msg[112];
        snprintf(msg, sizeof(msg), "%s;%lld;%.2f;%.2f", DEVICE_ID, (long long)ev.timestamp, ev.temperature,
                 ev.threshold);
        mqtt_publish("alarm", msg);
        has_last = true;
        last_published_ms = now;
        Serial.printf("[sporadic] ALARMĂ publicată: %.2f °C > prag %.2f\n", ev.temperature, ev.threshold);
    }
}

// ---------------------------------------------------------------------------
// TODO 3: TASK APERIODIC
// ---------------------------------------------------------------------------

// Publică pe "status" mesajul "deviceId;timestamp;text".
static void publish_status(const char* text) {
    char msg[160];
    snprintf(msg, sizeof(msg), "%s;%lld;%s", DEVICE_ID, (long long)epoch_ms(), text);
    mqtt_publish("status", msg);
    Serial.printf("[aperiodic] %s\n", text);
}

// Elimină spațiile (și '\r', '\n') de la început și de la sfârșit, pe loc.
static char* trim(char* s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char* end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

// Interpretează un întreg zecimal fără semn; false dacă textul nu este un număr întreg.
static bool parse_uint32(const char* s, uint32_t* out) {
    if (s == NULL || *s == '\0' || *s == '-' || *s == '+') return false;
    errno = 0;
    char* end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v > 0xFFFFFFFFUL) return false;
    *out = (uint32_t)v;
    return true;
}

// Interpretează un număr zecimal (cu punct); false dacă textul nu este un număr.
static bool parse_float(const char* s, float* out) {
    if (s == NULL || *s == '\0') return false;
    errno = 0;
    char* end = NULL;
    float v = strtof(s, &end);
    if (errno != 0 || end == s || *end != '\0' || isnan(v) || isinf(v)) return false;
    *out = v;
    return true;
}

// Execută o comandă și publică confirmarea (OK ...) sau eroarea (ERROR ...).
static void execute_command(char* line) {
    char reply[128];
    char* text = trim(line);

    // separăm numele comenzii de parametru
    char* save = NULL;
    char* name = strtok_r(text, " \t", &save);
    char* param = strtok_r(NULL, " \t", &save);
    char* extra = strtok_r(NULL, " \t", &save);

    if (name == NULL) {
        publish_status("ERROR comandă goală");
        return;
    }

    if (strcmp(name, "SET_INTERVAL") == 0) {
        uint32_t ms;
        if (param == NULL || extra != NULL || !parse_uint32(param, &ms)) {
            publish_status("ERROR SET_INTERVAL cere un număr întreg de milisecunde");
        } else if (ms < PERIOD_MS_MIN || ms > PERIOD_MS_MAX) {
            snprintf(reply, sizeof(reply), "ERROR SET_INTERVAL %lu în afara intervalului %lu..%lu ms",
                     (unsigned long)ms, (unsigned long)PERIOD_MS_MIN, (unsigned long)PERIOD_MS_MAX);
            publish_status(reply);
        } else {
            g_period_ms = ms;
            snprintf(reply, sizeof(reply), "OK SET_INTERVAL %lu", (unsigned long)ms);
            publish_status(reply);
        }
    } else if (strcmp(name, "SET_THRESHOLD") == 0) {
        float value;
        if (param == NULL || extra != NULL || !parse_float(param, &value)) {
            publish_status("ERROR SET_THRESHOLD cere o valoare numerică, de exemplu 35.5");
        } else {
            g_threshold = value;
            snprintf(reply, sizeof(reply), "OK SET_THRESHOLD %.2f", value);
            publish_status(reply);
        }
    } else if (strcmp(name, "RESET") == 0) {
        if (param != NULL) {
            publish_status("ERROR RESET nu are parametri");
        } else {
            g_period_ms = PERIOD_MS_INITIAL;
            g_threshold = THRESHOLD_INITIAL;
            snprintf(reply, sizeof(reply), "OK RESET interval=%lu threshold=%.2f", (unsigned long)PERIOD_MS_INITIAL,
                     THRESHOLD_INITIAL);
            publish_status(reply);
        }
    } else if (strcmp(name, "STATUS") == 0) {
        if (param != NULL) {
            publish_status("ERROR STATUS nu are parametri");
        } else {
            snprintf(reply, sizeof(reply), "OK STATUS interval=%lu threshold=%.2f", (unsigned long)g_period_ms,
                     (float)g_threshold);
            publish_status(reply);
        }
    } else {
        snprintf(reply, sizeof(reply), "ERROR comandă necunoscută: %.40s", name);
        publish_status(reply);
    }
}

// Fiecare comandă din coadă eliberează un job.
void aperiodic_task(void* arg) {
    (void)arg;
    for (;;) {
        Command cmd;
        if (xQueueReceive(g_command_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        cmd.text[COMMAND_MAX_LEN - 1] = '\0';
        Serial.printf("[aperiodic] execut: %s\n", cmd.text);
        execute_command(cmd.text);
    }
}

// Apelată de clientul MQTT pentru fiecare comandă primită (vezi connection.h).
// Rulează pe taskul clientului MQTT: doar copiază comanda în coadă, fără să se blocheze.
void command_received(const char* text) {
    Serial.printf("[mqtt] comandă primită: %s\n", text);
    if (g_command_queue == NULL) return;

    Command cmd;
    strncpy(cmd.text, text, COMMAND_MAX_LEN - 1);
    cmd.text[COMMAND_MAX_LEN - 1] = '\0';
    if (xQueueSend(g_command_queue, &cmd, 0) != pdTRUE) {
        Serial.println("[mqtt] coada de comenzi este plină, comandă pierdută");
    }
}

void setup() {
    Serial.begin(115200);
    // Fără așteptare la scriere: cu placa la USB, dar fără Serial Monitor deschis,
    // fiecare Serial.printf() ar aștepta până la 2 s să fie citit și ar întârzia
    // taskul care scrie. Așa, textul pe care nu-l citește nimeni se pierde.
    Serial.setTxTimeoutMs(0);
    uint32_t t = millis();
    while (!Serial && millis() - t < 3000) delay(10);  // așteptăm Serial Monitor, cel mult 3 s
    delay(200);
    Serial.println();
    Serial.printf("=== tema 02: STUDENT %s, DEVICE_ID %s ===\n", STUDENT, DEVICE_ID);

    wifi_connect();
    time_sync();

    // TODO 0 (a): cozile, înainte de mqtt_start()
    g_alarm_queue = xQueueCreate(ALARM_QUEUE_LEN, sizeof(AlarmEvent));
    g_command_queue = xQueueCreate(COMMAND_QUEUE_LEN, sizeof(Command));
    if (g_alarm_queue == NULL || g_command_queue == NULL) {
        Serial.println("EROARE: nu s-au putut crea cozile (memorie insuficientă)");
        for (;;) delay(1000);
    }

    mqtt_start();

    // TODO 0 (b): taskurile, după mqtt_start()
    BaseType_t ok = pdPASS;
    ok &= xTaskCreate(periodic_task, "periodic", TASK_STACK_BYTES, NULL, PRIO_PERIODIC, NULL);
    ok &= xTaskCreate(sporadic_task, "sporadic", TASK_STACK_BYTES, NULL, PRIO_SPORADIC, NULL);
    ok &= xTaskCreate(aperiodic_task, "aperiodic", TASK_STACK_BYTES, NULL, PRIO_APERIODIC, NULL);
    if (ok != pdPASS) {
        Serial.println("EROARE: nu s-au putut crea toate taskurile");
    } else {
        Serial.printf("taskuri pornite: periodic (prio %u), sporadic (prio %u), aperiodic (prio %u)\n",
                      (unsigned)PRIO_PERIODIC, (unsigned)PRIO_SPORADIC, (unsigned)PRIO_APERIODIC);
    }
}

void loop() {
    // Totul se face în taskuri: loop() nu are nimic de făcut.
    delay(1000);
}
