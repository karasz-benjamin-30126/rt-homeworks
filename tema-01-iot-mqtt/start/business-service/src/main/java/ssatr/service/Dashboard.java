package ssatr.service;

import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;


public class Dashboard {

    /** Starea unui device. */
    private static class DeviceState {
        Double lastTemperature;      // null până la prima telemetrie
        long lastMessageMillis;      // momentul primirii ultimului mesaj (orice tip)
        int telemetryCount;
        int alarmCount;
        String lastStatus = "-";
    }

    // TreeMap: device-urile apar sortate după ID în tabel
    private final Map<String, DeviceState> devices = new TreeMap<>();

    public synchronized void updateTelemetry(String deviceId, double temperature) {
        DeviceState s = state(deviceId);
        s.lastTemperature = temperature;
        s.telemetryCount++;
    }

    public synchronized void updateAlarm(String deviceId, double temperature) {
        DeviceState s = state(deviceId);
        s.lastTemperature = temperature;
        s.alarmCount++;
    }

    public synchronized void updateStatus(String deviceId, String text) {
        state(deviceId).lastStatus = text;
    }

    /** Întoarce starea device-ului (o creează la primul mesaj) și marchează momentul primirii. */
    private DeviceState state(String deviceId) {
        DeviceState s = devices.computeIfAbsent(deviceId, id -> new DeviceState());
        s.lastMessageMillis = System.currentTimeMillis();
        return s;
    }

    /** Afișează un tabel cu starea tuturor device-urilor. */
    public synchronized void print() {
        if (devices.isEmpty()) {
            System.out.println("Nu s-a primit încă niciun mesaj.");
            return;
        }
        long now = System.currentTimeMillis();
        String format = "%-12s %8s %15s %8s %8s   %s%n";
        System.out.printf(format, "DEVICE", "TEMP", "ULTIMUL MESAJ", "MESAJE", "ALARME", "ULTIMA CONFIRMARE");
        for (Map.Entry<String, DeviceState> e : devices.entrySet()) {
            DeviceState s = e.getValue();
            String temp = s.lastTemperature == null ? "-" : String.format(Locale.US, "%.2f", s.lastTemperature);
            System.out.printf(format, e.getKey(), temp, age(now - s.lastMessageMillis),
                    s.telemetryCount, s.alarmCount, s.lastStatus);
        }
    }

    private static String age(long ms) {
        long sec = ms / 1000;
        if (sec < 60) {
            return "acum " + sec + " s";
        }
        if (sec < 3600) {
            return "acum " + sec / 60 + " min";
        }
        return "acum " + sec / 3600 + " h";
    }
}
