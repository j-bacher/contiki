/*
 * Copyright (c) 2017, Jan Bacher <jabacher@gmail.com>
 * All rights reserved.
 * 
 * Using mqtt-sn library:
 * Copyright (c) 2014 IBM Corp.
 * 
 * borrows code from the mqtt client:
 * Copyright (c) 2014, Texas Instruments Incorporated - http://www.ti.com/
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/**
 * \addtogroup cc26xx-web-demo
 * @{
 *
 * \file
 *     A CC26XX-specific mqtt-sn client
 */
/*---------------------------------------------------------------------------*/
#include "contiki.h"
#include "contiki-net.h"
#include "rest-engine.h"
#include "board-peripherals.h"
#include "rf-core/rf-ble.h"
#include "simple-udp.h"
#include "net/ip/uip.h"
#include "net/ipv6/uip-icmp6.h"
#include "sys/etimer.h"
#include "sys/ctimer.h"
#include "lib/sensors.h"
#include "button-sensor.h"
#include "board-peripherals.h"
#include "cc26xx-web-demo.h"
#include "dev/leds.h"
#include "mqtt-sn-client.h"
#include "httpd-simple.h"
#include "MQTTSNPacket.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>

#define DEBUG_MQTTSN 1

#if DEBUG_MQTTSN == 1
#define DBG(...) printf(__VA_ARGS__)
#include <net/ip/uip-debug.h>
#else
#define DBG(...)
#endif /* DEBUG */

/*---------------------------------------------------------------------------*/
/* local  broker ip  */
static const char *broker_ip = "fd00::1";
static uip_ipaddr_t real_broker_ip;

#define SN_BROKER_PORT 1885U
#define APP_BUFFER_SIZE 120
static uint16_t sn_port = SN_BROKER_PORT;

// Define predefined topic IDs
#define SN_TOPICID_STATUS 		10
#define SN_TOPICID_BATTERY 		11
#define SN_TOPICID_ACC_GYRO		12
#define SN_TOPICID_TEMPERATURE 	13
#define SN_TOPICID_MISC 		14

#define SN_PING_TIME 60 * CLOCK_SECOND  // Ping time
#define SN_TIMEOUT SN_PING_TIME * 2 // timeout time, after this time the client will be thrown out from the broker

#define SN_CONNECT_TIMEOUT 5 * CLOCK_SECOND
#define SN_MAX_BACKOFF (1<<10)

static unsigned int packetid = 0;
static unsigned int backoff = 1;  // exponential backoff: 1  2  4  8 16  32  64 128  256  512 1024
                                    //    standard time 5s: 5 10 20 40 80 160 320 640 1280 2560 5120

/*---------------------------------------------------------------------------*/
static uip_ip6addr_t def_route;

/* Common resources */
extern resource_t res_leds;

/* Parent RSSI functionality */
extern int def_rt_rssi;

const static cc26xx_web_demo_sensor_reading_t *reading;

static struct simple_udp_connection mqtt_sn_conn;

static mqttsn_client_config_t snConf;

typedef enum {
	SN_STATE_UNCONNECTED,
	SN_STATE_INIT,
	SN_STATE_REGISTERED,
	SN_STATE_CONNECTING,
	SN_STATE_CONNECTED,
	SN_STATE_PUBLISHING,
	SN_STATE_NEWCONFIG,
	SN_STATE_CONFIG_ERROR,
	SN_STATE_ERROR 
} sn_state_t;

static sn_state_t state = SN_STATE_UNCONNECTED;

typedef enum {
	SN_KEEPALIVE_IDLE,
	SN_KEEPALIVE_SEND_PING,
	SN_KEEPALIVE_GOT_PONG,
	SN_KEEPALIVE_ERROR 
} sn_keepalive_t;

static sn_keepalive_t keep_alive = SN_KEEPALIVE_IDLE;

static struct etimer sn_publish_timer;
static struct ctimer sn_timeout_timer;
static struct ctimer sn_connect_timeout_timer;

