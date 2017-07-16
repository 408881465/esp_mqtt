#ifdef esp8266
#include "c_types.h"
#include "mem.h"
#include "osapi.h"

#define lang_debug //os_printf
#define lang_info os_printf

#else
#include <stdlib.h>
#include "stdio.h"
#include "string.h"
#define os_printf printf
#define os_malloc malloc
#define os_strcmp strcmp
#define os_strcpy strcpy
#define os_strlen strlen

#define false 0
#define true 1
#define bool int
#define ICACHE_FLASH_ATTR
#endif

#include "lang.h"

#include "mqtt_topics.h"


char **my_token;
int max_token;
bool script_enabled = false;
bool in_topic_statement;
Interpreter_Status interpreter_status;
char *interpreter_topic;
char *interpreter_data;
int interpreter_data_len;


void ICACHE_FLASH_ATTR test_tokens(void){
   int i;

   for (i = 0; i<max_token; i++) {
	lang_debug("<%s>", my_token[i]);
   }
   lang_debug("\r\n");
}


int ICACHE_FLASH_ATTR text_into_tokens(char *str)
{
char    *p, *q;
int     token_count = 0;
bool    in_token = false;

   // preprocessing
   lang_debug("lexxer preprocessing\r\n");

   for (p = q = str; *p != 0; p++) {
	if (*p == '\\') {
	   // next char is quoted, copy that - skip this one
	   if (*(p+1) != 0) *q++ = *++p;
	} else if (*p == '\"') {
	   // string quotation
	   in_token = !in_token;
	   *q++ = 1;
	} else if (*p == '%' && !in_token) {
	   // comment till eol
	   for (; *p != 0; p++)
	      if (*p == '\n') break;
	} else if (*p <= ' ' && !in_token) {
	   // mark this as whitespace
	   *q++ = 1;
	} else {
	   *q++ = *p;
	}
   }
   *q = 0;

   // eliminate double whitespace and count tokens
   lang_debug("lexxer whitespaces\r\n");

   in_token = false;
   for (p = q = str; *p != 0; p++) {
	if (*p == 1) {
	   if (in_token) {
		*q++ = 1;
		in_token = false;
	   }
	} else {
	   if (!in_token) {
		token_count++;
		in_token = true;
	   }
	   *q++ = *p;
	}
   }
   *q = 0;

   lang_debug("found %d tokens\r\n", token_count);
   my_token = (char **)os_malloc(token_count * sizeof(char *));
   if (my_token == 0) return 0;

   // assign tokens
   lang_debug("lexxer tokenize\r\n");

   in_token = false;
   token_count = 0;
   for (p = str; *p != 0; p++) {
	if (*p == 1) {
	   *p = '\0';
	   in_token = false;
	} else {
	   if (!in_token) {
		my_token[token_count++] = p;
		in_token = true;
	   }
	}
   }

   max_token = token_count;
   return max_token;
}


void ICACHE_FLASH_ATTR free_tokens(void){
   if (my_token != NULL) os_free((uint32_t *)my_token);
   my_token = NULL;
}


bool ICACHE_FLASH_ATTR is_token(int i, char *s)
{
   if (i >= max_token) return false;
//os_printf("cmp: %s %s\r\n", s, my_token[i]);
   return os_strcmp(my_token[i], s)==0;
}


int ICACHE_FLASH_ATTR search_token(int i, char *s)
{
   for (; i < max_token; i++)
	if (is_token(i, s)) return i;
   return -1;
}


int ICACHE_FLASH_ATTR syntax_error(int i, char *message)
{
int j;

   os_sprintf(sytax_error_buffer, "Error (%s) at >>", message);
   for (j = i; j < i+5 && j < max_token; j++) {
	int pos = os_strlen(sytax_error_buffer);
	if (sizeof(sytax_error_buffer)-pos-2 > os_strlen(my_token[j])) {
	    os_sprintf(sytax_error_buffer+pos, "%s ", my_token[j]);
	}
   }
   return -1;
}


int ICACHE_FLASH_ATTR parse_statement(int next_token)
{
  while (next_token < max_token) {

    in_topic_statement = false;

    if (is_token(next_token, "on")) {
   	bool event_happened;
	lang_debug("statement on\r\n");
	if ((next_token = parse_event(next_token+1, &event_happened)) == -1) return -1;
	if (!is_token(next_token, "do"))
	   return syntax_error(next_token, "'do' expected");   
	if ((next_token = parse_action(next_token+1, event_happened)) == -1) return -1;
    } else {
	return syntax_error(next_token, "'on' expected");
    }
  }
  return next_token;
}


