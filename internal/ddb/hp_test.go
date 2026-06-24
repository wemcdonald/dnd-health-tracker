package ddb

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

func decodeFixture(t *testing.T, name string) *characterData {
	t.Helper()
	b, err := os.ReadFile(filepath.Join("..", "..", "testdata", name))
	if err != nil {
		t.Fatal(err)
	}
	var env apiResponse
	if err := json.Unmarshal(b, &env); err != nil {
		t.Fatal(err)
	}
	if env.Data == nil {
		t.Fatal("fixture has no data")
	}
	return env.Data
}

func TestComputeHPPublicFixture(t *testing.T) {
	// base 38 + conMod(+3 from CON 14+2 racial)*level 5 = 53; removed 13 -> 40.
	hp := computeHP(decodeFixture(t, "character_public.json"))
	if hp != (HP{Current: 40, Max: 53, Temp: 0}) {
		t.Errorf("got %+v, want {40 53 0}", hp)
	}
}

func TestComputeHPOverrideFixture(t *testing.T) {
	// overrideHitPoints 60 wins over base+con; removed 10 -> 50; temp 7.
	hp := computeHP(decodeFixture(t, "character_override.json"))
	if hp != (HP{Current: 50, Max: 60, Temp: 7}) {
		t.Errorf("got %+v, want {50 60 7}", hp)
	}
}

func TestComputeHPNegativeConModifierFloors(t *testing.T) {
	d := &characterData{
		BaseHitPoints: 8,
		Stats:         []statEntry{{ID: conStatID, Value: ptr(7)}}, // CON 7 -> mod -2
		Classes:       []classEntry{{Level: 1}},
	}
	if hp := computeHP(d); hp.Max != 6 {
		t.Errorf("CON 7 at level 1: max = %d, want 6 (8 + (-2)*1)", hp.Max)
	}
}

func TestComputeHPClampsAndGuards(t *testing.T) {
	// Removed exceeds max -> current clamps to 0 (not negative).
	d := &characterData{
		BaseHitPoints:    10,
		RemovedHitPoints: 999,
		Stats:            []statEntry{{ID: conStatID, Value: ptr(10)}},
		Classes:          []classEntry{{Level: 1}},
	}
	if hp := computeHP(d); hp.Current != 0 {
		t.Errorf("over-damaged current = %d, want 0", hp.Current)
	}
	// Zero max is guarded to 1 to avoid division by zero downstream.
	if hp := computeHP(&characterData{}); hp.Max < 1 {
		t.Errorf("max should be guarded to >=1, got %d", hp.Max)
	}
}

func TestFloorDiv(t *testing.T) {
	cases := map[[2]int]int{{-3, 2}: -2, {3, 2}: 1, {-4, 2}: -2, {-1, 2}: -1, {0, 2}: 0}
	for in, want := range cases {
		if got := floorDiv(in[0], in[1]); got != want {
			t.Errorf("floorDiv(%d,%d) = %d, want %d", in[0], in[1], got, want)
		}
	}
}

func ptr(i int) *int { return &i }