/*---------------------------------------------------------------------------*/
/*
 * Buffers for Client ID.
 * Make sure they are large enough to hold the entire respective string
 *
 * We also need space for the null termination
 */
 
#define BUFFER_SIZE 64

static MQTTSNString clientID;

static char plain_id[BUFFER_SIZE];	// only address of sensor node (KW)

static void mqttsn_sendPacketBuffer(const uip_ipaddr_t* host, unsigned char* buf, int buflen);

static void mqtt_sn_receiver(struct simple_udp_connection *c,
					const uip_ipaddr_t *sender_addr,
					uint16_t sender_port, 
					const uip_ipaddr_t *receiver_addr,
					uint16_t receiver_port, 
					const uint8_t *data,
					uint16_t datalen) 
{   
	int packetType = 0;
	int len = 0, lenlen = 0;
	
	printf("Data received on port %d from port %d with length %d\n", receiver_port, sender_port, datalen);
	
	/* read the length.  This is a variable in itself */
	lenlen = MQTTSNPacket_decode((unsigned char*)data, datalen, &len);
	if (datalen != len)
		return; /* there was an error */

	packetType = data[lenlen]; /* return the packet type */
	
	switch(packetType) {
		case MQTTSN_ADVERTISE:
			DBG("APP - Application received a MQTT-SN advertisement\n");
			break;
			
		case MQTTSN_SEARCHGW:
			DBG("APP - Application received a MQTT-SN search GW\n");
			break;
			
		case MQTTSN_GWINFO:
			DBG("APP - Application received a MQTT-SN GW Info\n");
			break;
			
		case MQTTSN_RESERVED1:
			DBG("APP - Application received a MQTT-SN reserved1\n");
			break;
			
		case MQTTSN_CONNECT:
			DBG("APP - Application received a MQTT-SN connection request\n");
			break;
			
		case MQTTSN_CONNACK:
			DBG("APP - Application has a MQTT-SN connection\n");
			state = SN_STATE_CONNECTED;
            backoff = 1;
            process_post(&mqtt_sn_client_process, PROCESS_EVENT_CONTINUE, NULL);
			break;
			
		case MQTTSN_WILLTOPICREQ:
			DBG("APP - Application received a MQTT-SN willtopic request\n");
			break;
			
		case MQTTSN_WILLTOPIC:
			DBG("APP - Application received a MQTT-SN willtopic\n");
			break;
			
		case MQTTSN_WILLMSGREQ:
			DBG("APP - Application received a MQTT-SN willmsg request\n");
			break;
			
		case MQTTSN_WILLMSG:
			DBG("APP - Application received a MQTT-SN willmsg\n");
			break;
			
		case MQTTSN_REGISTER:
			DBG("APP - Application received a MQTT-SN register request\n");
			break;
			
		case MQTTSN_REGACK:
			DBG("APP - Application received a MQTT-SN register ack\n");
			break;
			
		case MQTTSN_PUBLISH:
			DBG("APP - Application received a MQTT-SN publish request\n");
			break;
			
		case MQTTSN_PUBACK:
			DBG("APP - Application received a MQTT-SN publish ack\n");
			break;
			
		case MQTTSN_PUBCOMP:
			DBG("APP - Application received a MQTT-SN publish compare\n");
			break;
			
		case MQTTSN_PUBREC:
			DBG("APP - Application received a MQTT-SN publish rec\n");
			break;
			
		case MQTTSN_PUBREL:
			DBG("APP - Application received a MQTT-SN publish rel\n");
			break;
			
		case MQTTSN_RESERVED2:
			DBG("APP - Application received a MQTT-SN reserved2\n");
			break;
			
		case MQTTSN_SUBSCRIBE:
			DBG("APP - Application received a MQTT-SN subscribe request\n");
			break;
			
		case MQTTSN_SUBACK:
			DBG("APP - Application received a MQTT-SN subscribe ack\n");
			break;
			
		case MQTTSN_UNSUBSCRIBE:
			DBG("APP - Application received a MQTT-SN unsubscribe request\n");
			break;
			
		case MQTTSN_UNSUBACK:
			DBG("APP - Application received a MQTT-SN unsubscribe ack\n");
			break;
			
		case MQTTSN_PINGREQ:
			DBG("APP - Application received a MQTT-SN ping request\n");
			
			if (uip_ipaddr_cmp(&real_broker_ip, sender_addr)) {
				unsigned char buf[10]; //should be enough
				int len = MQTTSNSerialize_pingresp(buf, sizeof(buf));
				if(len < 1) {
					printf("SN-pingreq: Buffer error");
					break;
				}
				mqttsn_sendPacketBuffer(&real_broker_ip, buf, len);
			} else {
				DBG("Not implemented yet"); //have to create new socket for this...
			}
			break;
			
		case MQTTSN_PINGRESP:
			DBG("APP - Application received a MQTT-SN pong response\n");
			if(keep_alive == SN_KEEPALIVE_SEND_PING)
				keep_alive = SN_KEEPALIVE_GOT_PONG;
			else {
				DBG("Got PINGRESP without ping? ERROR\n");
				//state = SN_STATE_UNCONNECTED; //reset connection!
                //process_post(&mqtt_sn_client_process, PROCESS_EVENT_CONTINUE, NULL);

			}
			break;
			
		case MQTTSN_DISCONNECT:
			DBG("APP - Application received a MQTT-SN disconnect request\n");
			state = SN_STATE_UNCONNECTED;
            process_post(&mqtt_sn_client_process, PROCESS_EVENT_CONTINUE, NULL);
			break;
			
		case MQTTSN_RESERVED3:
			DBG("APP - Application received a MQTT-SN reserved2\n");
			break;
			
		case MQTTSN_WILLTOPICUPD:
			DBG("APP - Application received a MQTT-SN willtopic update\n");
			break;
			
		case MQTTSN_WILLTOPICRESP:
			DBG("APP - Application received a MQTT-SN willtopic response\n");
			break;
			
		case MQTTSN_WILLMSGUPD:
			DBG("APP - Application received a MQTT-SN willmessage update\n");
			break;
			
		case MQTTSN_WILLMSGRESP:
			DBG("APP - Application received a MQTT-SN willmessage response\n");
			break;
		
		default:
			break;
	}
}
/*---------------------------------------------------------------------------*/
static int mqttsn_construct_plain_id(void)
{
	int len = snprintf(plain_id, BUFFER_SIZE, "%02x%02x%02x%02x%02x%02x",
				linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1],
				linkaddr_node_addr.u8[2], linkaddr_node_addr.u8[5],
				linkaddr_node_addr.u8[6], linkaddr_node_addr.u8[7]);

	/* len < 0: Error. Len >= BUFFER_SIZE: Buffer too small */
	if(len < 0 || len >= BUFFER_SIZE) {
		printf("Plain Client ID: %d, Buffer %d\n", len, BUFFER_SIZE);
		return 0;
	}
	writeMQTTSNString((unsigned char**)&plain_id, clientID);

	return 1;
}
/*---------------------------------------------------------------------------*/
static int mqttsn_init_config()
{
	/* Populate configuration with default values */
	//memset(snConf, 0, sizeof(mqttsn_client_config_t));

	memcpy(snConf.org_id, CC26XX_WEB_DEMO_DEFAULT_ORG_ID, 11);
	memcpy(snConf.type_id, CC26XX_WEB_DEMO_DEFAULT_TYPE_ID, 7);
	memcpy(snConf.event_type_id, CC26XX_WEB_DEMO_DEFAULT_EVENT_TYPE_ID, 7);
	memcpy(snConf.broker_ip, broker_ip, strlen(broker_ip));
	memcpy(snConf.cmd_type, CC26XX_WEB_DEMO_DEFAULT_SUBSCRIBE_CMD_TYPE, 1);

	snConf.broker_port = SN_BROKER_PORT;
	snConf.pub_interval = CC26XX_WEB_DEMO_DEFAULT_PUBLISH_INTERVAL;

	return 1;
}
/*---------------------------------------------------------------------------*/
static void mqttsn_sendPacketBuffer(const uip_ipaddr_t* host, unsigned char* buf, int buflen)
{
	if (simple_udp_sendto(&mqtt_sn_conn, buf, buflen, host) != 0)
		printf("socket error\n");
	return;
}
/*---------------------------------------------------------------------------*/
/*static void print_buffer(unsigned char* buffer, int datalen, int all) {
	for(int i=0; i < datalen || (all && buffer[i] != '\0'); i++) {
		printf("%2x ", buffer[i]);
		if(i == 0) continue; 
		if((i % 10) == 0) 
			printf("\n");
		else if((i % 5) == 0)
			printf("   ");
	}
}*/
/*---------------------------------------------------------------------------*/
static int sn_create_bat_packet(char* buf, int buf_len) {
	const char* volt = NULL;
	const char* temp = NULL;
	
	reading = cc26xx_web_demo_sensor_lookup(CC26XX_WEB_DEMO_SENSOR_BATMON_TEMP);
	if(reading->publish && reading->raw != CC26XX_SENSOR_READING_ERROR) {
		temp = reading->converted;
	}
	
	reading = cc26xx_web_demo_sensor_lookup(CC26XX_WEB_DEMO_SENSOR_BATMON_VOLT);
	if(reading->publish && reading->raw != CC26XX_SENSOR_READING_ERROR) {
		volt = reading->converted;
	}
	if(volt == NULL || temp == NULL)
		return -1;
	
	int len = snprintf(buf, buf_len, "{\"volt\":%s, \"temp\": %s}", volt, temp);

	if(len < 0 || len >= buf_len) {
		printf("Buffer too short. Have %d, need %d + \\0\n", buf_len, len);
		return -1;
	}
	return 0;
}
/*---------------------------------------------------------------------------*/
static int sn_create_temperature_packet(char* buf, int buf_len) {
	const char* air = NULL;
	const char* obj = NULL;
	const char* amb = NULL;
	const char* hdc = NULL;
	
	reading = cc26xx_web_demo_sensor_lookup(CC26XX_WEB_DEMO_SENSOR_BMP_TEMP);
	if(reading->publish && reading->raw != CC26XX_SENSOR_READING_ERROR) {
		air = reading->converted;
	}
	
	reading = cc26xx_web_demo_sensor_lookup(CC26XX_WEB_DEMO_SENSOR_TMP_OBJECT);
	if(reading->publish && reading->raw != CC26XX_SENSOR_READING_ERROR) {
		obj = reading->converted;
	}
	
	reading = cc26xx_web_demo_sensor_lookup(CC26XX_WEB_DEMO_SENSOR_HDC_TEMP);
	if(reading->publish && reading->raw != CC26XX_SENSOR_READING_ERROR) {
		hdc = reading->converted;
	}
	
	reading = cc26xx_web_demo_sensor_lookup(CC26XX_WEB_DEMO_SENSOR_TMP_AMBIENT);
	if(reading->publish && reading->raw != CC26XX_SENSOR_READING_ERROR) {
		amb = reading->converted;
	}
	
	if(air == NULL || obj == NULL || amb == NULL || hdc == NULL)
		return -1;
	
	int len = snprintf(buf, buf_len, "{\"Air\":%s, \"Obj\": %s, \"Amb\":%s, \"HDC\": %s}", air, obj, amb, hdc);

	if(len < 0 || len >= buf_len) {
		printf("Buffer too short. Have %d, need %d + \\0\n", buf_len, len);
		return -1;
	}
	
	return 0;
}

