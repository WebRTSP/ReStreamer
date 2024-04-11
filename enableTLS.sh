#!/bin/bash

set -e

if [[ $# < 1 ]]; then
    echo "No target host specified."
    exit 1
fi

IFS='@' read -ra PARTS <<< "$1"
if [[ ${#PARTS[*]} -ne 2 ]]; then
    echo "Required target format it \"user@host[:port]\"."
    exit 1
fi

if [[ $# < 2 ]]; then
    echo "No domain owner email specified."
    exit 1
fi

EMAIL=${2}

TARGET_USER=${PARTS[0]}

IFS=':' read -ra PARTS <<< "${PARTS[1]}"
if [[ ${#PARTS[*]} -eq 1 ]]; then
  TARGET_DOMAIN=${PARTS[0]}
  TARGET_PORT=22
elif [[ ${#PARTS[*]} -eq 2 ]]; then
  TARGET_DOMAIN=${PARTS[0]}
  TARGET_PORT=${PARTS[1]}
fi

echo ssh $TARGET_USER@$TARGET_DOMAIN -p $TARGET_PORT

ssh $TARGET_USER@$TARGET_DOMAIN -p $TARGET_PORT <<EOF

set -e

sudo snap install --classic certbot

sudo apt-get update
sudo apt-get upgrade -y
sudo apt-get install nginx -y

sudo systemctl stop nginx
# sudo rm -rf /etc/nginx/sites-enabled/*

sudo certbot certonly --standalone --non-interactive --agree-tos --email $EMAIL -d $TARGET_DOMAIN

sudo tee /etc/nginx/conf.d/rtsp-to-webrtsp.conf > /dev/null <<'EOF2'

# https
server {
  server_name $TARGET_DOMAIN;

  proxy_set_header X-Real-IP \$remote_addr;
  proxy_set_header X-Forwarded-For \$proxy_add_x_forwarded_for;

  location / {
      proxy_pass http://localhost:5080/;
  }

  location = /Config.js {
    proxy_pass http://localhost:5080/Config.js;
    sub_filter 'const WebRTSPPort = 5554;' 'const WebRTSPPort = 5555;';
    sub_filter_types text/javascript application/javascript;
    sub_filter_once on;
  }

  listen [::]:5443 ssl ipv6only=on;
  listen 5443 ssl;

  ssl_certificate /etc/letsencrypt/live/$TARGET_DOMAIN/fullchain.pem;
  ssl_certificate_key /etc/letsencrypt/live/$TARGET_DOMAIN/privkey.pem;
}

# wss
server {
  server_name $TARGET_DOMAIN;

  location / {
      proxy_pass http://localhost:5554/;
      proxy_http_version 1.1;
      proxy_set_header Upgrade \$http_upgrade;
      proxy_set_header Connection "Upgrade";
      proxy_set_header X-Real-IP \$remote_addr;
      proxy_set_header X-Forwarded-For \$proxy_add_x_forwarded_for;
  }

  error_page 497 https://\$server_name:\$server_port\$request_uri;

  listen [::]:5555 ssl ipv6only=on;
  listen 5555 ssl;
  ssl_certificate /etc/letsencrypt/live/$TARGET_DOMAIN/fullchain.pem;
  ssl_certificate_key /etc/letsencrypt/live/$TARGET_DOMAIN/privkey.pem;
}

EOF2

sudo nginx -t
sudo systemctl start nginx
EOF
