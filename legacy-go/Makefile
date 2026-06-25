.PHONY: build test race vet fmt demo run build-arm clean

# Laptop build (terminal simulator; no cgo/C library needed).
build:
	go build ./...

test:
	go test ./...

race:
	go test -race ./...

vet:
	go vet ./...

fmt:
	gofmt -w internal cmd

# Animated preview in the terminal simulator (no hardware, no D&D Beyond).
demo:
	go run ./cmd/healthbar --demo

# Run against local config with the simulator (set character_id in the config
# dir, or use the web UI on :8080).
run:
	go run ./cmd/healthbar --config-dir ./deploy/config --web-addr :8080

# Cross-compile the hardware binary for the Pi (see deploy/build-arm.sh).
build-arm:
	./deploy/build-arm.sh

clean:
	rm -f healthbar healthbar-* *.armv7 *.arm64
