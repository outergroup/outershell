// Backend for a full-stack outerframe app.
//
// Serves three things:
//
//	GET /                    the .outer descriptor (Content-Type: application/vnd.outerframe)
//	GET /frontend/<platform> the platform bundle archives (macos-arm, macos-x86)
//	GET /api/hello           a tiny binary greeting -- replace this with your app's real API
//
// It listens on a Unix domain socket by default (--socket), or a TCP port
// (--port) for local development. Static files are served from --root, which
// defaults to the directory containing this executable.
package main

import (
	"encoding/binary"
	"errors"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"runtime"
	"strings"
	"syscall"
	"time"
)

func main() {
	socketPath := flag.String("socket", "", "Unix domain socket path to listen on")
	port := flag.Int("port", 0, "TCP port to listen on (127.0.0.1), for local development")
	root := flag.String("root", "", "directory containing app.outer and frontends/ (default: executable's directory)")
	flag.Parse()

	if (*socketPath == "") == (*port == 0) {
		log.Fatal("specify exactly one of --socket or --port")
	}

	if *root == "" {
		exe, err := os.Executable()
		if err != nil {
			log.Fatalf("cannot locate executable: %v", err)
		}
		*root = filepath.Dir(exe)
	}

	mux := http.NewServeMux()
	mux.HandleFunc("GET /{$}", serveOuter(*root))
	mux.HandleFunc("GET /frontend/", serveFrontend(*root))
	mux.HandleFunc("GET /api/hello", serveHello)

	var listener net.Listener
	var err error
	if *socketPath != "" {
		// Remove a stale socket from a previous run. systemd's Restart= can
		// leave one behind; bind() fails on an existing path.
		if err := os.Remove(*socketPath); err != nil && !errors.Is(err, os.ErrNotExist) {
			log.Fatalf("cannot remove stale socket %s: %v", *socketPath, err)
		}
		listener, err = net.Listen("unix", *socketPath)
		if err != nil {
			log.Fatalf("cannot listen on %s: %v", *socketPath, err)
		}
		log.Printf("listening on unix socket %s, serving %s", *socketPath, *root)
	} else {
		listener, err = net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", *port))
		if err != nil {
			log.Fatalf("cannot listen on port %d: %v", *port, err)
		}
		log.Printf("listening on http://127.0.0.1:%d/, serving %s", *port, *root)
	}

	server := &http.Server{Handler: mux}

	// Serve until SIGTERM/SIGINT (systemd sends SIGTERM on stop).
	done := make(chan os.Signal, 1)
	signal.Notify(done, syscall.SIGTERM, syscall.SIGINT)
	go func() {
		if err := server.Serve(listener); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Fatalf("serve: %v", err)
		}
	}()
	<-done
	_ = server.Close()
	if *socketPath != "" {
		_ = os.Remove(*socketPath)
	}
}

// serveOuter serves the .outer descriptor at the root path. The special
// content type is what tells Outer Loop to launch an outerframe rather than
// render a page.
func serveOuter(root string) http.HandlerFunc {
	outerPath := filepath.Join(root, "app.outer")
	return func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/vnd.outerframe")
		http.ServeFile(w, r, outerPath)
	}
}

// serveFrontend serves the platform bundle archives referenced by the .outer
// descriptor's bundle URL (/frontend). Outer Loop requests
// /frontend/<platform> directly for its current platform.
func serveFrontend(root string) http.HandlerFunc {
	frontendsDir := filepath.Join(root, "frontends")
	return func(w http.ResponseWriter, r *http.Request) {
		name := strings.TrimPrefix(r.URL.Path, "/frontend/")
		if name == "" {
			// Plain-text platform listing; current Outer Loop builds don't
			// need it, but it's harmless and aids debugging with curl.
			entries, err := os.ReadDir(frontendsDir)
			if err != nil {
				http.Error(w, "no frontends directory", http.StatusNotFound)
				return
			}
			w.Header().Set("Content-Type", "text/plain; charset=utf-8")
			for _, e := range entries {
				fmt.Fprintln(w, e.Name())
			}
			return
		}
		if strings.Contains(name, "/") || strings.Contains(name, "..") {
			http.Error(w, "bad path", http.StatusBadRequest)
			return
		}
		w.Header().Set("Content-Type", "application/octet-stream")
		http.ServeFile(w, r, filepath.Join(frontendsDir, name))
	}
}

// serveHello is the app's "real" API. The frontend calls this over the SSH
// tunnel and displays the result. Replace it with your own endpoints.
func serveHello(w http.ResponseWriter, r *http.Request) {
	hostname, _ := os.Hostname()
	strings := []string{
		"Hello from your Go backend!",
		hostname,
		runtime.GOOS + "/" + runtime.GOARCH,
		time.Now().UTC().Format(time.RFC3339),
	}

	bodyLength := 32
	for _, s := range strings {
		bodyLength += len(s)
	}
	body := make([]byte, bodyLength)
	offset := 32
	for i, s := range strings {
		binary.LittleEndian.PutUint32(body[i*8:(i*8)+4], uint32(offset))
		binary.LittleEndian.PutUint32(body[(i*8)+4:(i*8)+8], uint32(len(s)))
		copy(body[offset:], s)
		offset += len(s)
	}

	w.Header().Set("Content-Type", "application/octet-stream")
	_, _ = w.Write(body)
}
