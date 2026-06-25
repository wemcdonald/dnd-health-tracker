package web

import (
	"embed"
	"fmt"
	"html/template"
	"net/http"

	"github.com/will/dnd-health-tracker/internal/app"
	"github.com/will/dnd-health-tracker/internal/config"
)

//go:embed templates/*.html
var tmplFS embed.FS

var funcs = template.FuncMap{
	"hex": func(h config.HexColor) string {
		b, _ := h.MarshalText()
		return string(b)
	},
}

// pageNames are the content templates composed with base.html.
var pageNames = []string{"status", "device", "theme", "wifi", "credentials"}

var pages = func() map[string]*template.Template {
	m := make(map[string]*template.Template, len(pageNames))
	for _, name := range pageNames {
		m[name] = template.Must(template.New(name).Funcs(funcs).
			ParseFS(tmplFS, "templates/base.html", "templates/"+name+".html"))
	}
	return m
}()

// viewData is the data passed to every page template.
type viewData struct {
	Title       string
	Flash       string
	Status      app.Status
	Device      config.Device
	Theme       config.Theme
	Networks    []config.WiFiNetwork
	Bookmarklet template.URL
}

// render writes a page or a 500 if the template is unknown/fails.
func render(w http.ResponseWriter, page string, data viewData) {
	t, ok := pages[page]
	if !ok {
		http.Error(w, fmt.Sprintf("unknown page %q", page), http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	if err := t.ExecuteTemplate(w, "base", data); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
	}
}
