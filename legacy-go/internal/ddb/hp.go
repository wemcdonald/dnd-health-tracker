package ddb

const conStatID = 3 // D&D Beyond ability-score id for Constitution

// computeHP derives current/max/temporary hit points from a character document
// using the standard D&D 5e formula:
//
//	max     = overrideHitPoints, if set; otherwise
//	          baseHitPoints + bonusHitPoints + conModifier*totalLevel
//	current = max - removedHitPoints   (clamped to [0, max])
//	temp    = temporaryHitPoints
//
// (This is more accurate than the reference project's hit-dice approximation.)
func computeHP(d *characterData) HP {
	level := 0
	for _, c := range d.Classes {
		level += c.Level
	}
	conMod := floorDiv(conScore(d)-10, 2)

	var max int
	if d.OverrideHitPoints != nil && *d.OverrideHitPoints > 0 {
		max = *d.OverrideHitPoints
	} else {
		max = d.BaseHitPoints + deref(d.BonusHitPoints) + conMod*level
	}
	if max < 1 {
		max = 1 // guard against div-by-zero; a living character has >=1 max HP
	}

	cur := max - d.RemovedHitPoints
	if cur < 0 {
		cur = 0
	}
	if cur > max {
		cur = max
	}

	temp := d.TemporaryHitPoints
	if temp < 0 {
		temp = 0
	}
	return HP{Current: cur, Max: max, Temp: temp}
}

// conScore resolves the Constitution score: base stat, replaced by an override
// if present, plus any bonus stat and constitution-score modifier bonuses
// (e.g. racial ASIs, which D&D Beyond expresses as modifiers).
func conScore(d *characterData) int {
	score := 10
	if v := statValue(d.Stats, conStatID); v != nil {
		score = *v
	}
	if v := statValue(d.OverrideStats, conStatID); v != nil {
		score = *v // an override replaces the base score
	}
	if v := statValue(d.BonusStats, conStatID); v != nil {
		score += *v
	}
	for _, list := range d.Modifiers {
		for _, m := range list {
			if m.Type == "bonus" && m.SubType == "constitution-score" && m.Value != nil {
				score += *m.Value
			}
		}
	}
	return score
}

func statValue(stats []statEntry, id int) *int {
	for _, s := range stats {
		if s.ID == id {
			return s.Value
		}
	}
	return nil
}

func deref(p *int) int {
	if p == nil {
		return 0
	}
	return *p
}

// floorDiv divides rounding toward negative infinity, so ability modifiers for
// scores below 10 are correct (e.g. CON 7 -> -2, not Go's truncated -1).
func floorDiv(a, b int) int {
	q := a / b
	if (a%b != 0) && ((a < 0) != (b < 0)) {
		q--
	}
	return q
}
