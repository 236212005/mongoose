// Copyright (c) 2020 Cesanta Software Limited
// All rights reserved
//
// Example MQTT server. Usage:
//  1. Start this server, type `make`
//  2. Install mosquitto MQTT client
//  3. In one terminal, run:   mosquitto_sub -h localhost -t foo -t bar
//  4. In another, run:        mosquitto_pub -h localhost -t foo -m hi

#include "mongoose.h"

static const char *s_listen_on = "mqtt://0.0.0.0:1883";

char ip_buf[100];

// A list of subscription, held in memory
struct sub {
  struct sub *next;
  struct mg_connection *c;
  struct mg_str topic;
  uint8_t qos;
};
static struct sub *s_subs = NULL;

// Handle interrupts, like Ctrl-C
static int s_signo;
static void signal_handler(int signo) {
  s_signo = signo;
}

// Event handler function
static void fn(struct mg_connection *c, int ev, void *ev_data, void *fn_data) {
  if (ev == MG_EV_MQTT_CMD) {
    mg_ntoa(&c->peer, ip_buf, sizeof(ip_buf));
    struct mg_mqtt_message *mm = (struct mg_mqtt_message *) ev_data;
    LOG(LL_INFO, ("cmd 0x%x qos %d", mm->cmd, mm->qos));
    switch (mm->cmd) {
      case MQTT_CMD_CONNECT: {
        LOG(LL_INFO, ("Received connect request from %s.",ip_buf));
        // Client connects
        if (mm->dgram.len < 9) {
          mg_error(c, "Malformed MQTT frame");
        } else if (mm->dgram.ptr[8] != 4) {
          mg_error(c, "Unsupported MQTT version %d", mm->dgram.ptr[8]);
        } else {
          uint8_t response[] = {0, 0};
          mg_mqtt_send_header(c, MQTT_CMD_CONNACK, 0, sizeof(response));
          mg_send(c, response, sizeof(response));
        }
        break;
      }
      case MQTT_CMD_SUBSCRIBE: {
        LOG(LL_INFO, ("Received subscribe request from %s.",ip_buf));
        // Client subscribes
        size_t pos = 4;  // Initial topic offset, where ID ends
        uint8_t qos, resp[256];
        struct mg_str topic;
        int num_topics = 0;
        while ((pos = mg_mqtt_next_sub(mm, &topic, &qos, pos)) > 0) {
          struct sub *sub = calloc(1, sizeof(*sub));
          sub->c = c;
          sub->topic = mg_strdup(topic);
          sub->qos = qos;
          LIST_ADD_HEAD(struct sub, &s_subs, sub);
          LOG(LL_INFO,
              ("%s SUB %p [%.*s]", ip_buf, c->fd, (int) sub->topic.len, sub->topic.ptr));
          // Change '+' to '*' for topic matching using mg_globmatch
          for (size_t i = 0; i < sub->topic.len; i++) {
            if (sub->topic.ptr[i] == '+') ((char *) sub->topic.ptr)[i] = '*';
          }
          resp[num_topics++] = qos;
        }
        mg_mqtt_send_header(c, MQTT_CMD_SUBACK, 0, num_topics + 2);
        uint16_t id = mg_htons(mm->id);
        mg_send(c, &id, 2);
        mg_send(c, resp, num_topics);
        break;
      }
      case MQTT_CMD_PUBLISH: {
        // Client published message. Push to all subscribed channels
        LOG(LL_INFO, ("PUB %p [%.*s] -> [%.*s]", c->fd, (int) mm->data.len,
                      mm->data.ptr, (int) mm->topic.len, mm->topic.ptr));
        for (struct sub *sub = s_subs; sub != NULL; sub = sub->next) {
          if (mg_globmatch(sub->topic.ptr, sub->topic.len, mm->topic.ptr,
                           mm->topic.len)) {
            mg_mqtt_pub(sub->c, mm->topic, mm->data, 1, false);
          }
        }
        break;
      }
    }
  } else if (ev == MG_EV_ACCEPT) {
    // c->is_hexdumping = 1;
  } else if (ev == MG_EV_CLOSE) {
    // Client disconnects. Remove from the subscription list
    for (struct sub *next, *sub = s_subs; sub != NULL; sub = next) {
      next = sub->next;
      if (c != sub->c) continue;
      LOG(LL_INFO,
          ("UNSUB %p [%.*s]", c->fd, (int) sub->topic.len, sub->topic.ptr));
      LIST_DELETE(struct sub, &s_subs, sub);
    }
  }
  (void) fn_data;
}

int main(void) {
  struct mg_mgr mgr;                // Event manager
  signal(SIGINT, signal_handler);   // Setup signal handlers - exist event
  signal(SIGTERM, signal_handler);  // manager loop on SIGINT and SIGTERM
  mg_mgr_init(&mgr);                // Initialise event manager
  LOG(LL_INFO, ("Starting on %s", s_listen_on));  // Inform that we're starting
  mg_mqtt_listen(&mgr, s_listen_on, fn, NULL);    // Create MQTT listener
  while (s_signo == 0) mg_mgr_poll(&mgr, 1000);   // Event loop, 1s timeout
  mg_mgr_free(&mgr);                              // Cleanup
  return 0;
}
