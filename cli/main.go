package main

import (
	"bufio"
	"bytes"
	"context"
	_ "embed"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func run() error {
	elf := flag.String("elf", "blink.elf", "ELF file matching the firmware on the board")
	gdb := flag.String("gdb", "gdb", "ARM-capable GDB executable")
	openocd := flag.String("openocd", "openocd", "OpenOCD executable")
	config := flag.String("config", "stm32f3.cfg", "OpenOCD configuration file")
	address := flag.String("address", "localhost:3333", "GDB server address")
	listen := flag.String("listen", "127.0.0.1:8080", "HTTP listen address")
	existing := flag.Bool("existing-server", false, "connect to an already running GDB server")
	flag.Parse()

	if _, err := os.Stat(*elf); err != nil {
		return fmt.Errorf("ELF file: %w", err)
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	if !*existing {
		cmd, err := startOpenOCD(ctx, *openocd, *config)
		if err != nil {
			return err
		}
		defer func() {
			_ = cmd.Process.Kill()
			_ = cmd.Wait()
		}()
	}

	listener, err := net.Listen("tcp", *listen)
	if err != nil {
		return fmt.Errorf("start web server: %w", err)
	}
	store := &measurements{}
	done := make(chan struct{})
	go func() {
		defer close(done)
		for {
			reading, err := readMeasurements(ctx, *gdb, *elf, *address)
			if ctx.Err() != nil {
				return
			}
			store.update(reading, err)
			if err == nil {
				fmt.Printf("Temperature ADC: %d  VREFINT ADC: %d\n", reading.LastTempADC, reading.LastVrefADC)
			}
			// GDB resumes the target after reading, allowing it to run and
			// update both values before the next connection halts it again.
			timer := time.NewTimer(100 * time.Millisecond)
			select {
			case <-ctx.Done():
				timer.Stop()
				return
			case <-timer.C:
			}
		}
	}()
	defer func() { stop(); <-done }()

	mux := http.NewServeMux()
	mux.HandleFunc("GET /{$}", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		fmt.Fprint(w, page)
	})
	mux.HandleFunc("GET /api/measurements", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		_ = json.NewEncoder(w).Encode(store.snapshot())
	})
	server := &http.Server{Handler: mux, ReadHeaderTimeout: 5 * time.Second}
	errs := make(chan error, 1)
	go func() { errs <- server.Serve(listener) }()
	fmt.Printf("Open http://%s/\n", listener.Addr())
	select {
	case <-ctx.Done():
		shutdown, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		return server.Shutdown(shutdown)
	case err := <-errs:
		return fmt.Errorf("web server: %w", err)
	}
}

type vector struct {
	X int16 `json:"x"`
	Y int16 `json:"y"`
	Z int16 `json:"z"`
}

type reading struct {
	LastTempADC  uint32   `json:"last_temp_adc"`
	LastVrefADC  uint32   `json:"last_vref_adc"`
	TemperatureC *float64 `json:"temperature_c"`
	VDDA         *float64 `json:"vdda_v"`
	Gyro         *vector  `json:"gyro"`
	Accel        *vector  `json:"accel"`
	Mag          *vector  `json:"mag"`
	GyroID       uint32   `json:"gyro_id"`
	MagType      uint32   `json:"mag_type"`
}

type snapshot struct {
	Reading *reading  `json:"reading"`
	Error   string    `json:"error"`
	Updated time.Time `json:"updated"`
}

type measurements struct {
	mu    sync.RWMutex
	value snapshot
}

func (m *measurements) update(value *reading, err error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if err != nil {
		m.value.Error = err.Error()
		return
	}
	m.value = snapshot{Reading: value, Updated: time.Now()}
}

func (m *measurements) snapshot() snapshot {
	m.mu.RLock()
	defer m.mu.RUnlock()
	return m.value
}

//go:embed index.html
var page string

func startOpenOCD(ctx context.Context, executable, config string) (*exec.Cmd, error) {
	cmd := exec.Command(executable, "-f", config)
	pipe, err := cmd.StderrPipe()
	if err != nil {
		return nil, err
	}
	cmd.Stdout = io.Discard
	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("start OpenOCD: %w", err)
	}
	ready := make(chan error, 1)
	go func() {
		scanner := bufio.NewScanner(pipe)
		announced := false
		for scanner.Scan() {
			line := scanner.Text()
			if !announced {
				fmt.Fprintln(os.Stderr, line)
			}
			if !announced && strings.Contains(line, "Listening on port") && strings.Contains(line, "for gdb connections") {
				announced = true
				ready <- nil
			}
		}
		if !announced {
			ready <- fmt.Errorf("OpenOCD exited before its GDB server was ready: %v", scanner.Err())
		}
	}()
	select {
	case err = <-ready:
	case <-ctx.Done():
		err = ctx.Err()
	case <-time.After(10 * time.Second):
		err = fmt.Errorf("timed out waiting for OpenOCD")
	}
	if err != nil {
		_ = cmd.Process.Kill()
		_ = cmd.Wait()
		return nil, err
	}
	return cmd, nil
}

