package ssatr.device;

import java.util.Locale;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.TimeUnit;


public class PeriodicTelemetryTask implements Runnable {

    private final String deviceId;
    private final TemperatureSensor sensor;
    private final MqttConnection mqtt;
    private final BlockingQueue<AlarmEvent> alarmEvents;

    // volatile: sunt citite aici, dar modificate de taskul aperiodic (alt thread)
    private volatile long periodMs = 2000;
    private volatile double threshold = 28.0;

    public PeriodicTelemetryTask(String deviceId, TemperatureSensor sensor, MqttConnection mqtt,
                                 BlockingQueue<AlarmEvent> alarmEvents) {
        this.deviceId = deviceId;
        this.sensor = sensor;
        this.mqtt = mqtt;
        this.alarmEvents = alarmEvents;
    }

    @Override
    public void run() {
        // r_0: prima eliberare este acum; grila folosește un timp monoton
        long nextRelease = System.nanoTime();
        long k = 0;

        try {
            while (!Thread.currentThread().isInterrupted()) {
                // așteptare ABSOLUTĂ până la eliberarea r_k de pe grilă
                long remaining = nextRelease - System.nanoTime();
                if (remaining > 0) {
                    TimeUnit.NANOSECONDS.sleep(remaining);
                }

                long start = System.nanoTime();
                double latenessMs = (start - nextRelease) / 1_000_000.0;

                // ---- jobul k ----
                double temperature = sensor.read();
                double currentThreshold = threshold;   // o singură citire a valorii volatile
                String message = String.format(Locale.US, "%s;%d;%.2f",
                        deviceId, System.currentTimeMillis(), temperature);
                mqtt.publish(Config.telemetryTopic(deviceId), message);

                if (temperature > currentThreshold) {
                    // doar semnalăm; alarma este tratată de taskul sporadic
                    alarmEvents.put(new AlarmEvent(temperature, currentThreshold, System.nanoTime()));
                }

                long responseMs = (System.nanoTime() - nextRelease) / 1_000_000;
                System.out.printf(Locale.US, "[periodic]  job #%d  T = %d ms  L = %.2f ms  R = %d ms  temp = %.2f%s%n",
                        k, periodMs, latenessMs, responseMs, temperature,
                        temperature > currentThreshold ? "  > prag " + currentThreshold : "");

                // ---- următoarea eliberare de pe grilă ----
                // periodMs se citește la fiecare job, deci SET_INTERVAL are efect
                // începând cu eliberarea următoare
                long periodNanos = periodMs * 1_000_000L;
                nextRelease += periodNanos;
                k++;

                // depășire: dacă jobul a terminat după una sau mai multe eliberări,
                // nu le executăm pe toate în rafală, ci sărim la următoarea
                // eliberare viitoare, rămânând pe aceeași grilă
                long now = System.nanoTime();
                if (now - nextRelease >= periodNanos) {
                    long missed = (now - nextRelease) / periodNanos;
                    nextRelease += missed * periodNanos;
                    k += missed;
                    System.out.printf("[periodic]  depășire: %d eliberări sărite%n", missed);
                }
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    public void setPeriodMs(long periodMs) {
        this.periodMs = periodMs;
    }

    public void setThreshold(double threshold) {
        this.threshold = threshold;
    }

    public long getPeriodMs() {
        return periodMs;
    }

    public double getThreshold() {
        return threshold;
    }
}
