package ssatr.device;

import java.util.Locale;
import java.util.concurrent.BlockingQueue;


public class AperiodicCommandTask implements Runnable {


    static final long MIN_PERIOD_MS = 100;
    static final long MAX_PERIOD_MS = 60_000;

    private final String deviceId;
    private final TemperatureSensor sensor;
    private final MqttConnection mqtt;
    private final BlockingQueue<String> commands;
    private final PeriodicTelemetryTask periodic;

    public AperiodicCommandTask(String deviceId, TemperatureSensor sensor, MqttConnection mqtt,
                                BlockingQueue<String> commands, PeriodicTelemetryTask periodic) {
        this.deviceId = deviceId;
        this.sensor = sensor;
        this.mqtt = mqtt;
        this.commands = commands;
        this.periodic = periodic;
    }

    @Override
    public void run() {
        try {
            while (!Thread.currentThread().isInterrupted()) {
                String command = commands.take().trim();   // eliberarea unui job
                long release = System.nanoTime();

                String reply;
                try {
                    reply = execute(command);
                } catch (IllegalArgumentException e) {
                    // comandă greșită: raportăm eroarea, taskul continuă
                    reply = "ERROR " + e.getMessage();
                }

                String message = deviceId + ";" + System.currentTimeMillis() + ";" + reply;
                mqtt.publish(Config.statusTopic(deviceId), message);

                long responseMs = (System.nanoTime() - release) / 1_000_000;
                System.out.printf("[aperiodic] \"%s\" -> %s  (R = %d ms, în coadă: %d)%n",
                        command, reply, responseMs, commands.size());
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    /** Execută o comandă și întoarce textul confirmării. */
    private String execute(String command) {
        if (command.isEmpty()) {
            throw new IllegalArgumentException("comandă goală");
        }
        String[] parts = command.split("\\s+");
        String name = parts[0].toUpperCase(Locale.ROOT);

        switch (name) {
            case "SET_INTERVAL": {
                requireArgs(parts, 1, "SET_INTERVAL <ms>");
                long ms;
                try {
                    ms = Long.parseLong(parts[1]);
                } catch (NumberFormatException e) {
                    throw new IllegalArgumentException("SET_INTERVAL: valoare invalidă '" + parts[1] + "'");
                }
                if (ms < MIN_PERIOD_MS || ms > MAX_PERIOD_MS) {
                    throw new IllegalArgumentException("SET_INTERVAL: perioada trebuie să fie între "
                            + MIN_PERIOD_MS + " și " + MAX_PERIOD_MS + " ms");
                }
                periodic.setPeriodMs(ms);
                return "OK SET_INTERVAL " + ms;
            }
            case "SET_THRESHOLD": {
                requireArgs(parts, 1, "SET_THRESHOLD <valoare>");
                double value;
                try {
                    value = Double.parseDouble(parts[1]);
                } catch (NumberFormatException e) {
                    throw new IllegalArgumentException("SET_THRESHOLD: valoare invalidă '" + parts[1] + "'");
                }
                if (!Double.isFinite(value)) {
                    throw new IllegalArgumentException("SET_THRESHOLD: valoare invalidă '" + parts[1] + "'");
                }
                periodic.setThreshold(value);
                return String.format(Locale.US, "OK SET_THRESHOLD %.2f", value);
            }
            case "RESET":
                requireArgs(parts, 0, "RESET");
                sensor.reset();
                return "OK RESET";
            case "STATUS":
                requireArgs(parts, 0, "STATUS");
                return String.format(Locale.US, "OK STATUS interval=%d threshold=%.2f",
                        periodic.getPeriodMs(), periodic.getThreshold());
            default:
                throw new IllegalArgumentException("comandă necunoscută '" + parts[0] + "'");
        }
    }

    private static void requireArgs(String[] parts, int expected, String usage) {
        if (parts.length - 1 != expected) {
            throw new IllegalArgumentException("format greșit, se așteaptă: " + usage);
        }
    }
}