static int sn_create_misc_packet(char* buf, int buf_len) {
	const char* pres = NULL;
	const char* light = NULL;
	const char* hum = NULL;
	
	reading = cc26xx_web_demo_sensor_lookup(CC26XX_WEB_DEMO_SENSOR_BMP_PRES);
	if(reading->publish && reading->raw != CC26XX_SENSOR_READING_ERROR) {
		pres = reading->converted;
	}
	
	reading = cc26xx_web_demo_sensor_lookup(CC26XX_WEB_DEMO_SENSOR_OPT_LIGHT);
	if(reading->publish && reading->raw != CC26XX_SENSOR_READING_ERROR) {
		light = reading->converted;
	}
	
	reading = cc26xx_web_demo_sensor_lookup(CC26XX_WEB_DEMO_SENSOR_HDC_HUMIDITY);
	if(reading->publish && reading->raw != CC26XX_SENSOR_READING_ERROR) {
		hum = reading->converted;
	}
	
	if(pres == NULL || light == NULL || hum == NULL)
		return -1;
	
	int len = snprintf(buf, buf_len, "{\"AirPres\":%s, \"AmbLight\": %s, \"Humi\":%s}", pres, light, hum);

	if(len < 0 || len >= buf_len) {
		printf("Buffer too short. Have %d, need %d + \\0\n", buf_len, len);
		return -1;
	}
	return 0;
}

