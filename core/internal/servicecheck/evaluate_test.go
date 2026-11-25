package servicecheck

import (
	"errors"
	"io"
	"net"
	"net/http"
	"syscall"
	"testing"
)

func TestClassifyProbeFailure(t *testing.T) {
	const cutSize = 18 * 1024
	for _, test := range []struct {
		name     string
		response Response
		want     string
	}{
		{"dns", Response{Err: &net.DNSError{Err: "not found"}}, StatusDNSFailure},
		{"tls", Response{Err: &tlsHandshakeError{errors.New("peer context")}}, StatusTLSFailure},
		{"reset", Response{Err: syscall.ECONNRESET}, StatusInterrupted},
		{"short", Response{Status: 200, Header: http.Header{"Content-Length": {"24576"}}, Body: make([]byte, cutSize), Err: io.ErrUnexpectedEOF}, StatusShortBody},
		{"legitimate size", Response{Status: 200, Header: http.Header{"Content-Length": {"18432"}}, Body: make([]byte, cutSize), Err: io.ErrUnexpectedEOF}, StatusInterrupted},
		{"compressed", Response{Status: 200, Header: http.Header{"Content-Length": {"24576"}, "Content-Encoding": {"gzip"}}, Body: make([]byte, cutSize), Err: io.ErrUnexpectedEOF}, StatusInterrupted},
	} {
		t.Run(test.name, func(t *testing.T) {
			if got := classifyFailure(&test.response); got != test.want {
				t.Fatalf("classifyFailure = %q, want %q", got, test.want)
			}
		})
	}
}

func TestExpandedDefaultCatalog(t *testing.T) {
	definitions := Default()
	found := map[string]bool{}
	for _, check := range definitions.Checks {
		found[check.ID] = true
	}
	for _, id := range []string{"chatgpt", "cloudflare_transfer", "telegram_web", "github_web", "wikipedia_reading"} {
		if !found[id] {
			t.Errorf("missing default check %q", id)
		}
	}
}
