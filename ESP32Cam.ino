#include <ArduinoWebsockets.h>
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "camera_index.h"
#include "Arduino.h"
#include "fd_forward.h"
#include "fr_forward.h"
#include "fr_flash.h"
#include <WiFi.h>
#include <WiFiClient.h>

using namespace websockets;

const char* ssid = "Door";
const char* password = "12345678";
IPAddress local_ip(192,168,10,1);
IPAddress gateway(192,168,10,1);
IPAddress subnet(255,255,255,0);

bool regimConfig = true;
#define ENROLL_CONFIRM_TIMES 5
#define FACE_ID_SAVE_NUMBER 7
#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"

WebsocketsServer socket_server;

camera_fb_t * fb = NULL;
long current_millis;
long last_detected_millis = 0;
#define relay_pin 12
unsigned long door_opened_millis = 0;
long interval = 1000;
bool face_recognised = false;

void app_httpserver_init();
void send_face_list(WebsocketsClient &client);
void delete_all_faces(WebsocketsClient &client);

WebsocketsClient client;
WiFiClient py_client;

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  digitalWrite(relay_pin, LOW);
  pinMode(relay_pin, OUTPUT);
  digitalWrite(4, LOW);
  pinMode(4, OUTPUT);

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  if (psramFound()) {
    config.frame_size = FRAMESIZE_UXGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  sensor_t * s = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_QVGA);

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(local_ip, gateway, subnet);
  WiFi.softAP(ssid, password);
  IPAddress myIP = WiFi.softAPIP();

  app_httpserver_init();
  socket_server.listen(82);

  regimConfig = true;

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(myIP);
  Serial.println("' to connect");

  py_client.connect("192.168.10.2", 8080); // IP и порт вашего Python сервера
}

void app_httpserver_init() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    Serial.println("httpd_start");
    httpd_uri_t index_uri = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = [](httpd_req_t *req) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, (const char *)index_ov2640_html_gz, index_ov2640_html_gz_len);
      },
      .user_ctx = NULL
    };
    httpd_register_uri_handler(camera_httpd, &index_uri);
  }
}

void send_face_list(WebsocketsClient &client) {
  client.send("delete_faces");
  face_id_node *head = st_face_list.head;
  char add_face[64];
  for (int i = 0; i < st_face_list.count; i++) {
    sprintf(add_face, "listface:%s", head->id_name);
    client.send(add_face);
    head = head->next;
  }
}

void delete_all_faces(WebsocketsClient &client) {
  delete_face_all_in_flash_with_name(&st_face_list);
  client.send("delete_faces");
}

void open_door(WebsocketsClient &client) {
  if (digitalRead(relay_pin) == LOW) {
    digitalWrite(relay_pin, HIGH);
    Serial.println("Door Unlocked");
    client.send("door_open");
    door_opened_millis = millis();
  }
}

void handle_message(WebsocketsClient &client, WebsocketsMessage msg) {
  if (msg.data() == "stream") {
    g_state = START_STREAM;
    client.send("STREAMING");
  }
  if (msg.data() == "detect") {
    g_state = START_DETECT;
    client.send("DETECTING");
  }
  if (msg.data().substring(0, 8) == "capture:") {
    g_state = START_ENROLL;
    char person[FACE_ID_SAVE_NUMBER * ENROLL_NAME_LEN] = {0,};
    msg.data().substring(8).toCharArray(person, sizeof(person));
    memcpy(st_name.enroll_name, person, strlen(person) + 1);
    client.send("CAPTURING");
  }
  if (msg.data() == "recognise") {
    g_state = START_RECOGNITION;
    client.send("RECOGNISING");
  }
  if (msg.data().substring(0, 7) == "remove:") {
    char person[ENROLL_NAME_LEN * FACE_ID_SAVE_NUMBER];
    msg.data().substring(7).toCharArray(person, sizeof(person));
    delete_face_id_in_flash_with_name(&st_face_list, person);
    send_face_list(client);
  }
  if (msg.data() == "delete_all") {
    delete_all_faces(client);
  }
}

void loop() {
  if (regimConfig) {
    Serial.println("true");
    WiFi.mode(WIFI_AP);
    auto client = socket_server.accept();
    client.onMessage(handle_message);
    send_face_list(client);
    client.send("STREAMING");
    while (client.available()) {
      client.poll();
      if (millis() - interval > door_opened_millis) {
        digitalWrite(relay_pin, LOW);
      }
      fb = esp_camera_fb_get();
      if (g_state == START_DETECT || g_state == START_ENROLL || g_state == START_RECOGNITION) {
        fmt2rgb888(fb->buf, fb->len, fb->format, image_matrix->item);
        // Отправляем изображение на сервер для распознавания лиц
        py_client.write((const char *)fb->buf, fb->len);
        if (py_client.available()) {
          String response = py_client.readString();
          if (response == "FACE DETECTED") {
            client.send("FACE DETECTED");
            last_detected_millis = millis();
            if (g_state == START_ENROLL) {
              client.send("CAPTURING");
            }
            if (g_state == START_RECOGNITION) {
              client.send("RECOGNISING");
            }
          } else if (response == "FACE NOT DETECTED") {
            client.send("NO FACE DETECTED");
          } else if (response.startsWith("DOOR OPEN FOR ")) {
            open_door(client);
            client.send(response);
          }
        }
      }
      client.sendBinary((const char *)fb->buf, fb->len);
      esp_camera_fb_return(fb);
      fb = NULL;
    }
  }
}