static void sn_keep_alive(void *ptr) {
	if (keep_alive == SN_KEEPALIVE_GOT_PONG || keep_alive == SN_KEEPALIVE_IDLE) {
		if(state == SN_STATE_CONNECTED) {
			unsigned char buf[BUFFER_SIZE+4]; //should be enough
			int len = MQTTSNSerialize_pingreq(buf, sizeof(buf), clientID);
			if(len < 1) {
				printf("SN-pingreq: Buffer error\n");
			} else {
				DBG("Sending PING\n");
				mqttsn_sendPacketBuffer(&real_broker_ip, buf, len);
				keep_alive = SN_KEEPALIVE_SEND_PING;
			}
		} else {
                keep_alive = SN_KEEPALIVE_IDLE;
        }
	} else {
		printf("Didn't get a ping reponse in timer interval... resetting!\n");
		state = SN_STATE_UNCONNECTED;
        keep_alive = SN_KEEPALIVE_IDLE;
        process_post(&mqtt_sn_client_process, PROCESS_EVENT_CONTINUE, NULL);
	}
	
	ctimer_reset(&sn_timeout_timer);
	return;
}

static void sn_reset_connection() {
    if(state == SN_STATE_CONNECTING) {
        DBG("SN: Timeout. Reseting connection\n");
        state = SN_STATE_UNCONNECTED;
        keep_alive = SN_KEEPALIVE_IDLE;
        if(backoff <= SN_MAX_BACKOFF)
            backoff <<= 1;
        process_post(&mqtt_sn_client_process, PROCESS_EVENT_CONTINUE, NULL);
    }
}

