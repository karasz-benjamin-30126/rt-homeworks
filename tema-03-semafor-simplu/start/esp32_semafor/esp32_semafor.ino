// Semafor pentru pietoni, cu buton, pe ESP32-C3 Super Mini.
//
// Placa este controlerul semaforului; lămpile sunt desenate de dashboard, care
// le aprinde după mesajele "lights" publicate de placă. Patru activități:
//
//   ISR buton BOOT ----+
//                      +--(coada butonului)--> task sporadic
//   command_received --+   (BUTTON)                 |
//                                                   v
//                                         [ starea semaforului ] <-- mutex
//                                            ^          |
//                                            |          v
//                         task periodic de control   task periodic de ieșire
//                         (fazele, T = 100 ms)       (lămpile, T = 500 ms) --> lights
//
// Gata implementate (nu trebuie modificate):
//   config.h         setările: Wi-Fi, broker, STUDENT, pinii, perioadele, duratele fazelor
//   connection.h/cpp Wi-Fi, ora exactă (SNTP), clientul MQTT, mqtt_publish(), epoch_ms()
//
// Placa: ESP32C3 Dev Module, USB CDC On Boot: Enabled. Serial Monitor: 115200 baud.

#include "config.h"
#include "connection.h"

// Fazele semaforului (vezi tabelul și diagrama din README.md). Numele din
// PHASE_NAMES sunt cele din mesajul "lights", în aceeași ordine ca în enum.
enum Phase { CAR_GREEN, CAR_YELLOW, ALL_RED, PED_GREEN };
const char* const PHASE_NAMES[] = {"CAR_GREEN", "CAR_YELLOW", "ALL_RED", "PED_GREEN"};

// Culorile unei lămpi, cu numele din mesajul "lights".
enum Light { RED, YELLOW, GREEN };
const char* const LIGHT_NAMES[] = {"RED", "YELLOW", "GREEN"};

// ---------------------------------------------------------------------------
// TODO 0: structura aplicației
// ---------------------------------------------------------------------------

// Prioritățile taskurilor (între 2 și 19). Rate Monotonic pentru cele două
// periodice: controlul (T = 100 ms) este mai prioritar decât ieșirea (T = 500 ms).
// Taskul sporadic are lucru puțin și scurt, deci îl facem cel mai prioritar,
// ca o apăsare să fie înregistrată imediat.
const UBaseType_t PRIO_OUTPUT = 4;
const UBaseType_t PRIO_CONTROL = 5;
const UBaseType_t PRIO_BUTTON = 6;

const uint32_t TASK_STACK_BYTES = 4096;

// STAREA SEMAFORULUI, partajată de taskuri și protejată de g_state_mutex.
struct SemaforState {
    Phase phase;         // faza curentă
    uint32_t entered_ms; // millis() la intrarea în faza curentă
    bool request;        // cererea pietonului (lampa AȘTEPTAȚI)
};

static SemaforState g_state;
static SemaphoreHandle_t g_state_mutex = NULL;

// Elementul cozii butonului: momentul sosirii apăsării, pus de cel care o
// trimite (ISR sau command_received()), în ticks.
struct ButtonPress {
    TickType_t arrival;
};

static QueueHandle_t g_button_queue = NULL;

// ---------------------------------------------------------------------------
// TODO 1: task periodic de control (T = CONTROL_PERIOD_MS)
// ---------------------------------------------------------------------------

// Durata fazei în ms pentru o fază cu durată fixă; CAR_GREEN este tratată separat.
static uint32_t phase_duration_ms(Phase p) {
    switch (p) {
        case CAR_GREEN:
            return CAR_GREEN_MIN_MS;
        case CAR_YELLOW:
            return CAR_YELLOW_MS;
        case ALL_RED:
            return ALL_RED_MS;
        case PED_GREEN:
        default:
            return PED_GREEN_MS;
    }
}

static void control_task(void* arg) {
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();  // r_0

    for (;;) {
        bool changed = false;
        Phase new_phase = CAR_GREEN;

        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        uint32_t now = millis();
        uint32_t elapsed = now - g_state.entered_ms;  // corect și la overflow de millis()
        Phase next = g_state.phase;

        switch (g_state.phase) {
            case CAR_GREEN:
                // se termină doar dacă verdele minim a trecut ȘI există o cerere
                if (elapsed >= CAR_GREEN_MIN_MS && g_state.request) next = CAR_YELLOW;
                break;
            case CAR_YELLOW:
                if (elapsed >= phase_duration_ms(CAR_YELLOW)) next = ALL_RED;
                break;
            case ALL_RED:
                if (elapsed >= phase_duration_ms(ALL_RED)) next = PED_GREEN;
                break;
            case PED_GREEN:
                if (elapsed >= phase_duration_ms(PED_GREEN)) next = CAR_GREEN;
                break;
        }

        if (next != g_state.phase) {
            g_state.phase = next;
            g_state.entered_ms = now;
            if (next == PED_GREEN) g_state.request = false;  // pietonul a primit ce a cerut
            changed = true;
            new_phase = next;
        }
        xSemaphoreGive(g_state_mutex);

        // Serial în afara mutexului
        if (changed) Serial.printf("[control] faza nouă: %s\n", PHASE_NAMES[new_phase]);

        // așteptare absolută: eliberările rămân pe grila r_k = r_0 + kT
        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(CONTROL_PERIOD_MS));
    }
}

