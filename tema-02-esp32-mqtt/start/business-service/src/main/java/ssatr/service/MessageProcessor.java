package ssatr.service;

import java.util.concurrent.BlockingQueue;

/**
 * Preia mesajele de un anumit tip ("telemetry", "alarm" sau "status") din coada
 * inbox, le salvează și actualizează dashboard-ul.
 *
 * Formatul mesajelor (câmpuri separate prin ";"):
 *   telemetry:  deviceId;timestamp;temperatura
 *   alarm:      deviceId;timestamp;temperatura;prag
 *   status:     deviceId;timestamp;text
 */
public class MessageProcessor implements Runnable {

    private final String kind;
    private final BlockingQueue<String> inbox;
    private final CsvStorage storage;
    private final Dashboard dashboard;

    public MessageProcessor(String kind, BlockingQueue<String> inbox, CsvStorage storage, Dashboard dashboard) {
        this.kind = kind;
        this.inbox = inbox;
        this.storage = storage;
        this.dashboard = dashboard;
    }

    @Override
    public void run() {
        while (!Thread.currentThread().isInterrupted()) {
            String message;
            try {
                message = inbox.take();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return;
            }

            try {
                process(message.trim());
            } catch (RuntimeException e) {
                // un mesaj greșit nu oprește thread-ul
                System.out.println("[" + kind + "] mesaj invalid ignorat: \"" + message + "\" (" + e.getMessage() + ")");
            }
        }
    }

    private void process(String message) {
        switch (kind) {
            case "telemetry": {
                String[] f = split(message, 3);
                String deviceId = f[0];
                long timestamp = parseTimestamp(f[1]);
                double temperature = parseNumber(f[2], "temperatura");
                storage.append(kind, message);
                dashboard.updateTelemetry(deviceId, timestamp, temperature);
                break;
            }
            case "alarm": {
                String[] f = split(message, 4);
                String deviceId = f[0];
                long timestamp = parseTimestamp(f[1]);
                double temperature = parseNumber(f[2], "temperatura");
                double threshold = parseNumber(f[3], "prag");
                storage.append(kind, message);
                dashboard.updateAlarm(deviceId, timestamp, temperature, threshold);
                System.out.printf("%n*** ALARMĂ %s: %.2f °C peste pragul %.2f ***%n", deviceId, temperature, threshold);
                break;
            }
            case "status": {
                // textul poate conține spații, dar nu ";": limităm împărțirea la 3 câmpuri
                String[] f = split(message, 3);
                String deviceId = f[0];
                long timestamp = parseTimestamp(f[1]);
                String text = f[2];
                storage.append(kind, message);
                dashboard.updateStatus(deviceId, timestamp, text);
                System.out.printf("%n[status] %s: %s%n", deviceId, text);
                break;
            }
            default:
                throw new IllegalStateException("tip de mesaj necunoscut: " + kind);
        }
    }

    /** Împarte mesajul în exact n câmpuri nevide; altfel aruncă excepție. */
    private static String[] split(String message, int n) {
        String[] f = message.split(";", n);
        if (f.length != n) {
            throw new IllegalArgumentException("se așteptau " + n + " câmpuri, am primit " + f.length);
        }
        for (int i = 0; i < n; i++) {
            f[i] = f[i].trim();
            if (f[i].isEmpty()) {
                throw new IllegalArgumentException("câmpul " + (i + 1) + " este gol");
            }
        }
        return f;
    }

    private static long parseTimestamp(String s) {
        try {
            return Long.parseLong(s);
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException("timestamp invalid: " + s);
        }
    }

    private static double parseNumber(String s, String what) {
        try {
            double v = Double.parseDouble(s);
            if (Double.isNaN(v) || Double.isInfinite(v)) {
                throw new NumberFormatException();
            }
            return v;
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException(what + " invalidă: " + s);
        }
    }
}