int ICACHE_FLASH_ATTR parse_event(int next_token, bool *happend)
{
   *happend = false;

   if (is_token(next_token, "init")) {
	if (next_token+1 >= max_token) syntax_error(next_token+1, "end of text");
	*happend = (interpreter_status==INIT || interpreter_status==RE_INIT);
	return next_token+1;
   }

   if (is_token(next_token, "topic")) {
	lang_debug("event topic\r\n");

	in_topic_statement = true;
	if (next_token+2 >= max_token) return syntax_error(next_token+2, "end of text");
	if (is_token(next_token+1, "remote")) {
	  if (interpreter_status!=TOPIC_REMOTE) return next_token+3;
	} else if (is_token(next_token+1, "local")) {
	  if (interpreter_status!=TOPIC_LOCAL) return next_token+3;
	} else {
	   return syntax_error(next_token+1, "'local' or 'remote' expected");
	}

	*happend = Topics_matches(my_token[next_token+2], true, interpreter_topic);

        lang_info("topic %s %s %s %s\r\n", my_token[next_token+1], my_token[next_token+2], interpreter_topic, *happend?"match":"no match");
	return next_token+3;
   }
/*
   if (is_token(next_token, "timer")) {
	if (next_token+1 >= max_token) syntax_error(next_token+1, "end of text");
if (interpreter_status==TIMER) {os_printf("timer %s\r\n", my_token[next_token+1]); *happend=true;}
	return next_token+2;
   }
*/
   return syntax_error(next_token, "'init', 'topic', or 'timer' expected");
}


int ICACHE_FLASH_ATTR parse_action(int next_token, bool doit)
{
  while (next_token < max_token && !is_token(next_token, "on")) {
   lang_debug("action %s %s\r\n", my_token[next_token], doit?"do":"ignore");

   if (is_token(next_token, "subscribe")) {
	bool retval;

        if (next_token+2 >= max_token) return syntax_error(next_token+2, "end of text");
	if (is_token(next_token+1, "remote")) {
	   if (doit && mqtt_connected) {
		retval = MQTT_Subscribe(&mqttClient, my_token[next_token+2], 0);
		lang_info("subsrcibe remote %s %s\r\n", my_token[next_token+2], retval?"success":"failed");
	   }
	} else if (is_token(next_token+1, "local")) {
	   if (doit && interpreter_status!=RE_INIT) {
		retval = MQTT_local_subscribe(my_token[next_token+2], 0);
		lang_info("subsrcibe local %s %s\r\n", my_token[next_token+2], retval?"success":"failed");
	   }
	} else {
	   return syntax_error(next_token+1, "'local' or 'remote' expected");
	}
	next_token += 3;
   }

   else if (is_token(next_token, "unsubscribe")) {
	bool retval;

        if (next_token+2 >= max_token) return syntax_error(next_token+2, "end of text");
	if (is_token(next_token+1, "remote")) {
	   if (doit && mqtt_connected) {
		retval = MQTT_UnSubscribe(&mqttClient, my_token[next_token+2]);
		lang_info("unsubsrcibe remote %s %s\r\n", my_token[next_token+2], retval?"success":"failed");
	   }
	} else if (is_token(next_token+1, "local")) {
	   if (doit && interpreter_status!=RE_INIT) {
		retval = MQTT_local_unsubscribe(my_token[next_token+2]);
		lang_info("unsubsrcibe local %s %s\r\n", my_token[next_token+2], retval?"success":"failed");
	   }
	} else {
	   return syntax_error(next_token+1, "'local' or 'remote' expected");
	}
	
	next_token += 3;
   }

   else if (is_token(next_token, "publish")) {
	bool retval;
	int incr = 4;
	bool retained = false;
	char *topic;
	char *data;
	int data_len;

        if (next_token+3 >= max_token) return syntax_error(next_token+3, "end of text");
	if (Topics_hasWildcards(my_token[2])) return syntax_error(next_token+2, "wildcards in topic");
	if (next_token+4 < max_token && is_token(next_token+4, "retained")) {
	   incr = 5;
	   retained = true;
	}

	if (doit) {
	   parse_topic(next_token+2, &topic);
	   parse_value(next_token+3, &data, &data_len);
	}

	if (is_token(next_token+1, "remote")) {
	   if (doit && mqtt_connected) {
		MQTT_Publish(&mqttClient, topic, data, data_len, 0, retained);
		lang_info("published remote %s len: %d\r\n", topic, data_len);
	   }
	} else if (is_token(next_token+1, "local")) {
	   if (doit && interpreter_status!=RE_INIT) {
		MQTT_local_publish(topic, data, data_len, 0, retained);
		lang_info("published local %s len: %d\r\n", topic, data_len);
	   }
	} else {
	   return syntax_error(next_token+1, "'local' or 'remote' expected");
	}
	
	next_token += incr;
   }

/*   else if (is_token(next_token, "settimer")) {
	if (next_token+2 >= max_token) syntax_error(next_token+2, "end of text");
if (doit) os_printf("settimer %s %s\r\n", my_token[next_token+1], my_token[next_token+2]);
	next_token += 3;
   }
*/
   else
	return syntax_error(next_token, "action command expected");
	
  }
  return next_token;
}