// ---------------------------------------------------------------------------
// TODO 2: task periodic de ieșire (T = OUTPUT_PERIOD_MS)
// ---------------------------------------------------------------------------

static void output_task(void* arg) {
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();  // r_0

    for (;;) {
        // copiem starea cu mutexul luat, cât mai scurt
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        SemaforState s = g_state;
        uint32_t now = millis();
        xSemaphoreGive(g_state_mutex);

        // culorile lămpilor din faza curentă
        Light cars, peds;
        switch (s.phase) {
            case CAR_GREEN:
                cars = GREEN;
                peds = RED;
                break;
            case CAR_YELLOW:
                cars = YELLOW;
                peds = RED;
                break;
            case ALL_RED:
                cars = RED;
                peds = RED;
                break;
            case PED_GREEN:
            default:
                cars = RED;
                peds = GREEN;
                break;
        }

        // secundele rămase pentru pietoni, rotunjite în sus; 0 în celelalte faze
        uint32_t seconds = 0;
        if (s.phase == PED_GREEN) {
            uint32_t elapsed = now - s.entered_ms;
            uint32_t remaining = (elapsed < PED_GREEN_MS) ? (PED_GREEN_MS - elapsed) : 0;
            seconds = (remaining + 999) / 1000;
            // controlul poate întârzia cu până la o perioadă schimbarea fazei:
            // cât timp faza e PED_GREEN, nu afișăm 0 (ar însemna "nu se afișează")
            if (seconds == 0) seconds = 1;
        }

        char msg[128];
        snprintf(msg, sizeof(msg), "%s;%lld;%s;%s;%s;%d;%u", DEVICE_ID, (long long)epoch_ms(),
                 PHASE_NAMES[s.phase], LIGHT_NAMES[cars], LIGHT_NAMES[peds], s.request ? 1 : 0,
                 (unsigned)seconds);
        mqtt_publish("lights", msg);

        // LED-ul plăcii: aprins (LOW) cât timp lampa AȘTEPTAȚI e aprinsă
        digitalWrite(LED_PIN, s.request ? LOW : HIGH);

        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(OUTPUT_PERIOD_MS));
    }
}

// ---------------------------------------------------------------------------
// TODO 3: butonul (ISR + task sporadic)
// ---------------------------------------------------------------------------

// Rutina de întrerupere a butonului BOOT: pune apăsarea în coadă și atât.
void IRAM_ATTR button_isr(void) {
    ButtonPress press;
    press.arrival = xTaskGetTickCountFromISR();
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(g_button_queue, &press, &woken);
    portYIELD_FROM_ISR(woken);
}

// Taskul sporadic: eliberat de apăsări, cu interval minim între sosiri.
static void button_task(void* arg) {
    (void)arg;
    bool have_last = false;
    TickType_t last_accepted = 0;

    for (;;) {
        ButtonPress press;
        xQueueReceive(g_button_queue, &press, portMAX_DELAY);

        // momentul sosirii vine din element, nu e citit acum
        if (have_last && pdTICKS_TO_MS(press.arrival - last_accepted) < MIN_INTERARRIVAL_MS) {
            Serial.println("[buton] apăsare respinsă (prea deasă)");
            continue;
        }
        have_last = true;
        last_accepted = press.arrival;

        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        g_state.request = true;  // mai multe apăsări = tot o singură cerere
        xSemaphoreGive(g_state_mutex);

        Serial.println("[buton] apăsare acceptată, cerere înregistrată");
    }
}

// Apelată de clientul MQTT pentru fiecare comandă primită (vezi connection.h).
void command_received(const char* text) {
    Serial.printf("[mqtt] comandă primită: %s\n", text);
    if (strcmp(text, "BUTTON") == 0) {
        ButtonPress press;
        press.arrival = xTaskGetTickCount();
        xQueueSend(g_button_queue, &press, 0);  // fără blocare
    }
    // orice altă comandă se ignoră
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
    Serial.printf("=== tema 03: STUDENT %s, DEVICE_ID %s ===\n", STUDENT, DEVICE_ID);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);  // LED stins
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    wifi_connect();
    time_sync();

    // TODO 0 (a): mutexul și coada, înainte de mqtt_start()
    g_state_mutex = xSemaphoreCreateMutex();
    g_button_queue = xQueueCreate(8, sizeof(ButtonPress));
    g_state.phase = CAR_GREEN;
    g_state.entered_ms = millis();
    g_state.request = false;

    mqtt_start();

    // TODO 0 (b): taskurile, apoi întreruperea
    xTaskCreate(control_task, "control", TASK_STACK_BYTES, NULL, PRIO_CONTROL, NULL);
    xTaskCreate(output_task, "output", TASK_STACK_BYTES, NULL, PRIO_OUTPUT, NULL);
    xTaskCreate(button_task, "button", TASK_STACK_BYTES, NULL, PRIO_BUTTON, NULL);
    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), button_isr, FALLING);
}

void loop() {
    // Totul se face în taskuri: loop() nu are nimic de făcut.
    delay(1000);
}
