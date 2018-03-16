/* Copyright (c) 2017 Arrow Electronics, Inc.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Apache License 2.0
 * which accompanies this distribution, and is available at
 * http://apache.org/licenses/LICENSE-2.0
 * Contributors: Arrow Electronics, Inc.
 */

#include "arrow/mqtt.h"
#include <config.h>
#include <arrow/sign.h>
#include <mqtt/client/MQTTClient.h>
#include <json/telemetry.h>
#include <data/property.h>
#include <debug.h>

#include <arrow/events.h>

#define USE_STATIC
#include <data/chunk.h>

#if defined(NO_EVENTS)
  #define MQTT_CLEAN_SESSION 1
#else
  #define MQTT_CLEAN_SESSION 0
#endif

static Network mqtt_net;
static MQTTClient mqtt_client;
static unsigned char buf[MQTT_BUF_LEN];
static unsigned char readbuf[MQTT_BUF_LEN];

#define S_TOP_NAME "krs/cmd/stg/"
#define P_TOP_NAME "krs.tel.gts."

#define S_TOP_LEN sizeof(S_TOP_NAME) + 66
#define P_TOP_LEN sizeof(P_TOP_NAME) + 66

static char s_topic[S_TOP_LEN];
static char p_topic[P_TOP_LEN];

static void messageArrived(MessageData* md) {
    MQTTMessage* message = md->message;
    DBG("mqtt msg arrived %u", message->payloadlen);
    char *nt_str = (char*)malloc(message->payloadlen+1);
    if ( !nt_str ) {
        DBG("Can't allocate more memory [%d]", message->payloadlen+1);
        return;
    }
    memcpy(nt_str, message->payload, message->payloadlen);
    nt_str[message->payloadlen] = 0x0;
#if defined(SINGLE_SOCKET)
    mqtt_disconnect();
#endif
    process_event(nt_str);
    free(nt_str);
}

#if defined(__IBM__)
int mqtt_connect_ibm(arrow_device_t *device,
                     arrow_gateway_config_t *config) {
  char username[100];
  char *organizationId = P_VALUE(config->organizationId);
  char *gatewayType = P_VALUE(config->gatewayType);
  char *gatewayId = P_VALUE(config->gatewayId);
  char *authToken = P_VALUE(config->authToken);
  char *deviceType = P_VALUE(device->type);
  char *externalId = P_VALUE(device->eid);

  int ret = snprintf(username, sizeof(username), "g:%s:%s:%s",
                     organizationId, gatewayType, gatewayId);
  username[ret] = '\0';

  ret = snprintf(p_topic, sizeof(p_topic),
                 "iot-2/type/%s/id/%s/evt/telemetry/fmt/json",
                 deviceType, externalId);
  p_topic[ret] = '\0';

  ret = snprintf(s_topic, sizeof(s_topic),
                 "iot-2/type/%s/id/%s/cmd/operation/fmt/json",
                 deviceType, externalId);
  s_topic[ret] = '\0';

  MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
  data.willFlag = 0;
  data.MQTTVersion = 3;
  data.clientID.cstring = username;
  data.username.cstring = "use-token-auth";
  data.password.cstring = authToken;
  data.keepAliveInterval = 10;
  data.cleansession = MQTT_CLEAN_SESSION;

  int rc;
  NetworkInit(&mqtt_net);
  char mqtt_addr[100];
  if ( !organizationId || !strlen(organizationId) ) {
      DBG("There is no organizationId");
      return -1;
  }
  strcpy(mqtt_addr, organizationId);
  strcat(mqtt_addr, MQTT_ADDR);
  DBG("addr: (%d)%s", strlen(mqtt_addr), mqtt_addr);
  rc = NetworkConnect(&mqtt_net, mqtt_addr, MQTT_PORT);
  DBG("Connecting to %s %d", mqtt_addr, MQTT_PORT);
  if ( rc < 0 ) return rc;
  MQTTClientInit(&mqtt_client, &mqtt_net, 3000, buf, MQTT_BUF_LEN, readbuf, MQTT_BUF_LEN);
  rc = MQTTConnect(&mqtt_client, &data);
  DBG("Connected %d", rc);
  return rc;
}
#elif defined(__AZURE__)
#include <time/time.h>
#include <arrow/utf8.h>
#include <crypt/crypt.h>
#include "wolfssl/wolfcrypt/coding.h"
#ifndef SHA256_DIGEST_SIZE
#define SHA256_DIGEST_SIZE 32
#endif


