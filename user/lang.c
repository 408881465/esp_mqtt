#include "c_types.h"
#include "mem.h"
#include "osapi.h"
#include "lang.h"
#include "user_config.h"
#include "config_flash.h"
#include "mqtt/mqtt_topics.h"
#include "mqtt/mqtt_retainedlist.h"
#ifdef NTP
#include "ntp.h"
#endif

#ifdef GPIO
#include "easygpio.h"
#ifdef GPIO_PWM
#include "pwm.h"
#endif
#endif

#ifdef ADC
#include "adc.h"
#endif

#ifdef JSON_PARSE
#include "json_path.h"
#endif

#define lang_debug	//os_printf

#define lang_log(...) 	{if (lang_logging){char log_buffer[256]; os_sprintf (log_buffer, "%s: ", get_timestr()); con_print(log_buffer); os_sprintf (log_buffer, __VA_ARGS__); con_print(log_buffer);}}
//#define lang_log	//os_printf

extern uint8_t *my_script;
extern void do_command(char *t1, char *t2, char *t3);
extern void con_print(uint8_t *str);

static char EOT[] = "end of text";
#define len_check(x) \
if (interpreter_status==SYNTAX_CHECK && next_token+(x) >= max_token) \
  return syntax_error(next_token+(x), EOT)
#define syn_chk (interpreter_status==SYNTAX_CHECK)

typedef enum {INVALID = 0, HAPPENED, NOT_HAPPENED, UNDEFINED} Alarm_State;
typedef struct _timestamp_entry_t {
    uint8_t ts[9];
    Alarm_State state;
} timestamp_entry_t;

#ifdef GPIO
typedef struct _gpio_entry_t {
    os_timer_t inttimer;
    uint8_t no;
    bool val;
} gpio_entry_t;
static gpio_entry_t gpios[MAX_GPIOS];
int gpio_counter;

#ifdef GPIO_PWM
static uint8_t pwm_channels[PWM_MAX_CHANNELS];
int pwm_counter;
#endif
#endif

bool lang_logging = false;
char **my_token;
int max_token;
bool script_enabled = false;
bool in_topic_statement;
Interpreter_Status interpreter_status;
char *interpreter_topic;
char *interpreter_data;
int interpreter_data_len;
int interpreter_timer;
int interpreter_timestamp;
#ifdef GPIO
bool in_gpio_statement;
int interpreter_gpio;
int interpreter_gpioval;
#endif
#ifdef HTTPC
bool in_http_statement;
int interpreter_http_status;

void interpreter_http_reply(char *response_body, int http_status, char *response_headers, int body_size);
#endif

static os_timer_t timers[MAX_TIMERS];
var_entry_t vars[MAX_VARS];
static timestamp_entry_t timestamps[MAX_TIMESTAMPS];

var_entry_t ICACHE_FLASH_ATTR *find_var(const uint8_t *name, var_entry_t **free_var) {
    int i;

    *free_var = NULL;
    for (i = 0; i<MAX_VARS; i++) {
	if (!vars[i].free) {
	    if (os_strncmp(name, vars[i].name, 14) == 0) {
		lang_debug("var %s found at %d\r\n", vars[i].name, i);
		return &vars[i];
	    }
	} else {
	    if (*free_var == NULL)
		*free_var = &vars[i];
	}
    }
    return NULL;
}

static void ICACHE_FLASH_ATTR lang_timers_timeout(void *arg) {

    interpreter_timer = (int)arg;
    os_timer_disarm(&timers[interpreter_timer]);
    if (!script_enabled)
	return;

    lang_debug("timer %d expired\r\n", interpreter_timer + 1);

    interpreter_topic = interpreter_data = "";
    interpreter_data_len = 0;
    interpreter_status = TIMER;
    parse_statement(0);
}

void ICACHE_FLASH_ATTR check_timestamps(uint8_t * curr_time) {
    int i;

    if (!script_enabled)
	return;

    for (i = 0; i < MAX_TIMESTAMPS; i++) {
	if (timestamps[i].state == INVALID)
	    continue;

	if (os_strcmp(curr_time, timestamps[i].ts) >= 0) {
	    if (timestamps[i].state == UNDEFINED) {
		timestamps[i].state = HAPPENED;
		continue;
	    }
	    if (timestamps[i].state == HAPPENED)
		continue;
	    lang_debug("timerstamp %s happened\r\n", timestamps[i].ts);

	    interpreter_topic = interpreter_data = "";
	    interpreter_data_len = 0;
	    interpreter_status = ALARM;
	    interpreter_timestamp = i;
	    parse_statement(0);
	    timestamps[i].state = HAPPENED;
	} else {
	    timestamps[i].state = NOT_HAPPENED;
	}
    }
}

#ifdef GPIO
// GPIO int timer - debouncing
void ICACHE_FLASH_ATTR inttimer_func(void *arg){

	gpio_entry_t *my_gpio_entry = (gpio_entry_t *)arg;
	interpreter_gpioval = easygpio_inputGet(my_gpio_entry->no);

	//os_printf("GPIO %d %d\r\n", my_gpio_entry->no, interpreter_gpioval);

        // Reactivate interrupts for GPIO
        gpio_pin_intr_state_set(GPIO_ID_PIN(my_gpio_entry->no), GPIO_PIN_INTR_ANYEDGE);

	if (script_enabled) {
	    lang_debug("interpreter GPIO %d %d\r\n", my_gpio_entry->no, interpreter_gpioval);

	    interpreter_status = GPIO_INT;
	    interpreter_topic = interpreter_data = "";
	    interpreter_data_len = 0;
	    interpreter_gpio = my_gpio_entry->no;
	    my_gpio_entry->val = interpreter_gpioval;
	    parse_statement(0);
	}
}

// Interrupt handler - this function will be executed on any edge of a GPIO
LOCAL void gpio_intr_handler(void *arg)
{
    gpio_entry_t *my_gpio_entry = (gpio_entry_t *)arg;

    uint32 gpio_status = GPIO_REG_READ(GPIO_STATUS_ADDRESS);

    if (gpio_status & BIT(my_gpio_entry->no)) {

        // Disable interrupt for GPIO
        gpio_pin_intr_state_set(GPIO_ID_PIN(my_gpio_entry->no), GPIO_PIN_INTR_DISABLE);

        // Clear interrupt status for GPIO
        GPIO_REG_WRITE(GPIO_STATUS_W1TC_ADDRESS, gpio_status & BIT(my_gpio_entry->no));

	// Start the timer
    	os_timer_setfn(&my_gpio_entry->inttimer, inttimer_func, my_gpio_entry);
    	os_timer_arm(&my_gpio_entry->inttimer, 50, 0); 
    }
}

void ICACHE_FLASH_ATTR init_gpios() {
    int i;

    if (!script_enabled)
	return;
    for (i = 0; i < gpio_counter; i++) {
	gpio_pin_intr_state_set(GPIO_ID_PIN(gpios[i].no), GPIO_PIN_INTR_ANYEDGE);
    }
}

