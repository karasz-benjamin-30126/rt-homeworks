package ssatr.device;


public record AlarmEvent(double temperature, double threshold, long arrivalNanos) {
}
