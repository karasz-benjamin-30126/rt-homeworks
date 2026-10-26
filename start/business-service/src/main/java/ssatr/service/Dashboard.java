package ssatr.service;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

/**
 * Ultima stare cunoscută a fiecărui device: ultima temperatură, momentul
 * ultimului mesaj, numărul de alarme etc.
 *
 * Este actualizată de thread-urile MessageProcessor și citită de CommandConsole,
 * deci accesul este sincronizat: toate metodele publice sunt synchronized
 * (pe același obiect), iar print() face o copie a stării sub lacăt și o
 * afișează după, ca să nu țină lacătul pe durata scrierii în consolă.
 */
public class Dashboard {

    /** Starea unui device. Se modifică doar sub lacătul Dashboard-ului. */
    private static final class DeviceState {
        final String deviceId;
        Double lastTemperature;      // null până la prima telemetrie
        long lastMessageReceivedAt;  // ora locală (ms) la care a sosit ultimul mesaj
        long lastDeviceTimestamp;    // timestamp-ul din ultimul mesaj, pus de device
        int alarmCount;
        Double lastAlarmThreshold;
        String lastStatus;

        DeviceState(String deviceId) {
            this.deviceId = deviceId;
        }

        DeviceState copy() {
            DeviceState c = new DeviceState(deviceId);
            c.lastTemperature = lastTemperature;
            c.lastMessageReceivedAt = lastMessageReceivedAt;
            c.lastDeviceTimestamp = lastDeviceTimestamp;
            c.alarmCount = alarmCount;
            c.lastAlarmThreshold = lastAlarmThreshold;
            c.lastStatus = lastStatus;
            return c;
        }
    }

    // TreeMap: device-urile apar sortate după ID în tabel
    private final Map<String, DeviceState> devices = new TreeMap<>();

    private DeviceState stateOf(String deviceId) {
        return devices.computeIfAbsent(deviceId, DeviceState::new);
    }

    private void touch(DeviceState s, long deviceTimestamp) {
        s.lastMessageReceivedAt = System.currentTimeMillis();
        s.lastDeviceTimestamp = deviceTimestamp;
    }

    public synchronized void updateTelemetry(String deviceId, long timestamp, double temperature) {
        DeviceState s = stateOf(deviceId);
        s.lastTemperature = temperature;
        touch(s, timestamp);
    }

    public synchronized void updateAlarm(String deviceId, long timestamp, double temperature, double threshold) {
        DeviceState s = stateOf(deviceId);
        s.alarmCount++;
        s.lastAlarmThreshold = threshold;
        touch(s, timestamp);
    }

    public synchronized void updateStatus(String deviceId, long timestamp, String text) {
        DeviceState s = stateOf(deviceId);
        s.lastStatus = text;
        touch(s, timestamp);
    }

    /** true dacă device-ul a trimis cel puțin un mesaj. */
    public synchronized boolean knows(String deviceId) {
        return devices.containsKey(deviceId);
    }

    /** Afișează un tabel cu starea tuturor device-urilor. */
    public void print() {
        List<DeviceState> snapshot = new ArrayList<>();
        synchronized (this) {
            for (DeviceState s : devices.values()) {
                snapshot.add(s.copy());
            }
        }

        if (snapshot.isEmpty()) {
            System.out.println("(niciun device nu a trimis încă mesaje)");
            return;
        }

        long now = System.currentTimeMillis();
        String format = "%-16s %8s %15s %7s %8s  %s%n";
        System.out.printf(format, "DEVICE", "TEMP", "ULTIMUL MESAJ", "ALARME", "PRAG", "ULTIMUL STATUS");
        for (DeviceState s : snapshot) {
            String temp = s.lastTemperature == null ? "-" : String.format("%.2f", s.lastTemperature);
            String age = "acum " + formatAge(now - s.lastMessageReceivedAt);
            String threshold = s.lastAlarmThreshold == null ? "-" : String.format("%.2f", s.lastAlarmThreshold);
            String status = s.lastStatus == null ? "-" : s.lastStatus;
            System.out.printf(format, s.deviceId, temp, age, s.alarmCount, threshold, status);
        }
    }

    private static String formatAge(long ms) {
        long sec = Math.max(0, ms / 1000);
        if (sec < 60) return sec + " s";
        if (sec < 3600) return (sec / 60) + " min " + (sec % 60) + " s";
        return (sec / 3600) + " h " + (sec % 3600 / 60) + " min";
    }
}