func readMeasurements(parent context.Context, gdb, elf, address string) (*reading, error) {
	ctx, cancel := context.WithTimeout(parent, 10*time.Second)
	defer cancel()
	// STM32F303xB/xC factory calibration words, measured at VDDA = 3.3 V.
	command := `printf "ADC_DATA %u %u %u %u %u\n", last_temp_adc, last_vref_adc, *(unsigned short *)0x1ffff7b8, *(unsigned short *)0x1ffff7ba, *(unsigned short *)0x1ffff7c2`
	motion := `printf "MOTION_DATA %u %u %u %d %d %d %d %d %d %d %d %d\n", sensor_status, gyro_id, mag_type, last_gyro.x, last_gyro.y, last_gyro.z, last_accel.x, last_accel.y, last_accel.z, last_mag.x, last_mag.y, last_mag.z`
	args := []string{"--nx", "--quiet", "--batch", elf,
		"-ex", "set pagination off", "-ex", "target extended-remote " + address,
		"-ex", command, "-ex", motion, "-ex", "monitor resume", "-ex", "detach"}
	cmd := exec.CommandContext(ctx, gdb, args...)
	var output bytes.Buffer
	cmd.Stdout, cmd.Stderr = &output, &output
	if err := cmd.Run(); err != nil {
		return nil, fmt.Errorf("GDB failed: %w\n%s", err, output.String())
	}
	r, err := parseReading(output.String())
	if err != nil {
		return nil, err
	}
	if err := parseMotion(output.String(), r); err != nil {
		return nil, err
	}
	return r, nil
}

func parseReading(output string) (*reading, error) {
	for line := range strings.SplitSeq(output, "\n") {
		if !strings.HasPrefix(line, "ADC_DATA ") {
			continue
		}
		fields := strings.Fields(line)
		if len(fields) != 6 {
			return nil, fmt.Errorf("invalid GDB measurement: expected 5 values, got %d", len(fields)-1)
		}
		values := [5]uint32{}
		for i, field := range fields[1:] {
			v, err := strconv.ParseUint(field, 10, 16)
			if err != nil {
				return nil, fmt.Errorf("invalid GDB measurement value %q: %w", field, err)
			}
			values[i] = uint32(v)
		}
		if values[0] > 4095 || values[1] > 4095 {
			return nil, fmt.Errorf("invalid 12-bit ADC measurement")
		}
		r := &reading{LastTempADC: values[0], LastVrefADC: values[1]}
		cal1, vrefCal, cal2 := values[2], values[3], values[4]
		if values[1] != 0 && vrefCal > 0 && vrefCal <= 4095 {
			vdda := 3.3 * float64(vrefCal) / float64(values[1])
			r.VDDA = &vdda
			if cal1 > 0 && cal1 <= 4095 && cal2 > 0 && cal2 <= 4095 && cal1 != cal2 {
				tempAtCalibrationVoltage := float64(values[0]) * vdda / 3.3
				temp := 30 + (tempAtCalibrationVoltage-float64(cal1))*80/(float64(cal2)-float64(cal1))
				r.TemperatureC = &temp
			}
		}
		return r, nil
	}
	return nil, fmt.Errorf("GDB returned no measurement record: %s", output)
}

func parseMotion(output string, r *reading) error {
	for line := range strings.SplitSeq(output, "\n") {
		if !strings.HasPrefix(line, "MOTION_DATA ") {
			continue
		}
		fields := strings.Fields(line)
		if len(fields) != 13 {
			return fmt.Errorf("invalid motion measurement: expected 12 values, got %d", len(fields)-1)
		}
		meta := [3]uint64{}
		for i := range meta {
			v, err := strconv.ParseUint(fields[i+1], 10, 32)
			if err != nil {
				return fmt.Errorf("invalid motion status %q: %w", fields[i+1], err)
			}
			meta[i] = v
		}
		var axes [9]int16
		for i := range axes {
			v, err := strconv.ParseInt(fields[i+4], 10, 16)
			if err != nil {
				return fmt.Errorf("invalid motion axis %q: %w", fields[i+4], err)
			}
			axes[i] = int16(v)
		}
		r.GyroID, r.MagType = uint32(meta[1]), uint32(meta[2])
		if meta[0]&1 != 0 {
			r.Gyro = &vector{axes[0], axes[1], axes[2]}
		}
		if meta[0]&2 != 0 {
			r.Accel = &vector{axes[3], axes[4], axes[5]}
		}
		if meta[0]&4 != 0 {
			r.Mag = &vector{axes[6], axes[7], axes[8]}
		}
		return nil
	}
	return fmt.Errorf("GDB returned no motion measurement record")
}