int ICACHE_FLASH_ATTR parse_topic(int next_token, char **topic)
{
   if (is_token(next_token, "this_topic")) {
	lang_debug("val this_topic\r\n");
	if (!in_topic_statement) return syntax_error(next_token, "undefined topic");
	*topic = interpreter_topic;
	return next_token+1;
   } else {
	*topic = my_token[next_token];
	return next_token+1;
   }
}


int ICACHE_FLASH_ATTR parse_value(int next_token, char **data, int *data_len)
{
static char val_buf[64];

   if (is_token(next_token, "this_data")) {
	lang_debug("val this_data\r\n");
	if (!in_topic_statement) return syntax_error(next_token, "undefined topic data");
	*data = interpreter_data;
	*data_len = interpreter_data_len;
	return next_token+1;
   }

/*   else if (my_token[next_token][0] == '\'') {
	char *p = &(my_token[next_token][1]);
	int *b = (int*)&val_buf[0];

	*b = atoi(htonl(p));
	*data = val_buf;
	*data_len = sizeof(int);
	return next_token+1;
   }
*/
   else if (my_token[next_token][0] == '#') {
	int i, j, a;
	char *p = &(my_token[next_token][1]);

	lang_debug("val hexbinary\r\n");
	if (os_strlen(p)/2 > sizeof(val_buf)) return syntax_error(next_token, "binary sting too long");
	for (i = 0, j = 0; i < os_strlen(p); i+=2, j++) {
	   if (p[i] <= '9')
		a = p[i] - '0';
	   else
		a = toupper(p[i]) - 'A'+10;
	   a <<= 4;
	   if (p[i+1] <= '9')
		a += p[i+1] - '0';
	   else
		a += toupper(p[i+1]) - 'A'+10;
	   val_buf[j] = a;
	}
 
	*data = val_buf;
	*data_len = j;
	return next_token+1;
   }

   else {
	*data = my_token[next_token];
	*data_len = os_strlen(my_token[next_token]);
	return next_token+1;
   }
}

int ICACHE_FLASH_ATTR interpreter_syntax_check()
{
   lang_debug("interpreter_syntax_check\r\n");

   os_sprintf(sytax_error_buffer, "Syntax okay");
   interpreter_status = SYNTAX_CHECK;
   interpreter_topic = interpreter_data ="";
   interpreter_data_len = 0;
   return parse_statement(0);
}

int ICACHE_FLASH_ATTR interpreter_init()
{
   if (!script_enabled) return -1;

   lang_debug("interpreter_init\r\n");

   interpreter_status = INIT;
   interpreter_topic = interpreter_data ="";
   interpreter_data_len = 0;
   return parse_statement(0);
}


int ICACHE_FLASH_ATTR interpreter_init_reconnect(void)
{
   if (!script_enabled) return -1;

   lang_debug("interpreter_init_reconnect\r\n");

   interpreter_status = RE_INIT;
   interpreter_topic = interpreter_data ="";
   interpreter_data_len = 0;
   return parse_statement(0);
}


int ICACHE_FLASH_ATTR interpreter_topic_received(const char *topic, const char *data, int data_len, bool local)
{
   if (!script_enabled) return -1;

   lang_debug("interpreter_topic_received\r\n");

   interpreter_status = (local)?TOPIC_LOCAL:TOPIC_REMOTE;
   interpreter_topic = (char *)topic;
   interpreter_data = (char *)data;
   interpreter_data_len = data_len;

   return parse_statement(0);
}


#ifndef esp8266
int main()
{

char *str = malloc(strlen(prog+1));
strcpy(str, prog);
test_tokens(str);
}
#endif
