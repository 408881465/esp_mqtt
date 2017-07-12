#include "mqtt_server.h"

extern MQTT_Client mqttClient;
extern bool mqtt_enabled, mqtt_connected;

typedef enum {INIT, RE_INIT, TOPIC_LOCAL, TOPIC_REMOTE, TIMER} Interpreter_Status;

bool is_token(int i, char *s);
int search_token(int i, char *s);
int syntax_error(int i, char *message);

int parse_statement(int next_token);
int parse_event(int next_token, bool *happened);
int parse_action(int next_token, bool doit);
int parse_topic(int next_token, char **topic);
int parse_value(int next_token, char **data, int *data_len);

int interpreter_init(char *str);
int interpreter_init_reconnect(void);
int interpreter_topic_received(char *topic, char *data, int data_len, bool local);

void test_tokens(void);
