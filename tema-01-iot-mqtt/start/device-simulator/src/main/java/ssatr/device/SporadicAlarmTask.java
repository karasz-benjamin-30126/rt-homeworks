package ssatr.device;

import java.util.Locale;
import java.util.concurrent.BlockingQueue;


public class SporadicAlarmTask implements Runnable {

    static final long MIN_INTERARRIVAL_MS = 5000;


    static final long DEADLINE_MS = 500;

    private final String deviceId;
    private final MqttConnection mqtt;
    private final BlockingQueue<AlarmEvent> alarmEvents;

    public SporadicAlarmTask(String deviceId, MqttConnection mqtt, BlockingQueue<AlarmEvent> alarmEvents) {
        this.deviceId = deviceId;
        this.mqtt = mqtt;
        this.alarmEvents = alarmEvents;
    }

    @Override
    public void run() {
        boolean released = false;     // a existat deja o eliberare?
        long lastReleaseNanos = 0;

        try {
            while (!Thread.currentThread().isInterrupted()) {
                AlarmEvent event = alarmEvents.take();   // blocat până sosește un eveniment

                long sinceLastMs = (event.arrivalNanos() - lastReleaseNanos) / 1_000_000;
                if (released && sinceLastMs < MIN_INTERARRIVAL_MS) {
                    System.out.printf(Locale.US,
                            "[sporadic]  alarmă RESPINSĂ (temp = %.2f): au trecut doar %d ms, interval minim %d ms%n",
                            event.temperature(), sinceLastMs, MIN_INTERARRIVAL_MS);
                    continue;
                }

                // eliberare acceptată
                released = true;
                lastReleaseNanos = event.arrivalNanos();

                String message = String.format(Locale.US, "%s;%d;%.2f;%.2f",
                        deviceId, System.currentTimeMillis(), event.temperature(), event.threshold());
                mqtt.publish(Config.alarmTopic(deviceId), message);

                long responseMs = (System.nanoTime() - event.arrivalNanos()) / 1_000_000;
                System.out.printf(Locale.US, "[sporadic]  ALARMĂ publicată: temp = %.2f > prag = %.2f  R = %d ms -> %s (D = %d ms)%n",
                        event.temperature(), event.threshold(), responseMs,
                        responseMs <= DEADLINE_MS ? "termen respectat" : "TERMEN RATAT", DEADLINE_MS);
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }
}
