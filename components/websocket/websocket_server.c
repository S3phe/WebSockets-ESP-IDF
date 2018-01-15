
#include "websocket_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

// #include "esp_log.h"
// const static char* TAG = "websocket_server";

static SemaphoreHandle_t xwebsocket_mutex; // to lock the client array
static QueueHandle_t xwebsocket_queue; // to hold the clients that send messages
static ws_client_t clients[WEBSOCKET_SERVER_MAX_CLIENTS]; // holds list of clients
static uint8_t xwebsocket_len; // number of connected clients
static TaskHandle_t xtask; // the task itself

static void background_callback(struct netconn* conn, enum netconn_evt evt,u16_t len) {
  switch(evt) {
    case NETCONN_EVT_RCVPLUS:
      xQueueSendToBack(xwebsocket_queue,&conn,WEBSOCKET_SERVER_QUEUE_TIMEOUT);
      break;
    default:
      break;
  }
}

static void handle_read(uint8_t num) {
  ws_header_t header;
  char* msg;

  header.received = 0;
  //ESP_LOGI(TAG,"actually about to read message...");
  msg = ws_read(&clients[num],&header);
  //ESP_LOGI(TAG,"Got message: %s",msg);
  if(!header.received) return NULL;
  //ESP_LOGI(TAG,"read message, sending out");
  switch(clients[num].last_opcode) {
    case WEBSOCKET_OPCODE_CONT:
      break;
    case WEBSOCKET_OPCODE_BIN:
      clients[num].scallback(num,WEBSOCKET_BIN,msg,header.length);
      break;
    case WEBSOCKET_OPCODE_TEXT:
      clients[num].scallback(num,WEBSOCKET_TEXT,msg,header.length);
      break;
    case WEBSOCKET_OPCODE_PING:
      ws_send(&clients[num],WEBSOCKET_OPCODE_PONG,msg,header.length,0);
      clients[num].scallback(num,WEBSOCKET_PING,msg,header.length);
      break;
    case WEBSOCKET_OPCODE_PONG:
      if(clients[num].ping) {
        clients[num].scallback(num,WEBSOCKET_PONG,NULL,0);
        clients[num].ping = 0;
      }
      break;
    case WEBSOCKET_OPCODE_CLOSE:
      clients[num].scallback(num,WEBSOCKET_DISCONNECT_EXTERNAL,NULL,0);
      ws_disconnect_client(&clients[num]);
      break;
    default:
      break;
  }
  if(msg) free(msg);
}

static void ws_server_task(void* pvParameters) {
  struct netconn* conn;

  xwebsocket_mutex = xSemaphoreCreateMutex();
  xwebsocket_queue = xQueueCreate(WEBSOCKET_SERVER_QUEUE_SIZE, sizeof(struct netconn*));
  xwebsocket_len = 0;

  // initialize all clients
  for(int i=0;i<WEBSOCKET_SERVER_MAX_CLIENTS;i++) {
    clients[i].conn = NULL;
    clients[i].url  = NULL;
    clients[i].ping = 0;
    clients[i].last_opcode = 0;
    clients[i].contin = NULL;
    clients[i].len = 0;
    clients[i].ccallback = NULL;
    clients[i].scallback = NULL;
  }

  //ESP_LOGI(TAG,"task started");

  for(;;) {
    xQueueReceive(xwebsocket_queue,&conn,portMAX_DELAY);
    //ESP_LOGI(TAG,"got something from a client! handling");
    if(!conn) continue;
    //ESP_LOGI(TAG,"about to grab mutex");
    xSemaphoreTake(xwebsocket_mutex,portMAX_DELAY); // take access
    //ESP_LOGI(TAG,"got mutex");
    for(int i=0;i<WEBSOCKET_SERVER_MAX_CLIENTS;i++) {
      if(clients[i].conn == conn) {
        //ESP_LOGI(TAG,"client %i is connected and sent message, handling read",i);
        handle_read(i);
        break;
      }
    }
    xSemaphoreGive(xwebsocket_mutex); // return access
    //ESP_LOGI(TAG,"finished handling");
  }
  vTaskDelete(NULL);
}

int ws_server_start() {
  if(xtask) return 0;
  #if WEBSOCKET_SERVER_PINNED
  xTaskCreatePinnedToCore(&ws_server_task,
                          "ws_server_task",
                          WEBSOCKET_SERVER_TASK_STACK_DEPTH,
                          NULL,
                          WEBSOCKET_SERVER_TASK_PRIORITY,
                          &xtask,
                          WEBSOCKET_SERVER_PINNED_CORE);
  #else
  xTaskCreate(&ws_server_task,
              "ws_server_task",
              WEBSOCKET_SERVER_TASK_STACK_DEPTH,
              NULL,
              WEBSOCKET_SERVER_TASK_PRIORITY,
              &xtask);
  #endif
  return 1;
}

