#include "routes.h"

// openapi_yaml.h is generated at build time from openapi.yaml by gen_openapi_yaml_h.py.
// It defines: static const char OPENAPI_YAML[]
#include "openapi_yaml.h"

static const std::string SWAGGER_HTML = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>stable-diffusion.cpp API</title>
  <link rel="stylesheet" href="https://unpkg.com/swagger-ui-dist@5/swagger-ui.css" />
  <style>
    body { margin: 0; }
    .topbar { display: none; }
  </style>
</head>
<body>
  <div id="swagger-ui"></div>
  <script src="https://unpkg.com/swagger-ui-dist@5/swagger-ui-bundle.js" crossorigin></script>
  <script>
    window.onload = () => {
      SwaggerUIBundle({
        url: '/openapi.yaml',
        dom_id: '#swagger-ui',
        presets: [
          SwaggerUIBundle.presets.apis,
          SwaggerUIBundle.SwaggerUIStandalonePreset,
        ],
        layout: 'BaseLayout',
        deepLinking: true,
        defaultModelsExpandDepth: 1,
        defaultModelExpandDepth: 1,
        docExpansion: 'list',
        filter: true,
        persistAuthorization: true,
      });
    };
  </script>
</body>
</html>
)HTML";

void register_docs_endpoints(httplib::Server& svr) {
    svr.Get("/openapi.yaml", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(OPENAPI_YAML, "application/yaml");
    });

    svr.Get("/docs", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(SWAGGER_HTML, "text/html");
    });
}