void ICACHE_FLASH_ATTR stop_gpios() {
    int i;

    for (i = 0; i < gpio_counter; i++) {
	gpio_pin_intr_state_set(GPIO_ID_PIN(gpios[i].no), GPIO_PIN_INTR_DISABLE);
    }
}

#ifdef GPIO_PWM
int32_t ICACHE_FLASH_ATTR pwm_channel_from_pin(int pin) {

    int32_t i;
    for (i = 0; i < pwm_counter; i++) {
	if (pin == pwm_channels[i])
	    return i;
    }
    return -1;
}

void ICACHE_FLASH_ATTR init_pwm() {
    uint32 io_info[pwm_counter][3];
    int32_t i;

    if (!script_enabled || pwm_counter == 0)
	return;

    for (i = 0; i < pwm_counter; i++) {
	io_info[i][2] = pwm_channels[i];
    }

    // initial duty: all off
    pwm_init(config.pwm_period, NULL, pwm_counter, io_info);
    pwm_start();
}
#endif /* GPIO_PWM */
#endif /* GPIO */

void ICACHE_FLASH_ATTR test_tokens(void) {
    int i;

    for (i = 0; i < max_token; i++) {
	lang_debug("<%s>", my_token[i]);
    }
    lang_debug("\r\n");
}