int ws_server_stop() {
  if(!xtask) return 0;
  vTaskDelete(xtask);
  return 1;
}

static bool prepare_response(char* buf,uint32_t buflen,char* handshake) {
  const char WS_HEADER[] = "Upgrade: websocket\r\n";
  const char WS_KEY[] = "Sec-WebSocket-Key: ";
  const char WS_RSP[] = "HTTP/1.1 101 Switching Protocols\r\n" \
                        "Upgrade: websocket\r\n" \
                        "Connection: Upgrade\r\n" \
                        "Sec-WebSocket-Accept: %s\r\n\r\n";
  //const uint8_t WS_KEY_LEN = 19;

  char* key_start;
  char* key_end;
  char* hashed_key;

  if(!strstr(buf,WS_HEADER)) return 0;
  if(!buflen) return 0;
  key_start = strstr(buf,WS_KEY);
  if(!key_start) return 0;
  key_start += 19;
  key_end = strstr(key_start,"\r\n");
  if(!key_end) return 0;

  hashed_key = ws_hash_handshake(key_start,key_end-key_start);
  if(!hashed_key) return 0;
  //ESP_LOGI(TAG,"handshake: %s",hashed_key);
  sprintf(handshake,WS_RSP,hashed_key);
  return 1;
}

int ws_server_add_client(struct netconn* conn,
                         char* msg,
                         uint16_t len,
                         char* url,
                         void (*callback)(uint8_t num,
                                          WEBSOCKET_TYPE_t type,
                                          char* msg,
                                          uint64_t len)) {
  int ret;
  char handshake[256];

  //struct netbuf* inbuf;
  //char* buf;
  //uint16_t buflen;

  if(!prepare_response(msg,len,handshake)) {
    netconn_close(conn);
    netconn_delete(conn);
    return -2;
  }


  ret = -1;
  xSemaphoreTake(xwebsocket_mutex,portMAX_DELAY);
  conn->callback = background_callback;
  netconn_write(conn,handshake,strlen(handshake),NETCONN_COPY);
  //////ESP_LOGI(TAG,"wrote handshake, reading any response...");
  //netconn_recv(conn,&inbuf);
  //netbuf_data(inbuf,(void**)&buf,&buflen);
  ////ESP_LOGI(TAG,"read %s",buf);

  //ret = -1;
  //vTaskDelay(1); // wait for the message to completely send
  //ESP_LOGI(TAG,"adding client, grabbing mutex...");
  //xSemaphoreTake(xwebsocket_mutex,portMAX_DELAY);
  //ESP_LOGI(TAG,"got mutex");
  for(int i=0;i<WEBSOCKET_SERVER_MAX_CLIENTS;i++) {
    if(clients[i].conn) continue;
    callback(i,WEBSOCKET_CONNECT,NULL,0);
    clients[i] = ws_connect_client(conn,url,NULL,callback);
    if(!ws_is_connected(clients[i])) {
      callback(i,WEBSOCKET_DISCONNECT_ERROR,NULL,0);
      ws_disconnect_client(&clients[i]);
      break;
    }
    //clients[i].scallback(i,WEBSOCKET_CONNECT,NULL,0);
    ret = i;
    break;
  }
  xSemaphoreGive(xwebsocket_mutex);
  //ESP_LOGI(TAG,"added client number %i",ret);
  return ret;
}

int ws_server_len_url(char* url) {
  //int len = strlen(url);
  int ret;
  ret = 0;
  xSemaphoreTake(xwebsocket_mutex,portMAX_DELAY);
  for(int i=0;i<WEBSOCKET_SERVER_MAX_CLIENTS;i++) {
    if(clients[i].url && strcmp(url,clients[i].url)) ret++;
  }
  xSemaphoreGive(xwebsocket_mutex);
  return ret;
}

int ws_server_len_all() {
  int ret;
  ret = 0;
  xSemaphoreTake(xwebsocket_mutex,portMAX_DELAY);
  for(int i=0;i<WEBSOCKET_SERVER_MAX_CLIENTS;i++) {
    if(clients[i].conn) ret++;
  }
  xSemaphoreGive(xwebsocket_mutex);
  return ret;
}

int ws_server_remove_client(int num) {
  int ret = 0;
  xSemaphoreTake(xwebsocket_mutex,portMAX_DELAY);
  if(ws_is_connected(clients[num])) {
    clients[num].scallback(num,WEBSOCKET_DISCONNECT_INTERNAL,NULL,0);
    ws_disconnect_client(&clients[num]);
    ret = 1;
  }
  xSemaphoreGive(xwebsocket_mutex);
  return ret;
}

int ws_server_remove_clients(char* url) {
  int ret = 0;
  xSemaphoreTake(xwebsocket_mutex,portMAX_DELAY);
  for(int i=0;i<WEBSOCKET_SERVER_MAX_CLIENTS;i++) {
    if(ws_is_connected(clients[i]) && strcmp(url,clients[i].url)) {
      clients[i].scallback(i,WEBSOCKET_DISCONNECT_INTERNAL,NULL,0);
      ws_disconnect_client(&clients[i]);
      ret += 1;
    }
  }
  xSemaphoreGive(xwebsocket_mutex);
  return ret;
}

