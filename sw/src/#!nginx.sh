#!nginx

  location ~ /(?<fwd_path>.*)/rest/config {
    add_header "X-Polyas-Path" "path: http://backend$DOMAIN:8091/api/anon/ballot/preview/$fwd_path/rest/config$is_args$args";
    proxy_pass http://backend$DOMAIN:8091/api/anon/ballot/preview/$fwd_path/rest/config$is_args$args;
  }
  location ~ /lv/preview/(?<fwd_path>.*)/theme.css {
    add_header "X-Polyas" "theme http://backend$DOMAIN:8091/api/anon/ballot/preview/$fwd_path/theme.css$is_args$args;";
    proxy_pass http://backend$DOMAIN:8091/api/anon/ballot/preview/$fwd_path/theme.css$is_args$args;
  }
  location ~ /(?<fwd_path>.*)/candidate_images/(?<imgfile>.*) {
    proxy_pass  http://backend$DOMAIN:8091/api/anon/ballot/preview/$fwd_path/candidate_images/$imgfile$is_args$args;
  }
  location ~ /(?<fwd_path>.*)/(?<staticcontent>.+) {
    add_header "X-Polyas" "static http://backend$DOMAIN:8091/api/anon/ballot/preview/$fwd_path/theme.css$is_args$args;";
    proxy_pass http://127.0.0.1:80/$staticcontent$is_args$args;
  }