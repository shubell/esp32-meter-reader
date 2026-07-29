package main

import (
	"math"
	"testing"
)

func TestCheckReadingFilter(t *testing.T) {
	tests := []struct {
		name     string
		divided  float64
		prev     float64
		incrOnly bool
		maxIncr  float64
		want     filterResult
	}{
		// --- First reading (prev is NaN) always accepted ---
		{
			name:     "first reading accepted with incr-only",
			divided:  100,
			prev:     math.NaN(),
			incrOnly: true,
			want:     filterAccept,
		},
		{
			name:    "first reading accepted with max-incr",
			divided: 100,
			prev:    math.NaN(),
			maxIncr: 10,
			want:    filterAccept,
		},

		// --- incr-only tests ---
		{
			name:     "incr-only accepts equal reading",
			divided:  100,
			prev:     100,
			incrOnly: true,
			want:     filterAccept,
		},
		{
			name:     "incr-only accepts higher reading",
			divided:  101,
			prev:     100,
			incrOnly: true,
			want:     filterAccept,
		},
		{
			name:     "incr-only rejects lower reading",
			divided:  99,
			prev:     100,
			incrOnly: true,
			want:     filterRejectDecrease,
		},

		// --- max-incr tests ---
		{
			name:    "max-incr accepts increase within limit",
			divided: 105,
			prev:    100,
			maxIncr: 10,
			want:    filterAccept,
		},
		{
			name:    "max-incr accepts increase exactly at limit",
			divided: 110,
			prev:    100,
			maxIncr: 10,
			want:    filterAccept,
		},
		{
			name:    "max-incr rejects increase above limit",
			divided: 111,
			prev:    100,
			maxIncr: 10,
			want:    filterRejectTooHigh,
		},
		{
			name:    "max-incr accepts decrease (not its job)",
			divided: 90,
			prev:    100,
			maxIncr: 10,
			want:    filterAccept,
		},
		{
			name:    "max-incr accepts equal reading",
			divided: 100,
			prev:    100,
			maxIncr: 10,
			want:    filterAccept,
		},

		// --- both flags combined ---
		{
			name:     "both flags: normal increase accepted",
			divided:  105,
			prev:     100,
			incrOnly: true,
			maxIncr:  10,
			want:     filterAccept,
		},
		{
			name:     "both flags: decrease rejected by incr-only",
			divided:  95,
			prev:     100,
			incrOnly: true,
			maxIncr:  10,
			want:     filterRejectDecrease,
		},
		{
			name:     "both flags: large increase rejected by max-incr",
			divided:  200,
			prev:     100,
			incrOnly: true,
			maxIncr:  50,
			want:     filterRejectTooHigh,
		},

		// --- neither flag (should never reject) ---
		{
			name:    "no flags: everything accepted",
			divided: 50,
			prev:    100,
			want:    filterAccept,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got, reason := checkReadingFilter(tt.divided, tt.prev, tt.incrOnly, tt.maxIncr)
			if got != tt.want {
				t.Errorf("checkReadingFilter = %d (%q), want %d", got, reason, tt.want)
			}
			if (got == filterAccept) != (reason == "") {
				t.Errorf("reason %q inconsistent with result %d", reason, got)
			}
		})
	}
}

func TestPushLowerRun(t *testing.T) {
	tests := []struct {
		name    string
		n       int
		tol     float64
		seq     []float64
		want    []bool // expected return for each reading in seq
		wantLen int    // expected len(lowerRun) after the sequence
	}{
		{
			name: "fewer than n never resets",
			n:    3,
			tol:  0.1,
			seq:  []float64{50, 50},
			want: []bool{false, false},
			// window not yet full
			wantLen: 2,
		},
		{
			name: "n consistent readings reset on the nth",
			n:    3,
			tol:  0.1,
			seq:  []float64{50.00, 50.05, 50.08},
			want: []bool{false, false, true},
			// window is full (n) at the point of reset
			wantLen: 3,
		},
		{
			name: "spread beyond tolerance does not reset",
			n:    3,
			tol:  0.1,
			seq:  []float64{50.0, 50.5, 51.0},
			want: []bool{false, false, false},
			// window capped at n
			wantLen: 3,
		},
		{
			name: "sliding window resets once a consistent run appears",
			n:    3,
			tol:  0.1,
			// two scattered reads then three consistent ones
			seq:  []float64{40, 60, 50.00, 50.02, 50.04},
			want: []bool{false, false, false, false, true},
			// window only ever holds the last n
			wantLen: 3,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			lowerRun = nil
			t.Cleanup(func() { lowerRun = nil })
			for i, v := range tt.seq {
				if got := pushLowerRun(v, tt.n, tt.tol); got != tt.want[i] {
					t.Errorf("pushLowerRun(%v) at index %d = %v, want %v", v, i, got, tt.want[i])
				}
			}
			if len(lowerRun) != tt.wantLen {
				t.Errorf("len(lowerRun) = %d, want %d", len(lowerRun), tt.wantLen)
			}
		})
	}
}

// TestIncidentRecovery replays the real-world incident: a spurious-high OCR
// misread poisons the incr-only floor, then a run of consistent lower readings
// should recover on the 3rd one.
func TestIncidentRecovery(t *testing.T) {
	lowerRun = nil
	t.Cleanup(func() { lowerRun = nil })

	const (
		n   = 3
		tol = 0.1
	)
	floor := 377.918 // poisoned floor from the misread

	// Legitimate readings, all below the poisoned floor but consistent.
	readings := []float64{377.418, 377.495, 377.445}
	for i, r := range readings {
		result, _ := checkReadingFilter(r, floor, true, 0)
		if result != filterRejectDecrease {
			t.Fatalf("reading %.3f: expected decrease rejection, got %d", r, result)
		}
		reset := pushLowerRun(r, n, tol)
		last := i == len(readings)-1
		if reset != last {
			t.Errorf("reading %.3f (index %d): reset = %v, want %v", r, i, reset, last)
		}
	}
}