static int sas_token_gen(char *sas, char *devname, char *key, char *time_exp) {
  char *dev = MQTT_ADDR "/devices/";
  char common[256];
  strcpy(common, dev);
  strcat(common, devname);
  strcat(common, "\n");
  strcat(common, time_exp);
  DBG("common string <%s>", common);
  char hmacdig[SHA256_DIGEST_SIZE];

  char decoded_key[100];
  int decoded_key_len = 100;
  DBG("decode key %d {%s}\r\n", strlen(key), key);
  int ret = Base64_Decode((byte*)key, (word32)strlen(key), (byte*)decoded_key, (word32*)&decoded_key_len);
  if ( ret ) return -1;

  hmac256(hmacdig, decoded_key, decoded_key_len, common, strlen(common));

  decoded_key_len = 100;
  ret = Base64_Encode((byte*)hmacdig, SHA256_DIGEST_SIZE, (byte*)decoded_key, (word32*)&decoded_key_len);
  if ( ret ) return -1;
  decoded_key[decoded_key_len] = 0x0;

  urlencode(sas, decoded_key, decoded_key_len-1);

  DBG("enc hmac [%s]\r\n", decoded_key);
  return 0;
}

static int mqtt_connect_azure(arrow_gateway_t *gateway,
                       arrow_device_t *device,
                       arrow_gateway_config_t *config) {
  SSP_PARAMETER_NOT_USED(device);
  char username[100];
  strcpy(username, config->host);
  strcat(username, "/");
  strcat(username, gateway->uid);
  strcat(username, "/");
  strcat(username, "api-version=2016-11-14&DeviceClientType=iothubclient%2F1.1.7");
  DBG("qmtt.username %s", username);
  char time_exp[32];
  int time_len = sprintf(time_exp, "%ld", time(NULL) + 3600);
  time_exp[time_len] = '\0';

  strcpy(s_topic, "devices/");
  strcat(s_topic, gateway->uid);
  strcat(s_topic, "/messages/events/");

  strcpy(p_topic, "devices/");
  strcat(p_topic, gateway->uid);
  strcat(p_topic, "/messages/events/");

  char pass[256];
  char sas[128];
  if ( sas_token_gen(sas, gateway->uid, config->accessKey, time_exp) < 0) {
    DBG("Fail SAS");
    return -2;
  }
  DBG("SAS: {%s}\r\n", sas);
  strcpy(pass, "SharedAccessSignature sr=");
  strcat(pass, config->host);
  strcat(pass, "/devices/");
  strcat(pass, gateway->uid);
  strcat(pass, "&sig=");
  strcat(pass, sas);
  strcat(pass, "&se=");
  strcat(pass, time_exp);
  strcat(pass, "&skn=");

  DBG("pass: %s\r\n", pass);

  MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
  data.willFlag = 0;
  data.will.qos = 1;
  data.MQTTVersion = 4;
  data.clientID.cstring = gateway->uid;
  data.username.cstring = username;
  data.password.cstring = pass;
  data.keepAliveInterval = 240;
  data.cleansession = 0;

  int rc;
  NetworkInit(&mqtt_net);
  char mqtt_addr[100];
  strcpy(mqtt_addr, MQTT_ADDR);
  DBG("azure addr: (%d)%s", strlen(mqtt_addr), mqtt_addr);
  rc = NetworkConnect(&mqtt_net, mqtt_addr, MQTT_PORT);
  DBG("Connecting to %s %d", mqtt_addr, MQTT_PORT);
  if ( rc < 0 ) return rc;
  MQTTClientInit(&mqtt_client, &mqtt_net, 3000, buf, MQTT_BUF_LEN, readbuf, MQTT_BUF_LEN);
  rc = MQTTConnect(&mqtt_client, &data);
  DBG("Connected %d", rc);
  if ( rc != MQTT_SUCCESS ) return FAILURE;
  return rc;
}
#else
static int mqtt_connect_iot(arrow_gateway_t *gateway) {
#define USERNAME_LEN (sizeof(VHOST) + 66)

  CREATE_CHUNK(username, USERNAME_LEN);

  int ret = snprintf(username, USERNAME_LEN, "%s%s", VHOST, P_VALUE(gateway->hid));
  if ( ret < 0 ) goto mqtt_connect_done;
  username[ret] = 0x0;
  DBG("qmtt.username %s", username);

  ret = snprintf(s_topic, S_TOP_LEN, "%s%s", S_TOP_NAME, P_VALUE(gateway->hid));
  if ( ret < 0 ) goto mqtt_connect_done;
  s_topic[ret] = 0x0;

  ret = snprintf(p_topic, P_TOP_LEN, "%s%s", P_TOP_NAME, P_VALUE(gateway->hid));
  if ( ret < 0 ) goto mqtt_connect_done;
  p_topic[ret] = 0x0;

  MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
  data.willFlag = 0;
  data.MQTTVersion = 3;
  data.clientID.cstring = P_VALUE(gateway->hid);
  data.username.cstring = username;
  data.password.cstring = (char*)get_api_key();
  data.keepAliveInterval = 10;
#if defined(NO_EVENTS)
  data.cleansession = 1;
#else
  data.cleansession = 0;
#endif

  NetworkInit(&mqtt_net);
  ret = NetworkConnect(&mqtt_net, MQTT_ADDR, MQTT_PORT);
  if ( ret < 0 ) {
      DBG("MQTT Network connect failed: %s", MQTT_ADDR);
      goto mqtt_network_connect_done;
  }
  DBG("Connecting to %s %d", MQTT_ADDR, MQTT_PORT);
  MQTTClientInit(&mqtt_client, &mqtt_net, DEFAULT_MQTT_TIMEOUT, buf, MQTT_BUF_LEN, readbuf, MQTT_BUF_LEN);
  ret = MQTTConnect(&mqtt_client, &data);
mqtt_network_connect_done:
  if ( ret < 0 ) NetworkDisconnect(&mqtt_net);
mqtt_connect_done:
  FREE_CHUNK(username);
  return ret;
}
#endif


