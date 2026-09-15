#pragma once

// Read-mostly HTTP surface over WiFi (see wifi_net.h), additive to BLE —
// the dashboard page, a JSON mirror of device state, and an on-demand
// screenshot. No mutating routes yet (Phase 2 adds config, token-gated via
// wifi_get_token()).

void web_server_init(void);   // call once from setup(); safe before WiFi connects
void web_server_tick(void);   // call every loop(); drives WebServer::handleClient()
