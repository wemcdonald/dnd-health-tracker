// Package ddb is the D&D Beyond integration: fetching a character's hit points
// from the unofficial character-service API and computing current/max/temp HP.
//
// All D&D Beyond specifics are isolated here behind a small surface (Client,
// Poller, HP) so they are easy to repair if the unofficial API changes.
package ddb

// apiResponse is the envelope returned by the v5 character endpoint.
type apiResponse struct {
	Success bool           `json:"success"`
	Message string         `json:"message"`
	Data    *characterData `json:"data"`
}

// characterData holds only the fields needed to compute HP. The full document
// is large; encoding/json ignores everything we don't declare.
type characterData struct {
	BaseHitPoints      int  `json:"baseHitPoints"`
	BonusHitPoints     *int `json:"bonusHitPoints"`
	OverrideHitPoints  *int `json:"overrideHitPoints"`
	RemovedHitPoints   int  `json:"removedHitPoints"`
	TemporaryHitPoints int  `json:"temporaryHitPoints"`

	Stats         []statEntry `json:"stats"`
	BonusStats    []statEntry `json:"bonusStats"`
	OverrideStats []statEntry `json:"overrideStats"`

	Classes   []classEntry          `json:"classes"`
	Modifiers map[string][]modifier `json:"modifiers"`
}

// statEntry is one of the six ability scores. id 3 is Constitution. value is a
// pointer because D&D Beyond uses null for "no override/bonus set".
type statEntry struct {
	ID    int  `json:"id"`
	Value *int `json:"value"`
}

// classEntry contributes its level to the character's total level.
type classEntry struct {
	Level int `json:"level"`
}

// modifier is a single rule effect; we only consume constitution-score bonuses.
type modifier struct {
	Type    string `json:"type"`
	SubType string `json:"subType"`
	Value   *int   `json:"value"`
}

// HP is the computed hit-point snapshot the rest of the app consumes.
type HP struct {
	Current int
	Max     int
	Temp    int
}