int mqtt_connect(arrow_gateway_t *gateway,
                 arrow_device_t *device,
                 arrow_gateway_config_t *config) {
#if defined(__IBM__)
  SSP_PARAMETER_NOT_USED(gateway);
  return mqtt_connect_ibm(device, config);
#elif defined(__AZURE__)
  SSP_PARAMETER_NOT_USED(gateway);
  SSP_PARAMETER_NOT_USED(device);
  SSP_PARAMETER_NOT_USED(config);
  return mqtt_connect_azure(gateway, device, config);
#else
  SSP_PARAMETER_NOT_USED(device);
  SSP_PARAMETER_NOT_USED(config);
  return mqtt_connect_iot(gateway);
#endif

}

void mqtt_disconnect(void) {
    MQTTDisconnect(&mqtt_client);
    NetworkDisconnect(&mqtt_net);
}

int mqtt_subscribe(void) {
    DBG("Subscribing to %s", s_topic);
    int rc = MQTTSubscribe(&mqtt_client, s_topic, QOS2, messageArrived);
    if ( rc < 0 ) {
        DBG("Subscribe failed %d\n", rc);
    }
    return rc;
}

int mqtt_yield(int timeout_ms) {
  return MQTTYield(&mqtt_client, timeout_ms);
}

int mqtt_publish(arrow_device_t *device, void *d) {
    MQTTMessage msg = {
        MQTT_QOS,
        MQTT_RETAINED,
        MQTT_DUP,
        0,
        NULL,
        0
    };
    char *payload = telemetry_serialize(device, d);
    msg.payload = payload;
    msg.payloadlen = strlen(payload);
    int ret = MQTTPublish(&mqtt_client, p_topic, &msg);
    free(payload);
    return ret;
}

int mqtt_is_connect() {
    return mqtt_client.isconnected;
}
