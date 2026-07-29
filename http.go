package main

import (
	"fmt"
	"io"
	"log"
	"math"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"sync"
	"time"
)

var (
	lastReadingMu sync.Mutex
	lastReading   = math.NaN()
	// lowerRun holds the most recent run of consecutive incr-only decrease
	// rejections. Once it contains ocrResetAfter mutually-consistent readings,
	// the stored floor is treated as an OCR misread and reset. Guarded by
	// lastReadingMu.
	lowerRun []float64
)

func handleOCR(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}

	query := r.URL.Query()
	var batLevel, batVoltage int
	if v := query.Get("bat_level"); v != "" {
		if level, err := strconv.Atoi(v); err == nil && level >= 0 && level <= 100 {
			batLevel = level
			metricBatteryLevel.Set(float64(level))
		}
	}
	if v := query.Get("bat_voltage"); v != "" {
		if voltage, err := strconv.Atoi(v); err == nil && voltage > 0 {
			batVoltage = voltage
			metricBatteryVoltage.Set(float64(voltage))
		}
	}

	imageData, err := io.ReadAll(io.LimitReader(r.Body, 10<<20))
	if err != nil {
		http.Error(w, "failed to read body: "+err.Error(), http.StatusBadRequest)
		return
	}
	defer r.Body.Close()

	log.Printf("OCR request: image_bytes=%d bat_level=%d bat_voltage=%d", len(imageData), batLevel, batVoltage)

	if len(imageData) == 0 {
		http.Error(w, "empty body", http.StatusBadRequest)
		return
	}

	// Respond immediately so the ESP32 can go back to sleep.
	w.WriteHeader(http.StatusAccepted)

	// Process OCR in the background.
	go processOCR(imageData, batLevel, batVoltage)
}

func processOCR(imageData []byte, batLevel, batVoltage int) {
	var cropped, masked bool
	ocrData := imageData
	if cropRect != nil {
		data, err := cropImage(imageData, cropRect)
		if err != nil {
			log.Printf("crop error: %v, using original image", err)
		} else {
			ocrData = data
			cropped = true
		}
	}

	if len(ocrMasks) > 0 {
		data, err := maskImage(ocrData, ocrMasks)
		if err != nil {
			log.Printf("mask error: %v, using unmasked image", err)
		} else {
			ocrData = data
			masked = true
		}
	}

	// Store images to disk before OCR so we have them even if OCR fails.
	imagePath := storeImages(imageData, ocrData, cropped, masked)

	tmpDir, err := os.MkdirTemp("", "ocr-")
	if err != nil {
		log.Printf("failed to create temp dir: %v", err)
		metricOCRErrors.Inc()
		return
	}
	defer os.RemoveAll(tmpDir)

	tmpFile := filepath.Join(tmpDir, "image.jpg")
	if err := os.WriteFile(tmpFile, ocrData, 0644); err != nil {
		log.Printf("failed to write temp file: %v", err)
		metricOCRErrors.Inc()
		return
	}

	start := time.Now()
	ocrOut, err := runOCR(tmpFile)
	elapsed := time.Since(start)
	metricOCRDuration.Observe(elapsed.Seconds())

	if err != nil {
		metricOCRErrors.Inc()
		log.Printf("ocr error: %v", err)
		return
	}

	reading := extractReading(ocrOut.Texts, ocrMatchRe, ocrFixRules, ocrMergeTexts)

	if reading == "" {
		log.Printf("OCR completed in %s: no reading found, texts=%v", elapsed, ocrOut.Texts)
		return
	}

	val, err := strconv.ParseFloat(reading, 64)
	if err != nil {
		storeReading(imagePath, reading, false)
		log.Printf("OCR completed in %s: invalid reading %q, texts=%v", elapsed, reading, ocrOut.Texts)
		return
	}

	divided := val / meterDivisor

	if ocrIncrOnly || ocrMaxIncr > 0 {
		lastReadingMu.Lock()
		prev := lastReading
		result, reason := checkReadingFilter(divided, prev, ocrIncrOnly, ocrMaxIncr)

		reset := false
		if result == filterRejectDecrease {
			// A single spurious-high misread poisons the incr-only floor, so a
			// sustained run of consistent lower readings means the floor itself
			// was wrong. Recover by adopting the newest reading as the floor.
			if ocrResetAfter > 0 && pushLowerRun(divided, ocrResetAfter, ocrResetTol) {
				reset = true
			}
		} else {
			lowerRun = nil
		}

		if result != filterAccept && !reset {
			lastReadingMu.Unlock()
			storeReading(imagePath, reading, false)
			log.Printf("%s", reason)
			return
		}

		if reset {
			log.Printf("OCR incr-only: floor %.3f appears to be a misread after %d consistent lower readings; resetting floor to %.3f", prev, ocrResetAfter, divided)
			lowerRun = nil
		}
		lastReading = divided
		lastReadingMu.Unlock()
	}

	storeReading(imagePath, reading, true)
	metricMeterReading.Set(val)

	if mqttBroker != "" {
		publishReading(divided, batLevel, batVoltage)
	}

	log.Printf("OCR completed in %s: reading=%s (%.3f m³) texts=%v", elapsed, reading, divided, ocrOut.Texts)
}

// filterResult classifies the outcome of checkReadingFilter so the caller can
// distinguish a decrease rejection (recoverable) from a too-high rejection.
type filterResult int

const (
	filterAccept filterResult = iota
	filterRejectDecrease
	filterRejectTooHigh
)

// checkReadingFilter classifies a reading and returns a reason string for the
// rejected cases. prev may be NaN for the first reading.
func checkReadingFilter(divided, prev float64, incrOnly bool, maxIncr float64) (filterResult, string) {
	if math.IsNaN(prev) {
		return filterAccept, ""
	}
	if incrOnly && divided < prev {
		return filterRejectDecrease, fmt.Sprintf("OCR incr-only: discarding reading %.3f < previous %.3f", divided, prev)
	}
	if maxIncr > 0 && divided-prev > maxIncr {
		return filterRejectTooHigh, fmt.Sprintf("OCR max-incr: discarding reading %.3f, increase %.3f > max %.3f (previous %.3f)", divided, divided-prev, maxIncr, prev)
	}
	return filterAccept, ""
}

// pushLowerRun records a decrease-rejected reading and reports whether the
// stored floor should be treated as a misread. It keeps a sliding window of the
// last n readings and returns true once that window is full and its values span
// no more than tol (i.e. they are mutually consistent). Caller holds lastReadingMu.
func pushLowerRun(divided float64, n int, tol float64) bool {
	lowerRun = append(lowerRun, divided)
	if len(lowerRun) > n {
		lowerRun = lowerRun[len(lowerRun)-n:]
	}
	if len(lowerRun) < n {
		return false
	}
	lo, hi := lowerRun[0], lowerRun[0]
	for _, v := range lowerRun {
		if v < lo {
			lo = v
		}
		if v > hi {
			hi = v
		}
	}
	return hi-lo <= tol
}

func handleHealth(w http.ResponseWriter, r *http.Request) {
	w.WriteHeader(http.StatusOK)
	w.Write([]byte("ok"))
}