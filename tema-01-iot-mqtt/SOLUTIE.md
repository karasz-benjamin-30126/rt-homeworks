# Tema 01 – Soluție

## Rulare

Înainte de rulare, înlocuiți `STUDENT = "popescu-ion"` cu numele vostru în **ambele** fișiere `Config.java`
(`start/device-simulator/.../Config.java` și `start/business-service/.../Config.java`).

```bash
cd start/business-service && mvn -q compile exec:java
cd start/device-simulator && mvn -q compile exec:java -Dexec.args="device-1"
cd start/device-simulator && mvn -q compile exec:java -Dexec.args="device-2"
```

În consola serviciului: `show`, `device-1 SET_INTERVAL 1000`, `device-1 SET_THRESHOLD 25`, `device-1 RESET`, `device-1 STATUS`.

## Ce s-a implementat

### Device simulator

| TODO | Clasă | Implementare |
|------|-------|--------------|
| 1 | `PeriodicTelemetryTask` | Buclă cu **așteptare absolută** pe grila $r_k = r_0 + kT$, cu `System.nanoTime()`: după fiecare job se calculează `next += T` și se așteaptă doar timpul rămas până la `next`. Astfel C și întârzierile nu se adună, deci nu apare derivă. `periodMs` se citește la fiecare job, deci `SET_INTERVAL` are efect de la eliberarea următoare. Dacă un job depășește una sau mai multe perioade, eliberările ratate sunt sărite, nu executate în rafală, iar taskul rămâne pe aceeași grilă. Pentru fiecare job se afișează întârzierea L și timpul de răspuns R. |
| 2 | `SporadicAlarmTask` | Preia evenimentele cu `take()`. Intervalul minim între sosiri se măsoară între **momentele de sosire** ale evenimentelor, pe ceas monoton, nu între momentele preluării din coadă. Evenimentele care sosesc la mai puțin de `MIN_INTERARRIVAL_MS` = 5000 ms după ultima eliberare sunt respinse. Pentru jobul acceptat se verifică $R \le D$, cu D = 500 ms. |
| 3 | `AperiodicCommandTask` | Execută comenzile FIFO. Validează formatul, numărul de parametri, valorile numerice și intervalul `100 ≤ T ≤ 60000` ms. Orice eroare produce `ERROR <motiv>` pe `status`, iar taskul continuă. `STATUS` răspunde cu `OK STATUS interval=… threshold=…`. |

**Alegerea pentru prag (cerută în TODO 2).** Am înlocuit `BlockingQueue<Double>` cu `BlockingQueue<AlarmEvent>`, unde `AlarmEvent` este un `record(temperature, threshold, arrivalNanos)`. Pragul este transmis **împreună cu temperatura**, din momentul detecției, și nu citit ulterior din `PeriodicTelemetryTask`. Motivul: dacă între detecție și publicare sosește un `SET_THRESHOLD`, alarma ar putea conține un prag mai mare decât temperatura, deci un mesaj inconsistent. În plus, `arrivalNanos` permite măsurarea corectă a intervalului minim între sosiri și a timpului de răspuns R.

### Business service

| TODO | Clasă | Implementare |
|------|-------|--------------|
| 4 | `MessageProcessor` | Fiecare mesaj se salvează în CSV exact cum a sosit, apoi se parsează și se actualizează dashboard-ul. Alarmele (`!!! ALARMĂ …`) și confirmările (`<<< …`) se afișează imediat. Un mesaj invalid este raportat și ignorat, iar thread-ul continuă. |
| 5 | `Dashboard` | `TreeMap<deviceId, DeviceState>`, cu toate metodele `synchronized`, pentru că este scris de 3 thread-uri și citit de consolă. Coloanele afișate: ultima temperatură, vechimea ultimului mesaj (după ceasul serviciului, independent de ceasul device-ului), numărul de mesaje, numărul de alarme și ultima confirmare. |
| 6 | `CommandConsole` | Linia se desparte în `deviceId` și restul comenzii. Un ID care conține `/`, `+` sau `#` este respins, pentru că ar strica topicul. Comanda se publică pe `commandTopic(deviceId)`, iar validarea parametrilor o face device-ul. |

### Diagrame (`diagrame/`)

| Fișier | Conținut |
|--------|----------|
| `diagrame.drawio` (tab-urile „Diagrama de componente” și „Diagrama de timp”) | sursele editabile |
| `diagrama-de-componente.png` | cele două aplicații cu clasele lor, brokerul care oferă interfața MQTT și dependențele interne |
| `diagrama-de-secventa-alarma.mmd` / `.png` | telemetrie peste prag → eveniment în coadă → alarmă publicată sau respinsă |
| `diagrama-de-secventa-set-interval.mmd` / `.png` | operator → `SET_INTERVAL` → device → confirmare `status` |
| `diagrama-de-timp.png` | 10 s: eliberările lui τ1 (T = 1 s), alarma acceptată la 2,12 s, cea respinsă la 3,12 s, cea acceptată la 7,12 s și un job aperiodic la 5,40 s |
