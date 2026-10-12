package ssatr.service;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.concurrent.BlockingQueue;


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

            // fiecare mesaj se salvează exact cum a sosit
            storage.append(kind, message);

            try {
                process(message);
            } catch (RuntimeException e) {
                // un mesaj greșit nu oprește thread-ul
                System.out.println("[" + kind + "] mesaj invalid ignorat: \"" + message + "\" (" + e.getMessage() + ")");
            }
        }
    }

    private void process(String message) {
        switch (kind) {
            case "telemetry": {
                String[] f = fields(message, 3);
                dashboard.updateTelemetry(f[0], Double.parseDouble(f[2]));
                break;
            }
            case "alarm": {
                String[] f = fields(message, 4);
                double temperature = Double.parseDouble(f[2]);
                double threshold = Double.parseDouble(f[3]);
                dashboard.updateAlarm(f[0], temperature);
                System.out.printf("%n!!! ALARMĂ %s la %s: temperatura %.2f > prag %.2f%n",
                        f[0], time(f[1]), temperature, threshold);
                break;
            }
            case "status": {
                // textul poate conține ";" doar teoretic; limităm split-ul la 3 câmpuri
                String[] f = fields(message, 3);
                dashboard.updateStatus(f[0], f[2]);
                System.out.printf("%n<<< %s la %s: %s%n", f[0], time(f[1]), f[2]);
                break;
            }
            default:
                throw new IllegalStateException("tip de mesaj necunoscut: " + kind);
        }
    }

    /** Desparte mesajul în exact n câmpuri; aruncă excepție dacă formatul nu e corect. */
    private static String[] fields(String message, int n) {
        String[] f = message.split(";", n);
        if (f.length != n || f[0].isBlank()) {
            throw new IllegalArgumentException("se așteaptă " + n + " câmpuri separate prin ;");
        }
        Long.parseLong(f[1]);   // verifică timestamp-ul
        return f;
    }

    private static String time(String timestamp) {
        return new SimpleDateFormat("HH:mm:ss").format(new Date(Long.parseLong(timestamp)));
    }
}
