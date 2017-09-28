#ifndef _USER_CONFIG_
#define _USER_CONFIG_

#define WIFI_SSID            "ssid"
#define WIFI_PASSWORD        "password"

#define WIFI_AP_SSID         "MyAP"
#define WIFI_AP_PASSWORD     "none"

#define MAX_CLIENTS	     8

//
// Here the MQTT stuff
//

// Define this if you want to have it work as a MQTT client
#define MQTT_CLIENT 	1	

// Define this if you need SSL for the *MQTT client*
//#define MQTT_SSL_ENABLE	1

#define MQTT_BUF_SIZE   1024
#define MQTT_KEEPALIVE    120  /*seconds*/
#define MQTT_RECONNECT_TIMEOUT  5 /*seconds*/
//#define PROTOCOL_NAMEv31  /*MQTT version 3.1 compatible with Mosquitto v0.15*/
#define PROTOCOL_NAMEv311     /*MQTT version 3.11 compatible with https://eclipse.org/paho/clients/testing/*/

#define MQTT_ID "ESPBroker"

//
// Define this if you want to have script support.
//
#define SCRIPTED  1

// Some params for scripts

#define MAX_SCRIPT_SIZE 0x1000
#define MAX_TIMERS	4
#define MAX_GPIOS	3
#define PWM_MAX_CHANNELS 8
#define MAX_VARS	10
#define DEFAULT_VAR_LEN	16
#define MAX_TIMESTAMPS	6
#define MAX_FLASH_SLOTS	8
#define FLASH_SLOT_LEN	64

//
// Define this if you want to have GPIO OUT support in scripts.
//
#define GPIO	  1

//
// Define this if you want to have additionally GPIO PWM support in scripts.
//
#define GPIO_PWM  1

//
// Define this if you want to have NTP support.
//
#define NTP	  1

//
// Define this if you want to have mDNS support in scripts.
//
#define MDNS	  1

//
// Define this to support the "scan" command for AP search
//
#define ALLOW_SCANNING      1

//
// Define this if you want to have access to the config console via TCP.
// Otherwise only local access via serial is possible
//
#define REMOTE_CONFIG      1
#define CONSOLE_SERVER_PORT  7777

//
// Size of the console buffers
//
#define MAX_CON_SEND_SIZE    1024
#define MAX_CON_CMD_SIZE     80

typedef enum {SIG_DO_NOTHING=0, SIG_START_SERVER=1, SIG_UART0, SIG_TOPIC_RECEIVED, SIG_SCRIPT_LOADED, SIG_CONSOLE_TX_RAW, SIG_CONSOLE_TX, SIG_CONSOLE_RX} USER_SIGNALS;

#define LOCAL_ACCESS 0x01
#define REMOTE_ACCESS 0x02

#endif /* _USER_CONFIG_ */
