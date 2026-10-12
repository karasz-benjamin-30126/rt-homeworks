package ssatr.service;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;


public class CsvStorage {

    private final Path directory;

    public CsvStorage(String directory) {
        this.directory = Path.of(directory);
    }


    public synchronized void append(String kind, String message) {
        Path file = directory.resolve(kind + ".csv");
        try {
            Files.createDirectories(directory);

            // un fișier nou începe cu rândul de antet
            if (Files.notExists(file)) {
                Files.writeString(file, header(kind) + "\n");
            }

            Files.writeString(file, message + "\n", StandardOpenOption.CREATE, StandardOpenOption.APPEND);
        } catch (IOException e) {
            System.out.println("Eroare la scrierea în " + file + ": " + e.getMessage());
        }
    }

    private static String header(String kind) {
        switch (kind) {
            case "telemetry":
                return "deviceId;timestamp;temperatura";
            case "alarm":
                return "deviceId;timestamp;temperatura;prag";
            case "status":
                return "deviceId;timestamp;text";
            default:
                return "mesaj";
        }
    }
}
