package mobile

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestCrashTextIsKept(t *testing.T) {
	if path := os.Getenv("RAWLINE_CRASH_FILE"); path != "" {
		crashOutput(path)
		panic("crash text test")
	}
	path := filepath.Join(t.TempDir(), "core-crash.log")
	cmd := exec.Command(os.Args[0], "-test.run=^TestCrashTextIsKept$")
	cmd.Env = append(os.Environ(), "RAWLINE_CRASH_FILE="+path)
	if cmd.Run() == nil {
		t.Fatal("the child did not crash")
	}
	text, err := os.ReadFile(path)
	if err != nil || !strings.Contains(string(text), "panic: crash text test") {
		t.Fatalf("crash text not kept: %v %q", err, text)
	}
}