/*---------------------------------------------------------------------------*/
PROCESS(mqtt_sn_client_process, "CC26XX mqtt-sn client");
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(mqtt_sn_client_process, ev, data)
{
	static unsigned char buf[APP_BUFFER_SIZE];
	static char appbuffer[APP_BUFFER_SIZE];
	int buflen = sizeof(buf);

	MQTTSN_topicid snTopic;
	int dup = 0, qos = 0, retained = 1, datalen;

	MQTTSNPacket_connectData options = MQTTSNPacket_connectData_initializer;

	PROCESS_BEGIN();

	printf("CC26XX MQTT-SN Client\n");

	DBG("SN: Debug enabled\n");

	mqttsn_init_config();

	mqttsn_construct_plain_id();

	uiplib_ip6addrconv(broker_ip, &real_broker_ip);

	if(simple_udp_register(&mqtt_sn_conn, 0, NULL, sn_port, mqtt_sn_receiver) != 1) {
		printf("MQTT-SN: Error getting socket. Exit\n");
		PROCESS_EXIT();
	}

#if DEBUG_MQTTSN == 1
	printf("Plain ID:    %s\n", plain_id);
	printf("Broker Port: %" PRIu16 "\n", snConf.broker_port);
	printf("Broker Port: %" PRIu16 "\n", sn_port);
	printf("Broker IP: %s\n", broker_ip);
	printf("Real IP: ");
	uip_debug_ipaddr_print(&real_broker_ip);
	DBG("\n");
#endif

	//start publish timer:
	etimer_set(&sn_publish_timer, snConf.pub_interval);
	
	//start timeout callback timer
	ctimer_set(&sn_timeout_timer, SN_PING_TIME, sn_keep_alive, NULL);

	/* Define application-specific events here. */
	while(1) {
		if(state == SN_STATE_UNCONNECTED) {
			DBG("SN: unconnected...\n");
			options.clientID.cstring = plain_id;
			options.duration = SN_TIMEOUT;
			datalen = MQTTSNSerialize_connect(buf, buflen, &options);
  
			DBG("connect packet length: %d\n", datalen);
/*#if DEBUG_MQTTSN == 1
			print_buffer(buf, datalen, 1);
#endif*/

			mqttsn_sendPacketBuffer(&real_broker_ip, buf, datalen);
		
			state = SN_STATE_CONNECTING;
            
            //start timeout callback timer
            ctimer_set(&sn_connect_timeout_timer, backoff * SN_CONNECT_TIMEOUT, sn_reset_connection, NULL);
		}
		else if(state == SN_STATE_CONNECTED) {
			if( (ev == PROCESS_EVENT_TIMER && data == &sn_publish_timer) ||
			ev == PROCESS_EVENT_POLL ||
			ev == cc26xx_web_demo_publish_event ||
			(ev == sensors_event && data == CC26XX_WEB_DEMO_MQTT_PUBLISH_TRIGGER)) 
			{

				char* buf_ptr = appbuffer;

				int len;
				int remaining = APP_BUFFER_SIZE;
				char def_rt_str[64];
				
				etimer_reset(&sn_publish_timer);
				
				leds_on(LEDS_GREEN);

				DBG("SN: START STATUS\n");

				len = snprintf(buf_ptr, remaining,
							"{\"Seq\":%d,\"Uptime\":%lu",
							 packetid / 4, clock_seconds());

				if(len < 0 || len >= remaining) {
					printf("Buffer too short. Have %d, need %d + \\0\n", remaining, len);
					leds_off(LEDS_GREEN);
					continue;
				}

				remaining -= len;
				buf_ptr += len;

				/* Put our Default route's string representation in a buffer */
				memset(def_rt_str, 0, sizeof(def_rt_str));
				cc26xx_web_demo_ipaddr_sprintf(def_rt_str, sizeof(def_rt_str),
											 uip_ds6_defrt_choose());

				len = snprintf(buf_ptr, remaining, ",\"Route\":\"%s\",\"RSSI\":%d",
							 def_rt_str, def_rt_rssi);

				if(len < 0 || len >= remaining) {
					printf("Buffer too short. Have %d, need %d + \\0\n", remaining, len);
					leds_off(LEDS_GREEN);
					continue;
				}
				remaining -= len;
				buf_ptr += len;

				memcpy(&def_route, uip_ds6_defrt_choose(), sizeof(uip_ip6addr_t));

				len = snprintf(buf_ptr, remaining, "}");

				if(len < 0 || len >= remaining) {
					printf("Buffer too short. Have %d, need %d + \\0\n", remaining, len);
					leds_off(LEDS_GREEN);
					continue;
				}

				snTopic.type = 	MQTTSN_TOPIC_TYPE_PREDEFINED;
				snTopic.data.id = SN_TOPICID_STATUS;
				len = MQTTSNSerialize_publish(buf, buflen, dup, qos, retained, packetid,
					snTopic, (unsigned char*)appbuffer, strlen(appbuffer));
				mqttsn_sendPacketBuffer(&real_broker_ip, buf, len);
				packetid++;

				DBG("APP - STATUS Publish!\n");
				
				//TODO: Function Pointer List? (seems messy...)
				
				DBG("APP - BAT!\n");
				
				buf_ptr = appbuffer;
				remaining = APP_BUFFER_SIZE;
				
				if(sn_create_bat_packet(buf_ptr, remaining) == 0) {
					snTopic.type = 	MQTTSN_TOPIC_TYPE_PREDEFINED;
					snTopic.data.id = SN_TOPICID_BATTERY;
					len = MQTTSNSerialize_publish(buf, buflen, dup, qos, retained, packetid,
						snTopic, (unsigned char*)appbuffer, strlen(appbuffer));
					mqttsn_sendPacketBuffer(&real_broker_ip, buf, len);
					packetid++;
					
					DBG("APP - BAT Publish!\n"); 
				}
				
				DBG("APP - Misc!\n");
				
				buf_ptr = appbuffer;
				remaining = APP_BUFFER_SIZE;
				
				if(sn_create_misc_packet(buf_ptr, remaining) == 0)
				{
					snTopic.type = 	MQTTSN_TOPIC_TYPE_PREDEFINED;
					snTopic.data.id = SN_TOPICID_MISC;
					len = MQTTSNSerialize_publish(buf, buflen, dup, qos, retained, packetid,
						snTopic, (unsigned char*)appbuffer, strlen(appbuffer));
					mqttsn_sendPacketBuffer(&real_broker_ip, buf, len);
					packetid++;
					
					DBG("APP - Misc Publish!\n");
				}
                				
				//wait, so buffer can be send (workaround?)
				DBG("Waiting for send...");
				static struct etimer et;
				etimer_set(&et, CLOCK_SECOND * 2);
				PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
				
				//if you need this, implement it yourself!
				/*DBG("APP - Acc / Gyro!\n");
				
				buf_ptr = appbuffer;
				remaining = APP_BUFFER_SIZE;
				
				if(sn_create_accgyro_packet(buf_ptr, remaining) == 0)
				{
					snTopic.type = 	MQTTSN_TOPIC_TYPE_PREDEFINED;
					snTopic.id = SN_TOPICID_ACC_GYRO;
					len = MQTTSNSerialize_publish(buf, buflen - len, dup, qos, retained, packetid,
						snTopic, (unsigned char*)appbuffer, strlen(appbuffer));
					mqttsn_sendPacketBuffer(&real_broker_ip, buf, len);
					packetid++;
					DBG("APP - Acc / Gyro Publish!\n");
				}
				*/
				
				DBG("APP - Temperature!\n");
				
				buf_ptr = appbuffer;
				remaining = APP_BUFFER_SIZE;
				
				if(sn_create_temperature_packet(buf_ptr, remaining) == 0)
				{
					snTopic.type = 	MQTTSN_TOPIC_TYPE_PREDEFINED;
					snTopic.data.id = SN_TOPICID_TEMPERATURE;
					len = MQTTSNSerialize_publish(buf, buflen, dup, qos, retained, packetid,
						snTopic, (unsigned char*)appbuffer, strlen(appbuffer));
					mqttsn_sendPacketBuffer(&real_broker_ip, buf, len);
					packetid++;
					
					DBG("APP - Temperature Publish!\n");
				}
				
				leds_off(LEDS_GREEN);
			} else {
				if(etimer_expired(&sn_publish_timer)) {
					DBG("SN Timer expired but event not fired. Restarting");
					etimer_restart(&sn_publish_timer);
				}
			}
		}
        DBG("SN: WAITING...\n");
		PROCESS_YIELD();
	}

	PROCESS_END();
}
/*---------------------------------------------------------------------------*/
/**
 * @}
 */