int ws_server_remove_all() {
  int ret = 0;
  xSemaphoreTake(xwebsocket_mutex,portMAX_DELAY);
  for(int i=0;i<WEBSOCKET_SERVER_MAX_CLIENTS;i++) {
    if(ws_is_connected(clients[i])) {
      clients[i].scallback(i,WEBSOCKET_DISCONNECT_INTERNAL,NULL,0);
      ws_disconnect_client(&clients[i]);
      ret += 1;
    }
  }
  xSemaphoreGive(xwebsocket_mutex);
  return ret;
}

int ws_server_send_text_client(int num,char* msg,uint64_t len) {
  int ret = 0;
  xSemaphoreTake(xwebsocket_mutex,portMAX_DELAY);
  if(ws_is_connected(clients[num])) {
    ws_send(&clients[num],WEBSOCKET_OPCODE_TEXT,msg,len,0);
    ret = 1;
    if(!ws_is_connected(clients[num])) {
      clients[num].scallback(num,WEBSOCKET_DISCONNECT_ERROR,NULL,0);
      ws_disconnect_client(&clients[num]);
      ret = 0;
    }
  }
  xSemaphoreGive(xwebsocket_mutex);
  return ret;
}

int ws_server_send_text_clients(char* url,char* msg,uint64_t len) {
  int ret = 0;
  xSemaphoreTake(xwebsocket_mutex,portMAX_DELAY);
  for(int i=0;i<WEBSOCKET_SERVER_MAX_CLIENTS;i++) {
    if(ws_is_connected(clients[i]) && strcmp(clients[i].url,url)) {
      ws_send(&clients[i],WEBSOCKET_OPCODE_TEXT,msg,len,0);
      if(ws_is_connected(clients[i])) ret += 1;
      else {
        clients[i].scallback(i,WEBSOCKET_DISCONNECT_ERROR,NULL,0);
        ws_disconnect_client(&clients[i]);
      }
    }
  }
  xSemaphoreGive(xwebsocket_mutex);
  return ret;
}

int ws_server_send_text_all(char* msg,uint64_t len) {
  int ret = 0;
  xSemaphoreTake(xwebsocket_mutex,portMAX_DELAY);
  for(int i=0;i<WEBSOCKET_SERVER_MAX_CLIENTS;i++) {
    if(ws_is_connected(clients[i])) {
      ws_send(&clients[i],WEBSOCKET_OPCODE_TEXT,msg,len,0);
      if(ws_is_connected(clients[i])) ret += 1;
      else {
        clients[i].scallback(i,WEBSOCKET_DISCONNECT_ERROR,NULL,0);
        ws_disconnect_client(&clients[i]);
      }
    }
  }
  xSemaphoreGive(xwebsocket_mutex);
  return ret;
}



/////////////////////

int ws_server_send_text_client_from_callback(int num,char* msg,uint64_t len) {
  int ret = 0;
  if(ws_is_connected(clients[num])) {
    ws_send(&clients[num],WEBSOCKET_OPCODE_TEXT,msg,len,0);
    ret = 1;
    if(!ws_is_connected(clients[num])) {
      clients[num].scallback(num,WEBSOCKET_DISCONNECT_ERROR,NULL,0);
      ws_disconnect_client(&clients[num]);
      ret = 0;
    }
  }
  return ret;
}

int ws_server_send_text_clients_from_callback(char* url,char* msg,uint64_t len) {
  int ret = 0;
  for(int i=0;i<WEBSOCKET_SERVER_MAX_CLIENTS;i++) {
    if(ws_is_connected(clients[i]) && strcmp(clients[i].url,url)) {
      ws_send(&clients[i],WEBSOCKET_OPCODE_TEXT,msg,len,0);
      if(ws_is_connected(clients[i])) ret += 1;
      else {
        clients[i].scallback(i,WEBSOCKET_DISCONNECT_ERROR,NULL,0);
        ws_disconnect_client(&clients[i]);
      }
    }
  }
  return ret;
}

int ws_server_send_text_all_from_callback(char* msg,uint64_t len) {
  int ret = 0;
  for(int i=0;i<WEBSOCKET_SERVER_MAX_CLIENTS;i++) {
    if(ws_is_connected(clients[i])) {
      ws_send(&clients[i],WEBSOCKET_OPCODE_TEXT,msg,len,0);
      if(ws_is_connected(clients[i])) ret += 1;
      else {
        clients[i].scallback(i,WEBSOCKET_DISCONNECT_ERROR,NULL,0);
        ws_disconnect_client(&clients[i]);
      }
    }
  }
  return ret;
}
