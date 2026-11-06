package main

import (
	"math"
	"testing"
)

func TestParseReading(t *testing.T) {
	// VDDA is 3.0 V; a temperature ADC of 1100 becomes 1000 at 3.3 V.
	got, err := parseReading("GDB connected\nADC_DATA 1100 1650 1000 1500 1300\nDetached\n")
	if err != nil {
		t.Fatal(err)
	}
	if got.LastTempADC != 1100 || got.LastVrefADC != 1650 || got.VDDA == nil || math.Abs(*got.VDDA-3.0) > 0.0001 || got.TemperatureC == nil || math.Abs(*got.TemperatureC-30) > 0.0001 {
		t.Fatalf("unexpected reading: %+v", got)
	}
	for _, invalid := range []string{"no record", "ADC_DATA 1", "ADC_DATA 1 2 3", "ADC_DATA bad 2 3 4 5", "ADC_DATA 4096 2 3 4 5"} {
		if _, err := parseReading(invalid); err == nil {
			t.Errorf("accepted invalid record %q", invalid)
		}
	}
}

func TestInvalidCalibration(t *testing.T) {
	for _, record := range []string{"ADC_DATA 1000 0 1000 1500 1300", "ADC_DATA 1000 1500 1000 65535 1300"} {
		got, err := parseReading(record)
		if err != nil || got.VDDA != nil || got.TemperatureC != nil {
			t.Fatalf("expected unavailable estimates for %q: %+v, %v", record, got, err)
		}
	}
	got, err := parseReading("ADC_DATA 1000 1500 1000 1500 1000")
	if err != nil || got.VDDA == nil || got.TemperatureC != nil {
		t.Fatalf("expected only VDDA for equal temperature calibration points: %+v, %v", got, err)
	}
}

func TestParseMotion(t *testing.T) {
	var r reading
	err := parseMotion("MOTION_DATA 5 212 1 -32768 0 32767 1 2 3 -4 5 -6\n", &r)
	if err != nil || r.Gyro == nil || r.Gyro.X != -32768 || r.Gyro.Z != 32767 || r.Accel != nil || r.Mag == nil || r.Mag.X != -4 || r.GyroID != 212 || r.MagType != 1 {
		t.Fatalf("unexpected motion: %+v, %v", r, err)
	}
	for _, record := range []string{"", "MOTION_DATA 7 1", "MOTION_DATA 7 1 1 0 0 0 0 0 0 0 0 32768"} {
		if err := parseMotion(record, &reading{}); err == nil {
			t.Errorf("accepted invalid motion record %q", record)
		}
	}
}