int ICACHE_FLASH_ATTR text_into_tokens(char *str) {
    char *p, *q;
    int token_count = 0;
    bool in_token = false;

    // preprocessing
    lang_debug("lexxer preprocessing (prog_len: %d)\r\n", os_strlen(str));

    for (p = q = str; *p != 0; p++) {

	if (*p == '\\') {
	    // next char is quoted, copy that - skip this one
	    if (*(p + 1) != 0)
		*q++ = *++p;
	} else if (*p == '"') {
	    // string quotation
	    if (in_token)
		*q++ = 1; // end of string
	    else
		*q++ = *p; // start of string - copy "
	    in_token = !in_token;
	} else if (*p == '%' && !in_token) {
	    // comment till eol
	    for (; *p != 0; p++)
		if (*p == '\n')
		    break;
	}  

	else if (*p <= ' ' && !in_token) {
	    // mark this as whitespace
	    *q++ = 1;
	} else if (*p == '|' && !in_token) {
	    // mark this as concat
	    *q++ = 2;
	} else if (*p == '+' && !in_token) {
	    // mark this as add
	    *q++ = 3;
	} else if (*p == '-' && !in_token) {
	    // mark this as sub
	    *q++ = 4;
	} else if (*p == '*' && !in_token) {
	    // mark this as mult
	    *q++ = 5;
	} else if (*p == '=' && !in_token) {
	    // mark this as div
	    *q++ = 6;
	} else if (*p == '>' && !in_token) {
	    // mark this as gt
	    *q++ = 7;
	} else if (*p == '(' && !in_token) {
	    // mark this as bracket open
	    *q++ = 8;
	} else if (*p == ')' && !in_token) {
	    // mark this as bracket close
	    *q++ = 9;
	} else if (*p == ',' && !in_token) {
	    // mark this as colon
	    *q++ = 10;
	} else {
	    *q++ = *p;
	}
    }
    *q = 0;

    // eliminate double whitespace, detect operators and count tokens
    lang_debug("lexxer whitespaces\r\n");

    in_token = false;
    for (p = q = str; *p != 0; p++) {
	if (*p < ' ') {
	    // it is a whitespace or an operator
	    if (*p == 1 && in_token) {
		// it is a whitespace
		*q++ = *p;
		in_token = false;
	    }
	    if (*p > 1) {
		// it is an operator
		*q++ = *p;
		token_count++;
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

    // resize (shrink) the mem needed for the script
    lang_debug("prog_len compact: %d\r\n", (q-str)+1);
    my_script = (char *)os_realloc(my_script, (q-str)+5);
    str = &my_script[4];

    my_token = (char **)os_malloc(token_count * sizeof(char *));
    if (my_token == 0)
	return 0;

    // assign tokens
    lang_debug("lexxer tokenize\r\n");

    in_token = false;
    token_count = 0;
    for (p = str; *p != 0; p++) {
	if (*p == 1) {
	    *p = '\0';
	    in_token = false;
	} 
	else if (*p == 2) {
	    my_token[token_count++] = "|";
	    *p = '\0';
	    in_token = false;
	}
	else if (*p == 3) {
	    my_token[token_count++] = "+";
	    *p = '\0';
	    in_token = false;
	}
	else if (*p == 4) {
	    my_token[token_count++] = "-";
	    *p = '\0';
	    in_token = false;
	}
	else if (*p == 5) {
	    my_token[token_count++] = "*";
	    *p = '\0';
	    in_token = false;
	}
	else if (*p == 6) {
	    my_token[token_count++] = "=";
	    *p = '\0';
	    in_token = false;
	}
	else if (*p == 7) {
	    my_token[token_count++] = ">";
	    *p = '\0';
	    in_token = false;
	}
	else if (*p == 8) {
	    my_token[token_count++] = "(";
	    *p = '\0';
	    in_token = false;
	}
	else if (*p == 9) {
	    my_token[token_count++] = ")";
	    *p = '\0';
	    in_token = false;
	}
	else if (*p == 10) {
	    my_token[token_count++] = ",";
	    *p = '\0';
	    in_token = false;
	}
	else {
	    if (!in_token) {
		my_token[token_count++] = p;
		in_token = true;
	    }
	}
    }

    max_token = token_count;
    return max_token;
}

void ICACHE_FLASH_ATTR free_tokens(void) {
    if (my_token != NULL)
	os_free((uint32_t *) my_token);
    my_token = NULL;
}

bool ICACHE_FLASH_ATTR is_token(int i, char *s) {
    if (i >= max_token)
	return false;
//os_printf("cmp: %s %s\r\n", s, my_token[i]);
    return os_strcmp(my_token[i], s) == 0;
}

int ICACHE_FLASH_ATTR search_token(int i, char *s) {
    for (; i < max_token; i++)
	if (is_token(i, s))
	    return i;
    return max_token;
}

int ICACHE_FLASH_ATTR syntax_error(int i, char *message) {
    int j;

    os_sprintf(tmp_buffer, "Error (%s) at >>", message);
    for (j = i; j < i + 8 && j < max_token; j++) {
	int pos = os_strlen(tmp_buffer);
	if (sizeof(tmp_buffer) - pos - 2 > os_strlen(my_token[j])) {
	    os_sprintf(tmp_buffer + pos, "%s ", my_token[j]);
	}
    }
    return -1;
}

int ICACHE_FLASH_ATTR parse_statement(int next_token) {
    bool event_happened;
    int on_token;

    uint32_t start = system_get_time();

    while ((next_token = syn_chk ? next_token : search_token(next_token, "on")) < max_token) {

	in_topic_statement = false;
#ifdef GPIO
	in_gpio_statement = false;
#endif
#ifdef HTTPC
	in_http_statement = false;
#endif

	if (is_token(next_token, "on")) {
	    lang_debug("statement on\r\n");

	    if ((next_token = parse_event(next_token + 1, &event_happened)) == -1)
		return -1;
	    if (!syn_chk && !event_happened)
		continue;

	    if (syn_chk && !is_token(next_token, "do"))
		return syntax_error(next_token, "'do' expected");
	    if ((next_token = parse_action(next_token + 1, event_happened)) == -1)
		return -1;
	} else if (is_token(next_token, "config")) {
	    next_token += 3;
	} else {
	    return syntax_error(next_token, "'on' or 'config' expected");
	}
    }

    lang_debug("Interpreter loop: %d us\r\n", (system_get_time()-start));
    if (interpreter_status == INIT)
	loop_time = system_get_time()-start;
    else
	loop_time = (loop_time * 7 + (system_get_time()-start)) / 8;

    return next_token;
}

int ICACHE_FLASH_ATTR parse_event(int next_token, bool * happend) {
    *happend = false;

    if (is_token(next_token, "init")) {
	lang_debug("event init\r\n");

	*happend = (interpreter_status == INIT);
	if (*happend)
	    lang_log("on init\r\n");
	return next_token + 1;
    }

    if (is_token(next_token, "mqttconnect")) {
	lang_debug("event mqttconnect\r\n");

	*happend = (interpreter_status == MQTT_CLIENT_CONNECT);
	if (*happend)
	    lang_log("on mqttconnect\r\n");
	return next_token + 1;
    }

    if (is_token(next_token, "wificonnect")) {
	lang_debug("event wificonnect\r\n");

	*happend = (interpreter_status == WIFI_CONNECT);
	if (*happend)
	    lang_log("on wificonnect\r\n");
	return next_token + 1;
    }

    if (is_token(next_token, "topic")) {
	char *topic;
	int topic_len;
	Value_Type topic_type;
	int lr_token = next_token + 1;

	lang_debug("event topic\r\n");
	in_topic_statement = true;

	len_check(2);
	if ((next_token = parse_value(next_token + 2, &topic, &topic_len, &topic_type)) == -1)
	    return -1;

	if (is_token(lr_token, "remote")) {
	    if (interpreter_status != TOPIC_REMOTE)
		return next_token;
	} else if (is_token(lr_token, "local")) {
	    if (interpreter_status != TOPIC_LOCAL)
		return next_token;
	} else {
	    return syntax_error(next_token + 1, "'local' or 'remote' expected");
	}

	*happend = Topics_matches(topic, true, interpreter_topic);

	if (*happend)
	    lang_log("on topic %s %s matched %s\r\n", my_token[lr_token],
		      topic, interpreter_topic);

	return next_token;
    }

    if (is_token(next_token, "timer")) {
	lang_debug("event timer\r\n");

	len_check(1);
	uint32_t timer_no = atoi(my_token[next_token + 1]);
	if (timer_no == 0 || timer_no > MAX_TIMERS)
	    return syntax_error(next_token + 1, "invalid timer number");
	if (interpreter_status == TIMER && interpreter_timer == --timer_no) {
	    lang_log("on timer %s\r\n", my_token[next_token + 1]);
	    *happend = true;
	}
	return next_token + 2;
    }

    if (is_token(next_token, "alarm")) {
	lang_debug("event alarm\r\n");

	len_check(1);
	uint32_t timer_no = atoi(my_token[next_token + 1]);
	if (timer_no == 0 || timer_no > MAX_TIMESTAMPS)
	    return syntax_error(next_token + 1, "invalid alarm number");
	if (interpreter_status == ALARM && interpreter_timestamp == --timer_no) {
	    lang_log("on alarm %s\r\n", my_token[next_token + 1]);
	    *happend = true;
	}
	return next_token + 2;
    }
#ifdef GPIO
    if (is_token(next_token, "gpio_interrupt")) {
	lang_debug("event gpio\r\n");

	in_gpio_statement = true;
	len_check(2);
	uint32_t gpio_no = atoi(my_token[next_token + 1]);

	if (syn_chk) {
	    if (gpio_no > 16)
		return syntax_error(next_token + 1, "invalid gpio number");
#ifdef GPIO_PWM
	    if (pwm_channel_from_pin(gpio_no) != -1)
		return syntax_error(next_token, "pin defined as pwm before");	
#endif
	    if (!is_token(next_token+2, "pullup") && !is_token(next_token+2, "nopullup"))
		return syntax_error(next_token + 2, "expected 'pullup' or 'nopullup'");
	    int pullup = is_token(next_token+2, "pullup") ? EASYGPIO_PULLUP : EASYGPIO_NOPULL;
	    if (gpio_counter >= MAX_GPIOS)
		return syntax_error(next_token, "too many gpio_interrupt");
	    gpios[gpio_counter].no = gpio_no;
	    easygpio_pinMode(gpio_no, pullup, EASYGPIO_INPUT);
	    easygpio_attachInterrupt(gpio_no, pullup, gpio_intr_handler, &gpios[gpio_counter]);

	    gpio_counter++;
	}
	if (interpreter_status == GPIO_INT && interpreter_gpio == gpio_no) {
	    lang_log("on gpio_interrupt %s\r\n", my_token[next_token + 1]);
	    *happend = true;
	}
	return next_token + 3;
    }
#endif
#ifdef HTTPC
    if (is_token(next_token, "http_response")) {
	lang_debug("event http_response\r\n");
	in_http_statement = true;

	*happend = (interpreter_status == HTTP_RESPONSE);
	if (*happend)
	    lang_log("on http_response\r\n");
	return next_token + 1;
    }
#endif
    return syntax_error(next_token, "'init', 'mqttconnect', 'topic', 'gpio_interrupt', 'alarm', 'http_response', or 'timer' expected");
}

int ICACHE_FLASH_ATTR parse_action(int next_token, bool doit) {

    while (next_token < max_token && !is_token(next_token, "on")
	   && !is_token(next_token, "config") && !is_token(next_token, "else")
	   && !is_token(next_token, "endif")) {
	bool is_nl = false;

	if (doit) {
	    lang_debug("action %s\r\n", my_token[next_token]);
	}
	//os_printf("action %s %s\r\n", my_token[next_token], doit ? "do" : "ignore");

	if ((is_nl = is_token(next_token, "println")) || is_token(next_token, "print")) {
	    char *p_char;
	    int p_len;
	    Value_Type p_type;

	    len_check(1);
	    if ((next_token = parse_expression(next_token + 1, &p_char, &p_len, &p_type, doit)) == -1)
		return -1;
	    if (doit) {
		con_print(p_char);
		if (is_nl)
		    con_print("\r\n");
	    }
	}

	else if (is_token(next_token, "system")) {
	    char *p_char;
	    int p_len;
	    Value_Type p_type;

	    len_check(1);
	    if ((next_token = parse_expression(next_token + 1, &p_char, &p_len, &p_type, doit)) == -1)
		return -1;
	    if (doit) {
		lang_log("system '%s'\r\n", p_char);
		do_command(p_char, "", "");
	    }
	}

	else if (is_token(next_token, "publish")) {
	    bool retained = false;
	    char *data;
	    int data_len;
	    Value_Type data_type;
	    char *topic;
	    int topic_len;
	    Value_Type topic_type;
	    int lr_token = next_token + 1;

	    len_check(3);
	    if ((next_token = parse_value(next_token + 2, &topic, &topic_len, &topic_type)) == -1)
		return -1;
	    if ((next_token = parse_expression(next_token, &data, &data_len, &data_type, doit)) == -1)
		return -1;
	    if (next_token < max_token && is_token(next_token, "retained")) {
		retained = true;
		next_token++;
	    }

	    if (doit) {
		if (topic_type != STRING_T || Topics_hasWildcards(topic)) {
		    os_printf("invalid topic string\r\n");
		    return next_token;
		}
	    }

#ifdef MQTT_CLIENT
	    if (is_token(lr_token, "remote")) {
		if (doit && mqtt_connected) {
		    if (data_type == STRING_T) {
		    	lang_log("publish remote %s %s\r\n", topic, data);
		    } else {
			lang_log("publish remote %s binary (%d bytes)\r\n", topic, data_len);
		    }
		    MQTT_Publish(&mqttClient, topic, data, data_len, 0, retained);
		}
	    } else
#endif
	    if (is_token(lr_token, "local")) {
		if (doit) {
		    if (data_type == STRING_T) {
		    	lang_log("publish local %s %s\r\n", topic, data);
		    } else {
			lang_log("publish local %s binary (%d bytes)\r\n", topic, data_len);
		    }
		    MQTT_local_publish(topic, data, data_len, 0, retained);
		}
	    } else {
		return syntax_error(lr_token, "'local' or 'remote' expected");
	    }
	}

	else if (is_token(next_token, "subscribe")) {
	    char *topic;
	    int topic_len;
	    Value_Type topic_type;
	    bool retval;
	    int rl_token = next_token + 1;

	    len_check(2);
	    if ((next_token = parse_value(next_token + 2, &topic, &topic_len, &topic_type)) == -1)
		return -1;
#ifdef MQTT_CLIENT
	    if (is_token(rl_token, "remote")) {
		if (doit && mqtt_connected) {
		    lang_log("subscribe remote %s\r\n", topic);
		    retval = MQTT_Subscribe(&mqttClient, topic, 0);
		}
	    } else 
#endif
	    if (is_token(rl_token, "local")) {
		if (doit) {
		    lang_log("subscribe local %s\r\n", topic);
		    retval = MQTT_local_subscribe(topic, 0);
		}
	    } else {
		return syntax_error(next_token + 1, "'local' or 'remote' expected");
	    }
	}

	else if (is_token(next_token, "unsubscribe")) {
	    char *topic;
	    int topic_len;
	    Value_Type topic_type;
	    bool retval;
	    int rl_token = next_token + 1;

	    len_check(2);
	    if ((next_token = parse_value(next_token + 2, &topic, &topic_len, &topic_type)) == -1)
		return -1;
#ifdef MQTT_CLIENT
	    if (is_token(rl_token, "remote")) {
		if (doit && mqtt_connected) {
		    lang_log("unsubscribe remote %s\r\n", topic);
		    retval = MQTT_UnSubscribe(&mqttClient, topic);
		}
	    } else
#endif
	    if (is_token(rl_token, "local")) {
		if (doit) {
		    lang_log("subscribe local %s\r\n", topic);
		    retval = MQTT_local_unsubscribe(topic);
		}
	    } else {
		return syntax_error(next_token + 1, "'local' or 'remote' expected");
	    }
	}

	else if (is_token(next_token, "if")) {
	    uint32_t if_val;
	    char *if_char;
	    int if_len;
	    Value_Type if_type;
	    int exp_token;

	    len_check(3);
	    exp_token = next_token + 1;
	    if ((next_token = parse_expression(next_token + 1, &if_char, &if_len, &if_type, doit)) == -1)
		return -1;
	    if (syn_chk && !is_token(next_token, "then"))
		return syntax_error(next_token, "'then' expected");

	    if (doit) {
		if_val = atoi(if_char);
		if (if_val != 0)
		    lang_log("if %s %s... (done) \r\n", my_token[exp_token++], my_token[exp_token]);
	    }
	    if ((next_token = parse_action(next_token + 1, doit && if_val != 0)) == -1)
		return -1;
	    if (is_token(next_token, "else")) {
		if (doit && if_val == 0)
		    lang_log("if %s %s... else (done) \r\n", my_token[exp_token++], my_token[exp_token]);
		if ((next_token = parse_action(next_token + 1, doit && if_val == 0)) == -1)
		    return -1;
		if (syn_chk && !is_token(next_token - 1, "endif"))
		    return syntax_error(next_token - 1, "'endif' expected");
	    }
	}

	else if (is_token(next_token, "settimer")) {
	    len_check(2);
	    uint32_t timer_no = atoi(my_token[next_token + 1]);
	    if (timer_no == 0 || timer_no > MAX_TIMERS)
		return syntax_error(next_token + 1, "invalid timer number");

	    uint32_t timer_val;
	    char *timer_char;
	    int timer_len;
	    Value_Type timer_type;
	    if ((next_token = parse_expression(next_token + 2, &timer_char, &timer_len, &timer_type, doit)) == -1)
		return -1;

	    if (doit) {
		timer_val = atoi(timer_char);
		lang_log("settimer %d %d\r\n", timer_no, timer_val);

		timer_no--;
		os_timer_disarm(&timers[timer_no]);
		if (timer_val != 0) {
		    os_timer_setfn(&timers[timer_no], (os_timer_func_t *) lang_timers_timeout, (void *)timer_no);
		    os_timer_arm(&timers[timer_no], timer_val, 0);
		}
	    }
	}

	else if (is_token(next_token, "setalarm")) {
	    len_check(2);
	    uint32_t alarm_no = atoi(my_token[next_token + 1]);
	    if (alarm_no == 0 || alarm_no > MAX_TIMESTAMPS)
		return syntax_error(next_token + 1, "invalid alarm number");

	    char *timer_char;
	    int timer_len;
	    Value_Type timer_type;
	    if ((next_token = parse_expression(next_token + 2, &timer_char, &timer_len, &timer_type, doit)) == -1)
		return -1;

	    if (doit) {
		lang_log("setalarm %d %d\r\n", alarm_no, timer_char);

		alarm_no--;
		os_strncpy(timestamps[alarm_no].ts, timer_char, 9);
		timestamps[alarm_no].state = UNDEFINED;
	    }
	}

	else if (is_token(next_token, "setvar")) {
	    len_check(3);
	    bool flash_var = false;
	    uint32_t slot_no;
	    var_entry_t *this_var, *free_var;
	    uint8_t *var_id = my_token[next_token + 1];

	    if (var_id[0] == '@') {
		slot_no = atoi(&var_id[1]);
		if (syn_chk && (slot_no == 0 || slot_no > MAX_FLASH_SLOTS))
		    return syntax_error(next_token + 1, "invalid flash var number");
		flash_var = true;
	    }

	    else if (var_id[0] == '$') {
		if (syn_chk && var_id[1] == '\0')
		    return syntax_error(next_token, "invalid var identifier");

		this_var = find_var(&var_id[1], &free_var);
		if (this_var == NULL) {
		    if (free_var == NULL)
			return syntax_error(next_token, "too many vars used");

		    this_var = free_var;
		    this_var->free = 0;
		    os_strncpy(this_var->name, &var_id[1], 14);
		    this_var->name[14] = '\0';
		    this_var->data = (uint8_t *)os_malloc(DEFAULT_VAR_LEN);
		    this_var->buffer_len = DEFAULT_VAR_LEN;
		}
	    } else {
		return syntax_error(next_token, "invalid var identifier");
	    }

	    if (syn_chk && os_strcmp(my_token[next_token + 2], "=") != 0)
		return syntax_error(next_token + 2, "'=' expected");

	    char *var_data;
	    int var_len;
	    Value_Type var_type;
	    if ((next_token = parse_expression(next_token + 3, &var_data, &var_len, &var_type, doit)) == -1)
		return -1;

	    if (doit) {
		if (var_type == STRING_T) {
		    lang_log("setvar %s = %s\r\n", var_id, var_data);
		} else {
		    lang_log("setvar %s = binary (%d bytes)\r\n", var_id, var_len);
		}
		lang_debug("setvar $%s\r\n", var_id);

		if (flash_var) {
		    if (var_len > FLASH_SLOT_LEN)
			var_len = FLASH_SLOT_LEN;

		    slot_no--;
		    uint8_t slots[MAX_FLASH_SLOTS*FLASH_SLOT_LEN];
		    blob_load(1, (uint32_t *)slots, sizeof(slots));
		    os_memcpy(&slots[slot_no*FLASH_SLOT_LEN], var_data, var_len);
		    slots[slot_no*FLASH_SLOT_LEN+FLASH_SLOT_LEN-1] = '\0';
		    blob_save(1, (uint32_t *)slots, sizeof(slots));
		} else {

		    if (var_len > this_var->buffer_len - 1) {
			os_free(this_var->data);
			this_var->data = (uint8_t *)os_malloc(var_len+1);
			this_var->buffer_len = var_len+1;
			if (this_var->data == NULL) {
			    os_printf("Out of mem for var $%s\r\n", this_var->name);
			    this_var->data = (uint8_t *)os_malloc(DEFAULT_VAR_LEN);
			    this_var->buffer_len = DEFAULT_VAR_LEN;
			    return next_token;
			}
		    }
		    os_memcpy(this_var->data, var_data, var_len);
		    this_var->data[var_len] = '\0';
		    this_var->data_len = var_len;
		    this_var->data_type = var_type;
		}
	    }
	}
#ifdef HTTPC
	else if (is_token(next_token, "http_get")) {
	    len_check(1);

	    char *url_data;
	    int url_len;
	    Value_Type url_type;
	    if ((next_token = parse_expression(next_token + 1, &url_data, &url_len, &url_type, doit)) == -1)
		return -1;

	    if (doit) {
		http_get(url_data, "", interpreter_http_reply);
	    }
	}

	else if (is_token(next_token, "http_post")) {
	    len_check(2);

	    char *url_data;
	    int url_len;
	    Value_Type url_type;
	    if ((next_token = parse_expression(next_token + 1, &url_data, &url_len, &url_type, doit)) == -1)
		return -1;

	    // Copy, in case it is in tmp_buffer
	    if (!doit)
		url_len = 0;
	    char post_url[url_len+1];
	    os_memcpy(post_url, url_data, url_len);
	    post_url[url_len] = '\0';

	    char *post_data;
	    int post_len;
	    Value_Type post_type;
	    if ((next_token = parse_expression(next_token + 1, &post_data, &post_len, &post_type, doit)) == -1)
		return -1;

	    if (doit) {
		http_post(post_url, post_data, "", interpreter_http_reply);
	    }
	}
#endif
#ifdef GPIO
	else if (is_token(next_token, "gpio_pinmode")) {
	    len_check(2);

	    uint32_t gpio_no = atoi(my_token[next_token + 1]);
	    if (gpio_no > 16)
		return syntax_error(next_token + 1, "invalid gpio number");
#ifdef GPIO_PWM
	    if (syn_chk && pwm_channel_from_pin(gpio_no) != -1)
		return syntax_error(next_token, "pin defined as pwm before");	
#endif
	    int pullup = EASYGPIO_NOPULL;
	    int inout = EASYGPIO_OUTPUT;
	    if (is_token(next_token+2, "input")) {
		inout = EASYGPIO_INPUT;
		if (is_token(next_token+3, "pullup")) {
		    pullup = EASYGPIO_PULLUP;
		    next_token++;
		}
	    } else if (syn_chk && !is_token(next_token+2, "output")) {
		return syntax_error(next_token + 2, "expected 'input' or 'output'");	
	    }

	    if (doit) {
		lang_log("gpio_pinmode %d %s\r\n", gpio_no, inout == EASYGPIO_INPUT ? "input" : "output");
		easygpio_pinMode(gpio_no, pullup, inout);
	    }
	    next_token += 3;
	}

	else if (is_token(next_token, "gpio_out")) {
	    len_check(2);

	    uint32_t gpio_no = atoi(my_token[next_token + 1]);
	    if (gpio_no > 16)
		return syntax_error(next_token + 1, "invalid gpio number");

	    char *gpio_data;
	    int gpio_len;
	    Value_Type gpio_type;
	    if ((next_token = parse_expression(next_token + 2, &gpio_data, &gpio_len, &gpio_type, doit)) == -1)
		return -1;

	    if (doit) {
		lang_log("gpio_out %d %d\r\n", gpio_no, atoi(gpio_data) != 0);
		if (easygpio_pinMode(gpio_no, EASYGPIO_NOPULL, EASYGPIO_OUTPUT))
		    easygpio_outputSet(gpio_no, atoi(gpio_data) != 0);
	    }
	}
#ifdef GPIO_PWM
	else if (is_token(next_token, "gpio_pwm")) {
	    len_check(1);

	    uint32_t gpio_no = atoi(my_token[next_token + 1]);
	    if (gpio_no > 16)
		return syntax_error(next_token + 1, "invalid gpio number");
	    if (syn_chk && pwm_channel_from_pin(gpio_no) == -1) {
		if (pwm_counter >= PWM_MAX_CHANNELS)
		    return syntax_error(next_token, "too many pwm channels");
		pwm_channels[pwm_counter] = gpio_no;
		pwm_counter++;
	    }

	    char *pwm_data;
	    int pwm_len;
	    Value_Type pwm_type;
	    if ((next_token = parse_expression(next_token + 2, &pwm_data, &pwm_len, &pwm_type, doit)) == -1)
		return -1;

	    uint32_t pwm_channel = pwm_channel_from_pin(gpio_no);

	    if (doit && pwm_channel != -1) {
		lang_log("gpio_pwm %d %s\r\n", gpio_no, pwm_data);
		pwm_set_duty((atoi(pwm_data)*config.pwm_period)/1000, pwm_channel);
		pwm_start();  
	    }
	}
#endif
#endif
	else
	    return syntax_error(next_token, "action command expected");

    }

    if (is_token(next_token, "endif"))
	next_token++;

    return next_token;
}

bool ICACHE_FLASH_ATTR retained_cb(retained_entry *topic, void *user_data) {
    *(retained_entry **)user_data = topic;
    return true;
}

int ICACHE_FLASH_ATTR parse_expression(int next_token, char **data, int *data_len, Value_Type * data_type, bool doit) {

    if (is_token(next_token, "not")) {
	lang_debug("expr not\r\n");

	len_check(3);
	if (syn_chk && !is_token(next_token+1, "("))
	    return syntax_error(next_token, "expected '('");
	if ((next_token = parse_expression(next_token + 2, data, data_len, data_type, doit)) == -1)
	    return -1;
	if (syn_chk && !is_token(next_token, ")"))
	    return syntax_error(next_token, "expected ')'");

	next_token++;
	*data = atoi(*data) ? "0" : "1";
	*data_len = 1;
	*data_type = STRING_T;
    }

    else if (is_token(next_token, "retained_topic")) {
	lang_debug("val retained_topic\r\n");

	len_check(3);
	if (syn_chk && !is_token(next_token+1, "("))
	    return syntax_error(next_token, "expected '('");

	char *topic_data;
	int topic_data_len;
	Value_Type topic_data_type;
	// parse path string
	if ((next_token = parse_expression(next_token + 2, &topic_data, &topic_data_len, &topic_data_type, doit)) == -1)
	    return -1;

	if (syn_chk && !is_token(next_token, ")"))
	    return syntax_error(next_token, "expected ')'");
	next_token += 1;

	*data = "";
	*data_len = 0;
	*data_type = DATA_T;
	if (doit) {
	    retained_entry *retained_entry_p;
	    if (find_retainedtopic(topic_data, retained_cb, &retained_entry_p)) {
		*data_len = retained_entry_p->data_len > sizeof(tmp_buffer)-1? sizeof(tmp_buffer)-1 : retained_entry_p->data_len;
		os_memcpy(tmp_buffer, retained_entry_p->data, *data_len);
		tmp_buffer[*data_len] = '\0';
		*data = tmp_buffer;
	    }
	}
    }
    else if (is_token(next_token, "substr")) {
	lang_debug("val substr\r\n");

	len_check(7);
	if (syn_chk && !is_token(next_token+1, "("))
	    return syntax_error(next_token+1, "expected '('");

	char *str_data;
	int str_data_len;
	Value_Type str_data_type;
	// parse path string
	if ((next_token = parse_expression(next_token + 2, &str_data, &str_data_len, &str_data_type, doit)) == -1)
	    return -1;
	if (!doit)
	    str_data_len = 0;
	char str[str_data_len+1];
	if (doit)
	    os_strcpy(str, str_data);

	if (syn_chk && !is_token(next_token, ","))
	    return syntax_error(next_token, "expected ','");
	next_token++;

	int16_t from = atoi(my_token[next_token]);
	// if as string const
	if (my_token[next_token][0] == '"')
	    from = atoi(&my_token[next_token][1]);
	next_token++;

	if (syn_chk && !is_token(next_token, ","))
	    return syntax_error(next_token, "expected ','");
	next_token++;

	uint16_t len = atoi(my_token[next_token]);
	next_token++;

	if (syn_chk && !is_token(next_token, ")"))
	    return syntax_error(next_token, "expected ')'");
	next_token++;

	if (doit) {
	    if (from < 0) {
		from = str_data_len+from;
		if (from < 0)
		    from = 0;
	    }

	    if (len > sizeof(tmp_buffer)-1)
		len = sizeof(tmp_buffer)-1;

	    os_strncpy(tmp_buffer, &str[from], len);
	    tmp_buffer[len] = '\0';

	    *data_len = os_strlen(tmp_buffer);
	    *data = tmp_buffer;
	    *data_type = STRING_T;
	}
    }
#ifdef GPIO
    else if (is_token(next_token, "gpio_in")) {
	lang_debug("val gpio_in\r\n");

	len_check(3);
	if (syn_chk && !is_token(next_token+1, "("))
	    return syntax_error(next_token, "expected '('");

	uint32_t gpio_no = atoi(my_token[next_token + 2]);
	if (gpio_no > 16)
	    return syntax_error(next_token+2, "invalid gpio number");

	if (syn_chk && !is_token(next_token+3, ")"))
	    return syntax_error(next_token+3, "expected ')'");

	next_token += 4;
	*data = "0";
	*data_len = 1;
	*data_type = STRING_T;
	if (doit && easygpio_inputGet(gpio_no)) {
	    *data = "1";
	}
    }
#endif
#ifdef JSON_PARSE
    else if (is_token(next_token, "json_parse")) {
	lang_debug("val json_parse\r\n");

	len_check(5);
	if (syn_chk && !is_token(next_token+1, "("))
	    return syntax_error(next_token+1, "expected '('");

	char *path_data;
	int path_data_len;
	Value_Type path_data_type;
	// parse path string
	if ((next_token = parse_expression(next_token + 2, &path_data, &path_data_len, &path_data_type, doit)) == -1)
	    return -1;
	if (!doit)
	    path_data_len = 0;
	char path[path_data_len+1];
	if (doit)
	    os_strcpy(path, path_data);

	if (syn_chk && !is_token(next_token, ","))
	    return syntax_error(next_token, "expected ','");

	char *json_data;
	int json_data_len;
	Value_Type json_data_type;
	// parse json string
	if ((next_token = parse_expression(next_token + 1, &json_data, &json_data_len, &json_data_type, doit)) == -1)
	    return -1;
	if (!doit)
	    json_data_len = 0;
	char json[json_data_len+1];
	if (doit)
	    os_strcpy(json, json_data);

	if (syn_chk && !is_token(next_token, ")"))
	    return syntax_error(next_token, "expected ')'");

	next_token += 1;

	if (doit) {
	    *data_len = sizeof(tmp_buffer);
	    json_path(json, path, tmp_buffer, data_len);
	    *data = tmp_buffer;
	    *data_type = STRING_T;
	}
    }
#endif
    else if (is_token(next_token, "(")) {
	lang_debug("expr (\r\n");

	len_check(2);
	if ((next_token = parse_expression(next_token + 1, data, data_len, data_type, doit)) == -1)
	    return -1;

	if (!is_token(next_token, ")"))
	    return syntax_error(next_token, "expected ')'");
	next_token++;
    }

    else {
	if ((next_token = parse_value(next_token, data, data_len, data_type)) == -1)
	    return -1;
    }

    // if it is not some kind of binary operation - finished
    if (!is_token(next_token, "=") 
	&& !is_token(next_token, "+")
	&& !is_token(next_token, "-")
	&& !is_token(next_token, "*")
	&& !is_token(next_token, "div")
	&& !is_token(next_token, "|")
	&& !is_token(next_token, ">")
	&& !is_token(next_token, "gte")
	&& !is_token(next_token, "str_gt")
	&& !is_token(next_token, "str_gte"))
	return next_token;

    // okay, it is an operation
    int op = next_token;

    char *r_data;
    int r_data_len;
    Value_Type r_data_type;

    // parse second operand
    if ((next_token = parse_expression(next_token + 1, &r_data, &r_data_len, &r_data_type, doit)) == -1)
	return -1;
    //os_printf("l:%s(%d) r:%s(%d)\r\n", *data, *data_len, r_data, r_data_len);

    if (!doit)
	return next_token;

    *data_type = STRING_T;
    if (is_token(op, "=")) {
	*data = os_strcmp(*data, r_data) ? "0" : "1";
    } else if (is_token(op, "+")) {
	 os_sprintf(tmp_buffer, "%d", atoi(*data) + atoi(r_data));
	 *data = tmp_buffer;
	 *data_len = os_strlen(tmp_buffer);
    } else if (is_token(op, "-")) {
	os_sprintf(tmp_buffer, "%d", atoi(*data) - atoi(r_data));
	*data = tmp_buffer;
	*data_len = os_strlen(tmp_buffer);
    } else if (is_token(op, "*")) {
	os_sprintf(tmp_buffer, "%d", atoi(*data) * atoi(r_data));
	*data = tmp_buffer;
	*data_len = os_strlen(tmp_buffer);
    } else if (is_token(op, "div")) {
	os_sprintf(tmp_buffer, "%d", atoi(*data) / atoi(r_data));
	*data = tmp_buffer;
	*data_len = os_strlen(tmp_buffer);
    } else if (is_token(op, "|")) {
	uint16_t len = os_strlen(*data) + os_strlen(r_data);
	char catbuf[len+1];
	os_sprintf(catbuf, "%s%s", *data, r_data);
	if (len > sizeof(tmp_buffer)-1) {
	    len = sizeof(tmp_buffer);
	    catbuf[len] = '\0';
	}
	*data_len = len;
	os_memcpy(tmp_buffer, catbuf, *data_len + 1);
	*data = tmp_buffer;
    } else if (is_token(op, ">")) {
	*data = atoi(*data) > atoi(r_data) ? "1" : "0";
	*data_len = 1;
    } else if (is_token(op, "gte")) {
	*data = atoi(*data) >= atoi(r_data) ? "1" : "0";
	*data_len = 1;
    } else if (is_token(op, "str_gt")) {
	*data = os_strcmp(*data, r_data) > 0 ? "1" : "0";
	*data_len = 1;
    } else if (is_token(op, "str_gte")) {
	*data = os_strcmp(*data, r_data) >= 0 ? "1" : "0";
	*data_len = 1;
    }

    return next_token;
}

int ICACHE_FLASH_ATTR parse_value(int next_token, char **data, int *data_len, Value_Type * data_type) {
    if (my_token[next_token][0] == '"') {
	lang_debug("val str(%s)\r\n", &my_token[next_token][1]);

	*data = &my_token[next_token][1];
	*data_len = os_strlen(&my_token[next_token][1]);
	*data_type = STRING_T;
	return next_token + 1;
    }

    else if (my_token[next_token][0] == '#') {
	lang_debug("val hexbinary\r\n");

	// Convert it in place to binary once during syntax check
	// Format: Byte 0: '#', Byte 1: len (max 255), Byte 2 to len: binary data, Byte len+1: 0
	if (syn_chk) {
	    int i, j, len = os_strlen(my_token[next_token])-1;
	    uint8_t a, *p = &(my_token[next_token][1]);

	    if (len == 0 || len % 2)
		return syntax_error(next_token, "number of hexdigits must be multiple of 2");
	    if (len > 511)
		return syntax_error(next_token, "hexbinary too long");
	    for (i = 0, j = 1; i < len; i += 2, j++) {
		if (p[i] <= '9')
		    a = p[i] - '0';
		else
		    a = toupper(p[i]) - 'A' + 10;
		a <<= 4;
		if (p[i + 1] <= '9')
		    a += p[i + 1] - '0';
		else
		    a += toupper(p[i + 1]) - 'A' + 10;
		p[j] = a;
	    }
	    p[j] = '\0';
	    p[0] = (uint8_t) j - 1;
	}

	*data = &my_token[next_token][2];
	*data_len = my_token[next_token][1];
	*data_type = DATA_T;

	return next_token + 1;
    }

    else if (is_token(next_token, "$this_data")) {
	lang_debug("val $this_data\r\n");

	if (!in_topic_statement)
	    return syntax_error(next_token, "undefined $this_data");
	*data = interpreter_data;
	*data_len = interpreter_data_len;
	*data_type = DATA_T;
	return next_token + 1;
    }

    else if (is_token(next_token, "$this_topic")) {
	lang_debug("val $this_topic\r\n");

	if (!in_topic_statement)
	    return syntax_error(next_token, "undefined $this_topic");
	*data = interpreter_topic;
	*data_len = os_strlen(interpreter_topic);
	*data_type = STRING_T;
	return next_token + 1;
    }
#ifdef GPIO
    else if (is_token(next_token, "$this_gpio")) {
	lang_debug("val $this_gpio\r\n");

	if (!in_gpio_statement)
	    return syntax_error(next_token, "undefined $this_gpio");
	*data = interpreter_gpioval == 0 ? "0" : "1";
	*data_len = 1;
	*data_type = STRING_T;
	return next_token + 1;
    }
#endif
#ifdef HTTPC
    else if (is_token(next_token, "$this_http_body")) {
	lang_debug("val $this_http_body\r\n");

	if (!in_http_statement)
	    return syntax_error(next_token, "undefined $this_http_body");
	*data = interpreter_data;
	*data_len = interpreter_data_len;
	*data_type = STRING_T;
	return next_token + 1;
    }

    else if (is_token(next_token, "$this_http_code")) {
	static char codebuf[4];
	lang_debug("val $this_http_code\r\n");

	if (!in_http_statement)
	    return syntax_error(next_token, "undefined $this_http_code");
	
	os_sprintf(codebuf, "%3d", interpreter_http_status);
	*data = codebuf;
	*data_len = os_strlen(codebuf);
	*data_type = STRING_T;
	return next_token + 1;
    }
#endif
#ifdef NTP
    else if (is_token(next_token, "$timestamp")) {
	lang_debug("val $timestamp\r\n");

	if (ntp_sync_done())
	    *data = get_timestr();
	else
	    *data = "99:99:99";
	*data_len = os_strlen(*data);
	*data_type = STRING_T;
	return next_token + 1;
    }

    else if (is_token(next_token, "$weekday")) {
	lang_debug("val $weekday\r\n");

	if (ntp_sync_done())
	    *data = get_weekday();
	else
	    *data = "xxx";
	*data_len = os_strlen(*data);
	*data_type = STRING_T;
	return next_token + 1;
    }
#endif
#ifdef ADC
    else if (is_token(next_token, "$adc")) {
	static char adcbuf[8];
	lang_debug("val $adc\r\n");

	os_sprintf(adcbuf, "%d", adc_read());
	*data = adcbuf;
	*data_len = os_strlen(*data);
	*data_type = STRING_T;
	return next_token + 1;
    }
#endif
    else if (my_token[next_token][0] == '$' && my_token[next_token][1] != '\0') {
	lang_debug("val var %s\r\n", &(my_token[next_token][1]));

	var_entry_t *this_var, *free_var;

	this_var = find_var(&(my_token[next_token][1]), &free_var);
	if (this_var == NULL)
	    return syntax_error(next_token, "unknown var name");

	*data = this_var->data;
	*data_len = this_var->data_len;
	*data_type = this_var->data_type;
	return next_token + 1;
    }

    else if (my_token[next_token][0] == '@' && my_token[next_token][1] != '\0') {
	lang_debug("val flashvar %s\r\n", my_token[next_token]);

	uint32_t slot_no = atoi(&my_token[next_token][1]);
	if (slot_no == 0 || slot_no > MAX_FLASH_SLOTS)
	    return syntax_error(next_token+2, "invalid flash slot number");

	*data = "0";
	*data_len = 1;
	*data_type = STRING_T;

	if (!syn_chk) {
	    slot_no--;
	    uint8_t slots[MAX_FLASH_SLOTS*FLASH_SLOT_LEN];
	    blob_load(1, (uint32_t *)slots, sizeof(slots));
	    os_memcpy(tmp_buffer, &slots[slot_no*FLASH_SLOT_LEN], FLASH_SLOT_LEN);
	    *data = tmp_buffer;
	    *data_len = os_strlen(tmp_buffer);
	}
	return next_token + 1;
    }

    else {
	lang_debug("val num/str(%s)\r\n", my_token[next_token]);

	*data = my_token[next_token];
	*data_len = os_strlen(my_token[next_token]);
	*data_type = STRING_T;
	return next_token + 1;
    }
}

int ICACHE_FLASH_ATTR interpreter_syntax_check() {
    lang_debug("interpreter_syntax_check\r\n");

    int i;
    for (i = 0; i<MAX_VARS; i++) {
	vars[i].free = 1;
	vars[i].data = "";//(uint8_t *)os_malloc(MAX_VAR_LEN);
	vars[i].data_len = 0;
    }

    os_sprintf(tmp_buffer, "Syntax okay");
    interpreter_status = SYNTAX_CHECK;
    interpreter_topic = interpreter_data = "";
    interpreter_data_len = 0;
    os_bzero(&timestamps, sizeof(timestamps));
#ifdef GPIO
    gpio_counter = 0;
#ifdef GPIO_PWM
    pwm_counter = 0;
#endif
#endif
    return parse_statement(0);
}

int ICACHE_FLASH_ATTR interpreter_config() {
    int next_token = 0;

    while ((next_token = search_token(next_token, "config")) < max_token) {
	lang_debug("statement config\r\n");
	uint8_t *val;

	len_check(2);

	if (my_token[next_token + 2][0] == '@') {
	    uint32_t slot_no = atoi(&my_token[next_token + 2][1]);
	    if (slot_no == 0 || slot_no > MAX_FLASH_SLOTS)
		return syntax_error(next_token + 1, "invalid flash slot number");

	    slot_no--;
	    uint8_t slots[MAX_FLASH_SLOTS*FLASH_SLOT_LEN];
	    blob_load(1, (uint32_t *)slots, sizeof(slots));
	    val = &slots[slot_no*FLASH_SLOT_LEN];
	    if (val[0] == '\0')
		val = "_undefined_";
	} else {
	    val = my_token[next_token + 2];
	}

	do_command("set", my_token[next_token + 1], val);
	next_token += 3;
    }
    return next_token;
}

int ICACHE_FLASH_ATTR interpreter_init() {
    if (!script_enabled)
	return -1;

    lang_debug("interpreter_init\r\n");
#ifdef GPIO
#ifdef GPIO_PWM
    init_pwm();
#endif
#endif
    interpreter_status = INIT;
    interpreter_topic = interpreter_data = "";
    interpreter_data_len = 0;
    int ret_val = parse_statement(0);
#ifdef GPIO
    init_gpios();
#endif

    return ret_val;
}

int ICACHE_FLASH_ATTR interpreter_wifi_connect(void) {
    if (!script_enabled)
	return -1;

    lang_debug("interpreter_wifi_connect\r\n");

    interpreter_status = WIFI_CONNECT;
    interpreter_topic = interpreter_data = "";
    interpreter_data_len = 0;
    return parse_statement(0);
}

int ICACHE_FLASH_ATTR interpreter_mqtt_connect(void) {
    if (!script_enabled)
	return -1;

    lang_debug("interpreter_mqtt_connect\r\n");

    interpreter_status = MQTT_CLIENT_CONNECT;
    interpreter_topic = interpreter_data = "";
    interpreter_data_len = 0;
    return parse_statement(0);
}

int ICACHE_FLASH_ATTR interpreter_topic_received(const char *topic, const char *data, int data_len, bool local) {
    if (!script_enabled)
	return -1;

    uint8_t data_null[data_len+1];
    os_memcpy(data_null, data, data_len);
    data_null[data_len] = '\0';

    lang_debug("interpreter_topic_received\r\n");

    interpreter_status = (local) ? TOPIC_LOCAL : TOPIC_REMOTE;
    interpreter_topic = (char *)topic;
    interpreter_data = data_null;
    interpreter_data_len = data_len;

    return parse_statement(0);
}

#ifdef HTTPC
void ICACHE_FLASH_ATTR interpreter_http_reply(char *response_body, int http_status, char *response_headers, int body_size) {
    if (!script_enabled)
	return;

    lang_debug("interpreter_http_reply\r\n");

    interpreter_status = HTTP_RESPONSE;
    interpreter_topic = response_headers;
    interpreter_http_status = http_status;
    interpreter_data = response_body;
    interpreter_data_len = body_size;

    parse_statement(0);
}
#endif

