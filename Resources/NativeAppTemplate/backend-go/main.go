// Backend for a full-stack outerframe app.
//
// Serves four things:
//
//	GET /                    HTML or .outer, negotiated with Outerframe-Accept
//	GET /<web asset>         the HTML implementation
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
	host := flag.String("host", "127.0.0.1", "TCP address to listen on when --port is used")
	root := flag.String("root", "", "directory containing web/, app.outer, and frontends/ (default: executable's directory)")
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
	mux.HandleFunc("GET /frontend/", serveFrontend(*root))
	mux.HandleFunc("GET /api/hello", serveHello)
	mux.Handle("GET /", serveApp(*root))

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
		listener, err = net.Listen("tcp", fmt.Sprintf("%s:%d", *host, *port))
		if err != nil {
			log.Fatalf("cannot listen on port %d: %v", *port, err)
		}
		log.Printf("listening on http://%s:%d/, serving %s", *host, *port, *root)
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

// serveApp is the cross-platform handoff. Outerframe-aware browsers announce
// support on top-level navigation; everyone else receives the HTML target.
func serveApp(root string) http.Handler {
	outerPath := filepath.Join(root, "app.outer")
	webRoot := filepath.Join(root, "web")
	webIndexPath := filepath.Join(webRoot, "index.html")

	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cache-Control", "no-store")
		if r.URL.Path != "/" {
			if webPath, ok := webFilePath(webRoot, r.URL.Path); ok && fileExists(webPath) {
				serveStaticFile(w, r, webPath, "", false)
				return
			}
			http.NotFound(w, r)
			return
		}

		w.Header().Add("Vary", "Outerframe-Accept")
		if acceptsOuterframe(r) && fileExists(outerPath) {
			serveStaticFile(w, r, outerPath, "application/vnd.outerframe", true)
			return
		}
		if fileExists(webIndexPath) {
			serveStaticFile(w, r, webIndexPath, "text/html; charset=utf-8", true)
			return
		}
		if fileExists(outerPath) {
			http.Error(w, "This app requires an outerframe-aware browser.\n", http.StatusNotAcceptable)
			return
		}
		http.NotFound(w, r)
	})
}

func webFilePath(webRoot, requestPath string) (string, bool) {
	relative := strings.TrimPrefix(requestPath, "/")
	if relative == "" || strings.Contains(relative, "\\") {
		return "", false
	}
	clean := filepath.Clean(filepath.FromSlash(relative))
	if clean == "." || clean == ".." || strings.HasPrefix(clean, ".."+string(filepath.Separator)) {
		return "", false
	}
	return filepath.Join(webRoot, clean), true
}

func acceptsOuterframe(r *http.Request) bool {
	return strings.Contains(strings.ToLower(r.Header.Get("Outerframe-Accept")), "application/vnd.outerframe")
}

func fileExists(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.Mode().IsRegular()
}

func serveStaticFile(w http.ResponseWriter,
	r *http.Request,
	path string,
	contentType string,
	varyOuterframeAccept bool,
) {
	file, err := os.Open(path)
	if err != nil {
		http.NotFound(w, r)
		return
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil || !info.Mode().IsRegular() {
		http.NotFound(w, r)
		return
	}

	w.Header().Set("Cache-Control", "public, max-age=0, must-revalidate")
	w.Header().Set("ETag", fmt.Sprintf(`W/"%x-%x"`, info.ModTime().UnixNano(), info.Size()))
	if contentType != "" {
		w.Header().Set("Content-Type", contentType)
	}
	if varyOuterframeAccept {
		w.Header().Set("Vary", "Outerframe-Accept")
	}
	http.ServeContent(w, r, info.Name(), info.ModTime(), file)
}

// serveFrontend serves the platform bundle archives referenced by the .outer
// descriptor's bundle URL (/frontend). Outer Loop requests
// /frontend/<platform> directly for its current platform.
func serveFrontend(root string) http.HandlerFunc {
	frontendsDir := filepath.Join(root, "frontends")
	return func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cache-Control", "no-store")
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
		serveStaticFile(w, r, filepath.Join(frontendsDir, name), "application/octet-stream", false)
	}
}

// serveHello is the app's "real" API. The frontend calls this over the SSH
// tunnel and displays the result. Replace it with your own endpoints.
func serveHello(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Cache-Control", "no-store")
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
