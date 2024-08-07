#!/bin/sh -e

PUBLIC_IP=$(snapctl get public-ip)
if [ -n "$PUBLIC_IP" ]; then
    EXTERNAL_IP_OPTION=--external-ip=$PUBLIC_IP
fi

COTURN="$SNAP/opt/coturn/bin/turnserver \
    -c $SNAP_COMMON/turnserver.conf \
    --db=$SNAP_COMMON/turndb \
    $EXTERNAL_IP_OPTION \
    --use-auth-secret  \
    --realm=$SNAP_NAME \
    --no-cli \
    --log-file=stdout \
    --simple-log \
    --pidfile= \
    --no-tls \
    --no-dtls"
echo Starting Coturn with: $COTURN
$COTURN
